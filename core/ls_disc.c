/*
 * LAN peer discovery shared by the PC and Android front ends.
 *
 * Wire format (one UDP datagram, UTF-8):
 *   LSDISC1|id=<hex>|port=<tcp port>|q=<0|1>|name=<device name>\n
 *
 * - On start, one beacon is broadcast to every interface broadcast address.
 * - A peer that receives a q=1 query answers once (per-source rate limited).
 * - ls_disc_scan() broadcasts a query and sweeps the local /24 by unicast,
 *   so it also works on APs that drop broadcast traffic.
 */

#include "ls_disc.h"
#include "ls_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <process.h>
typedef SOCKET ls_dock_t;
typedef HANDLE ls_thread_t;
typedef CRITICAL_SECTION ls_mutex_t;
#define LS_DOCK_INVALID INVALID_SOCKET
#define LS_DOCK_ERRNO WSAGetLastError()
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
typedef int ls_dock_t;
typedef pthread_t ls_thread_t;
typedef pthread_mutex_t ls_mutex_t;
#define LS_DOCK_INVALID (-1)
#define LS_DOCK_ERRNO errno
#endif

#define DISC_TAG        "LSDISC1|"
#define DISC_TAG_LEN    8
#define DISC_RECV_MS    300
#define DISC_REPLY_MS   2000
#define DISC_REPLY_SLOTS 16
#define DISC_RESEND_MS  800
#define DISC_FRESH_MS   10000
#define DISC_NAME_MAX   63

typedef struct {
    char id[40];
    char name[64];
    int  port;
    int  q;
} disc_beacon;

static ls_mutex_t g_mu;
static atomic_int g_started;
static atomic_int g_running;
static ls_dock_t  g_sock = LS_DOCK_INVALID;
static ls_thread_t g_thread;
static int        g_thread_started;
static int        g_udp_port;
static int        g_tcp_port;
static char       g_name[DISC_NAME_MAX + 1];
static char       g_id[40];
static atomic_int g_scanning;

static ls_peer_info g_peers[LS_DISC_MAX_PEERS];
static int          g_peer_count;

static struct {
    char ip[64];
    long long last_ms;
} g_replied[DISC_REPLY_SLOTS];
static int g_replied_next;

static void mu_init(ls_mutex_t *m)
{
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}

static void mu_destroy(ls_mutex_t *m)
{
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}

static void mu_lock(ls_mutex_t *m)
{
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}

static void mu_unlock(ls_mutex_t *m)
{
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void sock_close(ls_dock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/* Replaces characters that would break the line-oriented beacon format. */
static void copy_name(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (cap == 0)
        return;
    for (; src && *src && o + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '|' || c == '\r' || c == '\n' || c < 0x20)
            dst[o++] = ' ';
        else
            dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
}

static void make_id(void)
{
    unsigned long long v;
    struct timespec ts;
#ifdef _WIN32
    v = (unsigned long long)GetTickCount64() * 2654435761ull;
    v ^= (unsigned long long)GetCurrentProcessId() << 40;
#else
    v = (unsigned long long)now_ms() * 2654435761ull;
    v ^= (unsigned long long)getpid() << 40;
#endif
    clock_gettime(CLOCK_REALTIME, &ts);
    v ^= (unsigned long long)ts.tv_nsec << 16;
    v ^= (unsigned long long)(uintptr_t)&v;
    snprintf(g_id, sizeof(g_id), "%08x%06x",
             (unsigned)(v & 0xffffffffull), (unsigned)((v >> 32) & 0xffffffull));
}

static int build_beacon(int query, char *out, size_t cap)
{
    int n = snprintf(out, cap, DISC_TAG "id=%s|port=%d|q=%d|name=%s\n",
                     g_id, g_tcp_port, query ? 1 : 0, g_name);
    if (n <= 0 || (size_t)n >= cap)
        return -1;
    return n;
}

static int parse_beacon(const char *buf, disc_beacon *b)
{
    const char *p;
    if (!buf || strncmp(buf, DISC_TAG, DISC_TAG_LEN) != 0)
        return -1;
    memset(b, 0, sizeof(*b));
    p = buf + DISC_TAG_LEN;
    while (*p && *p != '\r' && *p != '\n') {
        const char *eq = strchr(p, '=');
        const char *end;
        size_t klen, vlen;
        if (!eq)
            return -1;
        end = strchr(p, '|');
        if (!end)
            end = p + strlen(p);
        if (eq > end)
            return -1;
        klen = (size_t)(eq - p);
        vlen = (size_t)(end - eq - 1);
        if (klen == 2 && strncmp(p, "id", 2) == 0) {
            if (vlen >= sizeof(b->id))
                vlen = sizeof(b->id) - 1;
            memcpy(b->id, eq + 1, vlen);
            b->id[vlen] = '\0';
        } else if (klen == 4 && strncmp(p, "port", 4) == 0) {
            b->port = atoi(eq + 1);
        } else if (klen == 1 && *p == 'q') {
            b->q = atoi(eq + 1);
        } else if (klen == 4 && strncmp(p, "name", 4) == 0) {
            if (vlen >= sizeof(b->name))
                vlen = sizeof(b->name) - 1;
            memcpy(b->name, eq + 1, vlen);
            b->name[vlen] = '\0';
        }
        p = *end ? end + 1 : end;
    }
    {
        size_t ln = strlen(b->name);
        while (ln > 0 && (b->name[ln - 1] == '\r' || b->name[ln - 1] == '\n'))
            b->name[--ln] = '\0';
    }
    return b->id[0] ? 0 : -1;
}

/* Records a peer; caller must not hold the lock. */
static void peers_update(const char *ip, const disc_beacon *b)
{
    int i, slot = -1, is_new = 0;
    long long now = now_ms();
    mu_lock(&g_mu);
    for (i = 0; i < g_peer_count; i++) {
        if (strcmp(g_peers[i].id, b->id) == 0 ||
            strcmp(g_peers[i].ip, ip) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && g_peer_count < LS_DISC_MAX_PEERS) {
        slot = g_peer_count++;
        is_new = 1;
    }
    if (slot >= 0) {
        snprintf(g_peers[slot].ip, sizeof(g_peers[slot].ip), "%s", ip);
        snprintf(g_peers[slot].name, sizeof(g_peers[slot].name), "%s",
                 b->name[0] ? b->name : ip);
        snprintf(g_peers[slot].id, sizeof(g_peers[slot].id), "%s", b->id);
        g_peers[slot].tcp_port = b->port;
        g_peers[slot].last_ms = now;
    }
    mu_unlock(&g_mu);
    if (is_new)
        ls_log_printf("[disc] peer %s name='%s' tcp=%d", ip, b->name, b->port);
}

static int should_reply(const char *ip)
{
    int i;
    int ok = 1;
    long long now = now_ms();
    mu_lock(&g_mu);
    for (i = 0; i < DISC_REPLY_SLOTS; i++) {
        if (g_replied[i].ip[0] && strcmp(g_replied[i].ip, ip) == 0) {
            if (now - g_replied[i].last_ms < DISC_REPLY_MS)
                ok = 0;
            else
                g_replied[i].last_ms = now;
            break;
        }
    }
    if (i == DISC_REPLY_SLOTS) {
        snprintf(g_replied[g_replied_next].ip,
                 sizeof(g_replied[g_replied_next].ip), "%s", ip);
        g_replied[g_replied_next].last_ms = now;
        g_replied_next = (g_replied_next + 1) % DISC_REPLY_SLOTS;
    }
    mu_unlock(&g_mu);
    return ok;
}

#ifdef _WIN32
#define DISC_MAX_IFACES 16
typedef struct {
    char ip[64];
    char bcast[64];
    char prefix[16];    /* first three octets, empty when not private */
} disc_iface;

static int collect_ifaces(disc_iface *out, int cap)
{
    MIB_IPADDRTABLE *tab = NULL;
    ULONG size = 0;
    DWORD rc;
    int n = 0;
    DWORD i;
    if (GetIpAddrTable(NULL, &size, 0) == ERROR_INSUFFICIENT_BUFFER) {
        tab = (MIB_IPADDRTABLE *)malloc(size);
        if (!tab)
            return 0;
        if (GetIpAddrTable(tab, &size, 0) != NO_ERROR) {
            free(tab);
            return 0;
        }
    }
    if (!tab)
        return 0;
    for (i = 0; i < tab->dwNumEntries && n < cap; i++) {
        DWORD addr = tab->table[i].dwAddr;
        DWORD mask = tab->table[i].dwMask;
        unsigned char o[4], m[4];
        if (addr == 0 || mask == 0)
            continue;
        o[0] = (unsigned char)(addr & 0xff);
        o[1] = (unsigned char)((addr >> 8) & 0xff);
        o[2] = (unsigned char)((addr >> 16) & 0xff);
        o[3] = (unsigned char)((addr >> 24) & 0xff);
        m[0] = (unsigned char)(mask & 0xff);
        m[1] = (unsigned char)((mask >> 8) & 0xff);
        m[2] = (unsigned char)((mask >> 16) & 0xff);
        m[3] = (unsigned char)((mask >> 24) & 0xff);
        if (o[0] == 127 || (o[0] == 169 && o[1] == 254))
            continue;
        snprintf(out[n].ip, sizeof(out[n].ip), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
        snprintf(out[n].bcast, sizeof(out[n].bcast), "%u.%u.%u.%u",
                 o[0] | (unsigned char)~m[0], o[1] | (unsigned char)~m[1],
                 o[2] | (unsigned char)~m[2], o[3] | (unsigned char)~m[3]);
        {
            int priv = o[0] == 10 || (o[0] == 192 && o[1] == 168) ||
                       (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
            if (priv)
                snprintf(out[n].prefix, sizeof(out[n].prefix), "%u.%u.%u", o[0], o[1], o[2]);
            else
                out[n].prefix[0] = '\0';
        }
        n++;
    }
    free(tab);
    (void)rc;
    return n;
}
#else
#define DISC_MAX_IFACES 16
typedef struct {
    char ip[64];
    char bcast[64];
    char prefix[16];
} disc_iface;

static int collect_ifaces(disc_iface *out, int cap)
{
    struct ifaddrs *ifs = NULL, *it;
    int n = 0;
    if (getifaddrs(&ifs) != 0)
        return 0;
    for (it = ifs; it && n < cap; it = it->ifa_next) {
        struct sockaddr_in *sin;
        unsigned char o[4], m[4];
        unsigned int mask;
        int priv;
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK))
            continue;
        if (!(it->ifa_flags & IFF_BROADCAST))
            continue;
        sin = (struct sockaddr_in *)it->ifa_addr;
        memcpy(o, &sin->sin_addr.s_addr, 4);
        if (o[0] == 0 || o[0] == 127 || (o[0] == 169 && o[1] == 254))
            continue;
        mask = 0xffffff00u;
        if (it->ifa_netmask)
            memcpy(&mask, &((struct sockaddr_in *)it->ifa_netmask)->sin_addr.s_addr, 4);
        mask = ntohl(mask);
        m[0] = (unsigned char)(mask >> 24);
        m[1] = (unsigned char)(mask >> 16);
        m[2] = (unsigned char)(mask >> 8);
        m[3] = (unsigned char)mask;
        snprintf(out[n].ip, sizeof(out[n].ip), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
        snprintf(out[n].bcast, sizeof(out[n].bcast), "%u.%u.%u.%u",
                 o[0] | (unsigned char)~m[0], o[1] | (unsigned char)~m[1],
                 o[2] | (unsigned char)~m[2], o[3] | (unsigned char)~m[3]);
        priv = o[0] == 10 || (o[0] == 192 && o[1] == 168) ||
               (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
        if (priv)
            snprintf(out[n].prefix, sizeof(out[n].prefix), "%u.%u.%u", o[0], o[1], o[2]);
        else
            out[n].prefix[0] = '\0';
        n++;
    }
    freeifaddrs(ifs);
    return n;
}
#endif

/* Broadcast one beacon to 255.255.255.255 and every interface broadcast. */
static void send_broadcast_beacon(int query)
{
    disc_iface ifs[DISC_MAX_IFACES];
    int nif, i;
    char payload[256];
    int len = build_beacon(query, payload, sizeof(payload));
    struct sockaddr_in a;
    if (len <= 0 || g_sock == LS_DOCK_INVALID)
        return;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_udp_port);
    a.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(g_sock, payload, len, 0, (struct sockaddr *)&a, (socklen_t)sizeof(a));
    nif = collect_ifaces(ifs, DISC_MAX_IFACES);
    for (i = 0; i < nif; i++) {
        struct in_addr b;
        if (inet_pton(AF_INET, ifs[i].bcast, &b) != 1)
            continue;
        a.sin_addr = b;
        sendto(g_sock, payload, len, 0, (struct sockaddr *)&a, (socklen_t)sizeof(a));
    }
}

/* Unicast the same query to <prefix>.1-254, skipping our own address. */
static void sweep_subnet(const char *prefix, const char *own_ip)
{
    char payload[256];
    struct sockaddr_in a;
    int len = build_beacon(1, payload, sizeof(payload));
    int i, sent = 0;
    if (len <= 0)
        return;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_udp_port);
    for (i = 1; i <= 254; i++) {
        char host[32];
        snprintf(host, sizeof(host), "%s.%d", prefix, i);
        if (own_ip && strcmp(host, own_ip) == 0)
            continue;
        if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
            continue;
        sendto(g_sock, payload, len, 0, (struct sockaddr *)&a, (socklen_t)sizeof(a));
        if (++sent % 16 == 0)
            sleep_ms(8);
    }
}

static void scan_sweep_all(void)
{
    disc_iface ifs[DISC_MAX_IFACES];
    int nif = collect_ifaces(ifs, DISC_MAX_IFACES);
    int i;
    for (i = 0; i < nif; i++) {
        if (ifs[i].prefix[0])
            sweep_subnet(ifs[i].prefix, ifs[i].ip);
    }
}

/* Most recent peer heard at or after since_ms. Returns 0 and fills out. */
static int peer_since(long long since_ms, ls_peer_info *out)
{
    int i, best = -1;
    long long best_ms = 0;
    mu_lock(&g_mu);
    for (i = 0; i < g_peer_count; i++) {
        if (g_peers[i].last_ms >= since_ms && g_peers[i].last_ms >= best_ms) {
            best = i;
            best_ms = g_peers[i].last_ms;
        }
    }
    if (best >= 0 && out)
        *out = g_peers[best];
    mu_unlock(&g_mu);
    return best >= 0 ? 0 : -1;
}

static void recv_loop(void)
{
    char buf[512];
    while (atomic_load(&g_running)) {
        struct sockaddr_in ra;
        socklen_t rl = (socklen_t)sizeof(ra);
        int n;
        disc_beacon b;
        char ip[64];

        memset(&ra, 0, sizeof(ra));
        n = (int)recvfrom(g_sock, buf, (int)sizeof(buf) - 1, 0,
                          (struct sockaddr *)&ra, &rl);
        if (n <= 0) {
#ifdef _WIN32
            if (LS_DOCK_ERRNO == WSAETIMEDOUT)
                continue;
            if (!atomic_load(&g_running))
                break;
            sleep_ms(20);
            continue;
#else
            if (LS_DOCK_ERRNO == EAGAIN || LS_DOCK_ERRNO == EWOULDBLOCK ||
                LS_DOCK_ERRNO == EINTR)
                continue;
            if (!atomic_load(&g_running))
                break;
            sleep_ms(20);
            continue;
#endif
        }
        buf[n] = '\0';
        if (parse_beacon(buf, &b) != 0)
            continue;
        if (strcmp(b.id, g_id) == 0)
            continue;
        if (!inet_ntop(AF_INET, &ra.sin_addr, ip, sizeof(ip)))
            continue;
        if (strncmp(ip, "127.", 4) == 0 || strcmp(ip, "0.0.0.0") == 0)
            continue;
        peers_update(ip, &b);
        if (b.q == 1 && should_reply(ip)) {
            char payload[256];
            int len = build_beacon(0, payload, sizeof(payload));
            if (len > 0)
                sendto(g_sock, payload, len, 0,
                       (struct sockaddr *)&ra, (socklen_t)sizeof(ra));
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall disc_thread(void *arg)
{
    (void)arg;
    recv_loop();
    return 0;
}
#else
static void *disc_thread(void *arg)
{
    (void)arg;
    recv_loop();
    return NULL;
}
#endif

int ls_disc_start(int udp_port, int tcp_port, const char *name)
{
    struct sockaddr_in a;
    int one = 1;

    if (atomic_load(&g_started))
        return 0;
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return -1;
    }
#endif
    g_udp_port = udp_port > 0 ? udp_port : LS_DISC_DEFAULT_PORT;
    g_tcp_port = tcp_port > 0 ? tcp_port : 5201;
    copy_name(name && name[0] ? name : "device", g_name, sizeof(g_name));
    make_id();
    mu_init(&g_mu);
    g_peer_count = 0;
    memset(g_replied, 0, sizeof(g_replied));
    g_replied_next = 0;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == LS_DOCK_INVALID) {
        mu_destroy(&g_mu);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
#ifdef _WIN32
    {
        DWORD tmo = DISC_RECV_MS;
        setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    }
#else
    {
    struct timeval tv;
    tv.tv_sec = DISC_RECV_MS / 1000;
    tv.tv_usec = (DISC_RECV_MS % 1000) * 1000;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_udp_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0) {
        sock_close(g_sock);
        g_sock = LS_DOCK_INVALID;
        mu_destroy(&g_mu);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    atomic_store(&g_running, 1);
    atomic_store(&g_scanning, 0);
#ifdef _WIN32
    {
        uintptr_t th = _beginthreadex(NULL, 0, disc_thread, NULL, 0, NULL);
        if (!th) {
            atomic_store(&g_running, 0);
            sock_close(g_sock);
            g_sock = LS_DOCK_INVALID;
            mu_destroy(&g_mu);
            WSACleanup();
            return -1;
        }
        g_thread = (HANDLE)th;
        g_thread_started = 1;
    }
#else
    if (pthread_create(&g_thread, NULL, disc_thread, NULL) != 0) {
        atomic_store(&g_running, 0);
        sock_close(g_sock);
        g_sock = LS_DOCK_INVALID;
        mu_destroy(&g_mu);
        return -1;
    }
    g_thread_started = 1;
#endif
    atomic_store(&g_started, 1);
    send_broadcast_beacon(0);
    ls_log_printf("[disc] started udp=%d tcp=%d id=%s name='%s'",
                  g_udp_port, g_tcp_port, g_id, g_name);
    return 0;
}

void ls_disc_stop(void)
{
    if (!atomic_load(&g_started))
        return;
    atomic_store(&g_started, 0);
    atomic_store(&g_running, 0);
    if (g_thread_started) {
#ifdef _WIN32
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
#else
        pthread_join(g_thread, NULL);
#endif
        g_thread_started = 0;
    }
    if (g_sock != LS_DOCK_INVALID) {
        sock_close(g_sock);
        g_sock = LS_DOCK_INVALID;
    }
    mu_destroy(&g_mu);
#ifdef _WIN32
    WSACleanup();
#endif
}

int ls_disc_scan(int timeout_ms, ls_peer_info *out)
{
    long long start, deadline, next_broadcast;
    int swept = 0, retry_done = 0;

    if (!atomic_load(&g_started) || g_sock == LS_DOCK_INVALID)
        return -1;
    if (out)
        memset(out, 0, sizeof(*out));

    /* A beacon heard moments ago is good enough: no need to sweep again. */
    if (peer_since(now_ms() - DISC_FRESH_MS, out) == 0)
        return 0;

    if (atomic_exchange(&g_scanning, 1) != 0)
        return peer_since(0, out);

    if (timeout_ms < 300)
        timeout_ms = 300;
    start = now_ms();
    deadline = start + timeout_ms;
    next_broadcast = start;

    while (atomic_load(&g_running) && now_ms() < deadline) {
        long long now = now_ms();
        if (now >= next_broadcast) {
            send_broadcast_beacon(1);
            next_broadcast = now + DISC_RESEND_MS;
        }
        if (!swept) {
            scan_sweep_all();
            swept = 1;
        } else if (!retry_done && now - start >= timeout_ms / 2) {
            retry_done = 1;
            scan_sweep_all();
        }
        if (peer_since(start, out) == 0) {
            atomic_store(&g_scanning, 0);
            ls_log_printf("[disc] scan found %s ('%s') after %lld ms",
                          out->ip, out->name, now_ms() - start);
            return 0;
        }
        sleep_ms(40);
    }
    atomic_store(&g_scanning, 0);
    if (peer_since(start, out) == 0) {
        ls_log_printf("[disc] scan found %s ('%s') at deadline", out->ip, out->name);
        return 0;
    }
    ls_log_printf("[disc] scan %d ms: no peer", timeout_ms);
    return -1;
}

int ls_disc_latest(ls_peer_info *out)
{
    if (!atomic_load(&g_started))
        return -1;
    return peer_since(0, out);
}

int ls_disc_peer_count(void)
{
    int n;
    if (!atomic_load(&g_started))
        return 0;
    mu_lock(&g_mu);
    n = g_peer_count;
    mu_unlock(&g_mu);
    return n;
}

int ls_disc_self_id(char *out, size_t len)
{
    if (!atomic_load(&g_started) || !out || len == 0)
        return -1;
    snprintf(out, len, "%s", g_id);
    return 0;
}

int ls_disc_peer_at(int index, ls_peer_info *out)
{
    int ok = -1;
    if (!atomic_load(&g_started) || !out)
        return -1;
    mu_lock(&g_mu);
    if (index >= 0 && index < g_peer_count) {
        *out = g_peers[index];
        ok = 0;
    }
    mu_unlock(&g_mu);
    return ok;
}
