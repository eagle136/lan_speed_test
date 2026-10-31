#include "ls_core.h"

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
#include <process.h>
#include <malloc.h>
typedef SOCKET ls_sock_t;
typedef HANDLE ls_thread_t;
typedef CRITICAL_SECTION ls_mutex_t;
typedef CONDITION_VARIABLE ls_cond_t;
#define LS_INVALID_SOCK INVALID_SOCKET
#define LS_SOCK_ERRNO WSAGetLastError()
#define LS_NFDS(fd) 0
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
typedef int ls_sock_t;
typedef pthread_t ls_thread_t;
typedef pthread_mutex_t ls_mutex_t;
typedef pthread_cond_t ls_cond_t;
#define LS_INVALID_SOCK (-1)
#define LS_SOCK_ERRNO errno
#define LS_NFDS(fd) ((int)(fd) + 1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define LS_MAGIC        0x4C535431u   /* "LST1" */
#define LS_HELLO_MAGIC  0x4C534831u   /* "LSH1" */
#define LS_VERSION      1

#define LS_FRAME_HELLO  1
#define LS_FRAME_START  2
#define LS_FRAME_END    3
#define LS_FRAME_DATA   4
#define LS_FRAME_DONE   5

#define LS_PATTERN_A    0xA5u
#define LS_PATTERN_B    0x5Au

#define LS_STALL_NS     10000000000ull  /* 10 s without progress */
#define LS_RATE_WINDOW  200000000ull    /* 200 ms raw window */
#define LS_RATE_SAMPLES 8
#define LS_GAP_LOG_NS   50000000ull     /* log intervals above 50 ms */

static atomic_int g_log_on = 0;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  role;
    uint16_t version;
    uint32_t seconds;
    uint32_t reserved;
} ls_hello_t;

typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  phase;
    uint16_t reserved;
    uint32_t seq;
    uint32_t value;
} ls_frame_hdr_t;

typedef struct {
    uint64_t bytes;
    uint64_t ns;
} ls_result_t;
#pragma pack(pop)

_Static_assert(sizeof(ls_hello_t) == 16, "hello size");
_Static_assert(sizeof(ls_frame_hdr_t) == 16, "frame header size");
_Static_assert(sizeof(ls_result_t) == 16, "result size");

struct ls_session {
    int role;
    int port;
    int seconds;
    int pipeline;
    struct in_addr peer_addr;
    char cfg_peer_ip[64];

    ls_thread_t thread;
    int thread_started;
    atomic_int stop;

    ls_sock_t sock;
    ls_sock_t listen_sock;

    ls_mutex_t mu;
    ls_status st;
};

/* ---------- logging ---------- */

#ifdef _WIN32
static FILE *g_log_file;
static void log_write_line(const char *line)
{
    if (!g_log_file) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        char *p;
        size_t len;
        if (n == 0 || n >= MAX_PATH)
            return;
        p = strrchr(path, '\\');
        if (!p)
            return;
        p[1] = '\0';
        len = strlen(path);
        if (len + 16 >= MAX_PATH)
            return;
        strcpy(path + len, "lanspeed_log.txt");
        g_log_file = fopen(path, "a");
    }
    if (g_log_file) {
        fprintf(g_log_file, "%s\n", line);
        fflush(g_log_file);
    }
}
#endif

static void ls_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (!atomic_load_explicit(&g_log_on, memory_order_relaxed))
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LanSpeedCore", "%s", buf);
#elif defined(_WIN32)
    log_write_line(buf);
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

void ls_set_log_enabled(int enabled)
{
    atomic_store(&g_log_on, enabled ? 1 : 0);
}

void ls_log_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ls_log("%s", buf);
}

static void log_wall_stamp(const char *what)
{
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    ls_log("=== %04d-%02d-%02d %02d:%02d:%02d %s ===",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec, what);
}

static unsigned long tid_now(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

static uint64_t thread_cpu_ns(void)
{
#ifdef _WIN32
    FILETIME ct, et, kt, ut;
    ULARGE_INTEGER k, u;
    if (!GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut))
        return 0;
    k.LowPart = kt.dwLowDateTime;
    k.HighPart = kt.dwHighDateTime;
    u.LowPart = ut.dwLowDateTime;
    u.HighPart = ut.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100ull;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------- live-rate window ---------- */

typedef struct {
    uint64_t t[LS_RATE_SAMPLES];
    uint64_t b[LS_RATE_SAMPLES];
    int n;
    int head;
    uint64_t last_t;
    uint64_t last_b;
} live_rate_t;

static void live_reset(live_rate_t *lr, uint64_t now, uint64_t bytes)
{
    memset(lr, 0, sizeof(*lr));
    lr->last_t = now;
    lr->last_b = bytes;
}

static double live_sample(live_rate_t *lr, uint64_t now, uint64_t bytes,
                          double *raw_out)
{
    uint64_t dt = now - lr->last_t;
    uint64_t db = bytes - lr->last_b;
    double raw = dt ? (double)db * 8.0 * 1e9 / (double)dt / 1e6 : 0.0;
    double smooth;
    int oldest;

    lr->t[lr->head] = now;
    lr->b[lr->head] = bytes;
    lr->head = (lr->head + 1) % LS_RATE_SAMPLES;
    if (lr->n < LS_RATE_SAMPLES)
        lr->n++;
    if (lr->n > 6)
        lr->n = 6;

    oldest = (lr->head - lr->n + LS_RATE_SAMPLES) % LS_RATE_SAMPLES;
    dt = now - lr->t[oldest];
    db = bytes - lr->b[oldest];
    smooth = dt ? (double)db * 8.0 * 1e9 / (double)dt / 1e6 : raw;

    lr->last_t = now;
    lr->last_b = bytes;
    if (raw_out)
        *raw_out = raw;
    return smooth;
}

/* ---------- phase statistics ---------- */

typedef struct {
    uint64_t bytes;
    uint64_t frames;
    uint64_t calls;
    uint64_t partial;
    uint64_t sys_ns;
    uint64_t sys_max_ns;
    uint64_t prep_ns;
    uint64_t cpu_start_ns;
    uint64_t cpu_end_ns;
    uint64_t gaps;
    uint64_t gap_max_ns;
    uint64_t last_rx_ns;
    double win_min;
    double win_max;
    double win_sum;
    int win_n;
} phase_stats_t;

static void stats_init(phase_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->win_min = 1e18;
    st->cpu_start_ns = thread_cpu_ns();
}

static void stats_io(phase_stats_t *st, uint64_t dt)
{
    st->calls++;
    st->sys_ns += dt;
    if (dt > st->sys_max_ns)
        st->sys_max_ns = dt;
}

static void stats_window(phase_stats_t *st, double raw)
{
    if (raw < st->win_min)
        st->win_min = raw;
    if (raw > st->win_max)
        st->win_max = raw;
    st->win_sum += raw;
    st->win_n++;
}

static void log_sock_details(ls_session *s)
{
    int snd = 0, rcv = 0;
    socklen_t sl = (socklen_t)sizeof(snd);
    if (getsockopt(s->sock, SOL_SOCKET, SO_SNDBUF, (char *)&snd, &sl) != 0)
        snd = -1;
    sl = (socklen_t)sizeof(rcv);
    if (getsockopt(s->sock, SOL_SOCKET, SO_RCVBUF, (char *)&rcv, &sl) != 0)
        rcv = -1;
    ls_log("[sock] tid=%lu sndbuf=%d rcvbuf=%d", tid_now(), snd, rcv);
}

static void log_tcp_info(ls_session *s, const char *tag)
{
#if defined(__linux__)
    struct tcp_info ti;
    socklen_t len = (socklen_t)sizeof(ti);
    memset(&ti, 0, sizeof(ti));
    if (getsockopt(s->sock, IPPROTO_TCP, TCP_INFO, (char *)&ti, &len) == 0) {
        ls_log("[tcp] %s rtt_us=%u rttvar_us=%u cwnd=%u ssthresh=%u "
               "unacked=%u retrans=%u total_retrans=%u rcv_space=%u",
               tag,
               (unsigned)ti.tcpi_rtt, (unsigned)ti.tcpi_rttvar,
               (unsigned)ti.tcpi_snd_cwnd, (unsigned)ti.tcpi_snd_ssthresh,
               (unsigned)ti.tcpi_unacked, (unsigned)ti.tcpi_retransmits,
               (unsigned)ti.tcpi_total_retrans, (unsigned)ti.tcpi_rcv_space);
    }
#else
    (void)s;
    (void)tag;
#endif
}

/* ---------- small platform helpers ---------- */

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

static void cv_init(ls_cond_t *c)
{
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}

static void cv_destroy(ls_cond_t *c)
{
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(c);
#endif
}

static void cv_wait(ls_cond_t *c, ls_mutex_t *m)
{
#ifdef _WIN32
    SleepConditionVariableCS(c, m, INFINITE);
#else
    pthread_cond_wait(c, m);
#endif
}

static void cv_broadcast(ls_cond_t *c)
{
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}

static uint64_t now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER c;
    if (!inited) {
        QueryPerformanceFrequency(&freq);
        inited = 1;
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1000000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static int ensure_wsa(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static void sock_close(ls_sock_t fd)
{
    if (fd == LS_INVALID_SOCK)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

static int sock_timeout(int e)
{
#ifdef _WIN32
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == ETIMEDOUT;
#endif
}

static int sock_intr(int e)
{
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

static int sock_in_progress(int e)
{
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
    return e == EINPROGRESS || e == EALREADY || e == EINTR;
#endif
}

static void set_nonblock(ls_sock_t fd, int on)
{
#ifdef _WIN32
    u_long v = on ? 1u : 0u;
    ioctlsocket(fd, FIONBIO, &v);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return;
    if (on)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    else
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

static void addr_to_str(const struct sockaddr_in *a, char *out, size_t n)
{
#ifdef _WIN32
    inet_ntop(AF_INET, &a->sin_addr, out, n);
#else
    inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)n);
#endif
}

static void tune_sock(ls_sock_t fd)
{
    int one = 1;
    int buf = 4 * 1024 * 1024;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&buf, sizeof(buf));
#ifdef _WIN32
    {
        DWORD ms = 500;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif
}

static uint8_t *block_alloc(uint8_t pattern)
{
    uint8_t *p;
#ifdef _WIN32
    p = (uint8_t *)_aligned_malloc(LS_BLOCK_SIZE, 64);
#else
    p = NULL;
    if (posix_memalign((void **)&p, 64, LS_BLOCK_SIZE) != 0)
        p = NULL;
#endif
    if (p)
        memset(p, pattern, LS_BLOCK_SIZE);
    return p;
}

static void block_free(void *p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

/* ---------- status publishing ---------- */

static void st_phase(ls_session *s, int phase)
{
    mu_lock(&s->mu);
    s->st.phase = phase;
    s->st.cur_mbps = 0.0;
    s->st.cur_mbps_raw = 0.0;
    s->st.progress = 0.0;
    mu_unlock(&s->mu);
}

static void st_live(ls_session *s, int phase, double mbps, double raw,
                    double progress)
{
    if (progress < 0.0)
        progress = 0.0;
    if (progress > 1.0)
        progress = 1.0;
    mu_lock(&s->mu);
    s->st.phase = phase;
    s->st.cur_mbps = mbps;
    s->st.cur_mbps_raw = raw;
    s->st.progress = progress;
    mu_unlock(&s->mu);
}

static void st_info(ls_session *s, const char *fmt, ...)
{
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    mu_lock(&s->mu);
    snprintf(s->st.err, sizeof(s->st.err), "%s", b);
    mu_unlock(&s->mu);
}

static void st_fail(ls_session *s, const char *fmt, ...)
{
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    mu_lock(&s->mu);
    s->st.phase = LS_PHASE_ERROR;
    s->st.error = 1;
    s->st.running = 0;
    snprintf(s->st.err, sizeof(s->st.err), "%s", b);
    mu_unlock(&s->mu);
}

static double calc_mbps(uint64_t bytes, uint64_t ns)
{
    if (bytes == 0 || ns == 0)
        return 0.0;
    return (double)bytes * 8.0 * 1e9 / (double)ns / 1e6;
}

static void st_done(ls_session *s, int up_valid, uint64_t up_b, uint64_t up_ns,
                    int down_valid, uint64_t down_b, uint64_t down_ns,
                    const char *warning)
{
    mu_lock(&s->mu);
    s->st.phase = LS_PHASE_DONE;
    s->st.running = 0;
    s->st.finished = 1;
    s->st.progress = 1.0;
    s->st.cur_mbps = 0.0;
    s->st.cur_mbps_raw = 0.0;
    s->st.has_up = up_valid;
    s->st.has_down = down_valid;
    s->st.up_bytes = up_b;
    s->st.down_bytes = down_b;
    s->st.up_secs = (double)up_ns / 1e9;
    s->st.down_secs = (double)down_ns / 1e9;
    s->st.up_mbps = calc_mbps(up_b, up_ns);
    s->st.down_mbps = calc_mbps(down_b, down_ns);
    if (warning && warning[0])
        snprintf(s->st.err, sizeof(s->st.err), "%s", warning);
    else
        s->st.err[0] = '\0';
    mu_unlock(&s->mu);
}

static void st_connected(ls_session *s, ls_sock_t fd)
{
    struct sockaddr_in pa, la;
    socklen_t pl = (socklen_t)sizeof(pa);
    socklen_t ll = (socklen_t)sizeof(la);
    char peer[64] = "";
    char local[64] = "";
    if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0)
        addr_to_str(&pa, peer, sizeof(peer));
    if (getsockname(fd, (struct sockaddr *)&la, &ll) == 0)
        addr_to_str(&la, local, sizeof(local));
    mu_lock(&s->mu);
    snprintf(s->st.peer_ip, sizeof(s->st.peer_ip), "%s", peer);
    snprintf(s->st.local_ip, sizeof(s->st.local_ip), "%s", local);
    s->st.connected = 1;
    s->st.err[0] = '\0';
    mu_unlock(&s->mu);
}

/* ---------- socket I/O ---------- */

static int send_all(ls_session *s, const void *data, int len)
{
    const char *p = (const char *)data;
    uint64_t last = now_ns();
    while (len > 0) {
        int n;
        int e;
        int flags = 0;
        if (atomic_load(&s->stop))
            return -2;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        n = send(s->sock, p, len, flags);
        if (n > 0) {
            p += n;
            len -= n;
            last = now_ns();
            continue;
        }
        if (n == 0)
            return -1;
        e = LS_SOCK_ERRNO;
        if (sock_intr(e))
            continue;
        if (sock_timeout(e)) {
            if (now_ns() - last > LS_STALL_NS)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int recv_exact(ls_session *s, void *data, int len, uint64_t stall_ns)
{
    char *p = (char *)data;
    uint64_t last = now_ns();
    while (len > 0) {
        int n;
        int e;
        if (atomic_load(&s->stop))
            return -2;
        n = recv(s->sock, p, len, 0);
        if (n > 0) {
            p += n;
            len -= n;
            last = now_ns();
            continue;
        }
        if (n == 0)
            return -1;
        e = LS_SOCK_ERRNO;
        if (sock_intr(e))
            continue;
        if (sock_timeout(e)) {
            if (now_ns() - last > stall_ns)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* ---------- connection establishment ---------- */

static int connect_once(ls_session *s, ls_sock_t fd, const struct sockaddr_in *addr,
                        int timeout_ms)
{
    uint64_t deadline;
    int r;
    set_nonblock(fd, 1);
    r = connect(fd, (const struct sockaddr *)addr, (socklen_t)sizeof(*addr));
    if (r == 0) {
        set_nonblock(fd, 0);
        return 0;
    }
    if (!sock_in_progress(LS_SOCK_ERRNO))
        return -1;

    deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    while (!atomic_load(&s->stop) && now_ns() < deadline) {
        fd_set wfds;
        struct timeval tv;
        int pr;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        pr = select(LS_NFDS(fd), NULL, &wfds, NULL, &tv);
        if (pr > 0) {
            int soerr = 0;
            socklen_t sl = (socklen_t)sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) != 0)
                return -1;
            if (soerr == 0) {
                set_nonblock(fd, 0);
                return 0;
            }
            return -1;
        }
        if (pr < 0 && !sock_intr(LS_SOCK_ERRNO))
            return -1;
    }
    return -1;
}

static int client_connect(ls_session *s)
{
    for (;;) {
        struct sockaddr_in a;
        ls_sock_t fd;
        if (atomic_load(&s->stop))
            return -2;
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == LS_INVALID_SOCK) {
            st_fail(s, "socket() failed: %d", LS_SOCK_ERRNO);
            return -1;
        }
        tune_sock(fd);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((unsigned short)s->port);
        a.sin_addr = s->peer_addr;
        if (connect_once(s, fd, &a, 1500) == 0) {
            s->sock = fd;
            st_connected(s, fd);
            log_sock_details(s);
            return 0;
        }
        sock_close(fd);
        if (atomic_load(&s->stop))
            return -2;
        st_info(s, "connecting to %s:%d ...", s->cfg_peer_ip, s->port);
        sleep_ms(500);
    }
}

static int server_wait(ls_session *s)
{
    ls_sock_t lfd;
    int one = 1;
    struct sockaddr_in a;
    int rc = -1;

    lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd == LS_INVALID_SOCK) {
        st_fail(s, "socket() failed: %d", LS_SOCK_ERRNO);
        return -1;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)s->port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0) {
        st_fail(s, "bind port %d failed: %d", s->port, LS_SOCK_ERRNO);
        sock_close(lfd);
        return -1;
    }
    if (listen(lfd, 4) != 0) {
        st_fail(s, "listen() failed: %d", LS_SOCK_ERRNO);
        sock_close(lfd);
        return -1;
    }
    s->listen_sock = lfd;
    st_info(s, "listening on port %d, waiting for PC ...", s->port);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int pr;
        struct sockaddr_in ra;
        socklen_t rl = (socklen_t)sizeof(ra);
        ls_sock_t c;
        if (atomic_load(&s->stop)) {
            rc = -2;
            break;
        }
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        pr = select(LS_NFDS(lfd), &rfds, NULL, NULL, &tv);
        if (pr == 0)
            continue;
        if (pr < 0) {
            if (sock_intr(LS_SOCK_ERRNO))
                continue;
            st_fail(s, "select() failed: %d", LS_SOCK_ERRNO);
            break;
        }
        c = accept(lfd, (struct sockaddr *)&ra, &rl);
        if (c == LS_INVALID_SOCK)
            continue;
        if (ra.sin_addr.s_addr != s->peer_addr.s_addr) {
            char ip[64];
            addr_to_str(&ra, ip, sizeof(ip));
            st_info(s, "rejected connection from %s (expected %s)",
                    ip, s->cfg_peer_ip);
            sock_close(c);
            continue;
        }
        sock_close(lfd);
        s->listen_sock = LS_INVALID_SOCK;
        tune_sock(c);
        s->sock = c;
        st_connected(s, c);
        log_sock_details(s);
        rc = 0;
        break;
    }
    if (s->listen_sock != LS_INVALID_SOCK) {
        sock_close(s->listen_sock);
        s->listen_sock = LS_INVALID_SOCK;
    }
    return rc;
}

/* ---------- protocol ---------- */

static void frame_put(uint8_t *buf, uint8_t type, uint8_t phase,
                      uint32_t seq, uint32_t value)
{
    ls_frame_hdr_t *h = (ls_frame_hdr_t *)buf;
    h->magic = LS_MAGIC;
    h->type = type;
    h->phase = phase;
    h->reserved = 0;
    h->seq = seq;
    h->value = value;
}

static int send_control(ls_session *s, uint8_t *buf, uint8_t type,
                        uint8_t phase, uint32_t value)
{
    frame_put(buf, type, phase, 0, value);
    return send_all(s, buf, (int)LS_BLOCK_SIZE);
}

static int do_hello(ls_session *s)
{
    ls_hello_t h, r;
    int rc;
    memset(&h, 0, sizeof(h));
    h.magic = LS_HELLO_MAGIC;
    h.type = LS_FRAME_HELLO;
    h.role = (uint8_t)s->role;
    h.version = LS_VERSION;
    h.seconds = (uint32_t)s->seconds;
    rc = send_all(s, &h, (int)sizeof(h));
    if (rc != 0) {
        st_fail(s, "handshake send failed");
        return -1;
    }
    memset(&r, 0, sizeof(r));
    rc = recv_exact(s, &r, (int)sizeof(r), LS_STALL_NS);
    if (rc != 0) {
        st_fail(s, "handshake timeout");
        return -1;
    }
    if (r.magic != LS_HELLO_MAGIC || r.type != LS_FRAME_HELLO ||
        r.version != LS_VERSION) {
        st_fail(s, "handshake mismatch");
        return -1;
    }
    if (r.role != (s->role == LS_ROLE_PC ? LS_ROLE_PHONE : LS_ROLE_PC)) {
        st_fail(s, "unexpected peer role %u", (unsigned)r.role);
        return -1;
    }
    if (s->role == LS_ROLE_PC && r.seconds >= 1 && r.seconds <= 600)
        s->seconds = (int)r.seconds;
    return 0;
}

typedef struct {
    ls_session *s;
    int phase;
    int ui_phase;
    uint64_t t_phase;
    uint64_t duration;
    phase_stats_t st;
    live_rate_t lr;
    uint64_t win_t;
    uint64_t live_bytes;
    uint64_t last_data_ns;
} send_ctx_t;

static void send_ctx_init(send_ctx_t *c, ls_session *s, int phase)
{
    memset(c, 0, sizeof(*c));
    c->s = s;
    c->phase = phase;
    c->ui_phase = (phase == 1) ? LS_PHASE_UPLINK : LS_PHASE_DOWNLINK;
    c->t_phase = now_ns();
    c->duration = (uint64_t)s->seconds * 1000000000ull;
    stats_init(&c->st);
    live_reset(&c->lr, c->t_phase, 0);
    c->win_t = c->t_phase;
    st_live(s, c->ui_phase, 0.0, 0.0, 0.0);
}

static int send_prepared(send_ctx_t *c, uint8_t *buf, int is_end)
{
    ls_session *s = c->s;
    int off = 0;
    int flags = 0;
    uint64_t last_progress = now_ns();
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    if (!is_end) {
        uint32_t tag = ((const ls_frame_hdr_t *)buf)->seq;
        memcpy(buf + sizeof(ls_frame_hdr_t), &tag, sizeof(tag));
        memcpy(buf + LS_BLOCK_SIZE - sizeof(tag), &tag, sizeof(tag));
    }

    while (off < (int)LS_BLOCK_SIZE) {
        int rem = (int)LS_BLOCK_SIZE - off;
        int n, e;
        uint64_t a, z;
        if (atomic_load(&s->stop))
            return -2;
        a = now_ns();
        n = send(s->sock, (const char *)buf + off, rem, flags);
        z = now_ns();
        stats_io(&c->st, z - a);
        if (n > 0) {
            off += n;
            last_progress = z;
            if (n < 65536)
                c->st.partial++;
            if (z - a > LS_GAP_LOG_NS)
                ls_log("[P%d SEND] slow syscall: %d/%d bytes in %.1f ms",
                       c->phase, n, rem, (double)(z - a) / 1e6);
            if (!is_end) {
                c->live_bytes += (uint64_t)n;
                c->st.bytes += (uint64_t)n;
                if (z - c->win_t >= LS_RATE_WINDOW) {
                    double raw, smooth, prog;
                    smooth = live_sample(&c->lr, z, c->live_bytes, &raw);
                    stats_window(&c->st, raw);
                    prog = (double)(z - c->t_phase) / (double)c->duration;
                    st_live(s, c->ui_phase, smooth, raw, prog);
                    ls_log("[P%d SEND] tid=%lu t=%.2f raw=%.1f smooth=%.1f bytes=%llu",
                           c->phase, tid_now(),
                           (double)(z - c->t_phase) / 1e9, raw, smooth,
                           (unsigned long long)c->live_bytes);
                    c->win_t = z;
                    if ((c->st.win_n % 5) == 0)
                        log_tcp_info(s, c->phase == 1 ? "P1" : "P2");
                }
            }
            continue;
        }
        if (n == 0)
            return -1;
        e = LS_SOCK_ERRNO;
        if (sock_intr(e))
            continue;
        if (sock_timeout(e)) {
            if (now_ns() - last_progress > LS_STALL_NS)
                return -1;
            continue;
        }
        return -1;
    }

    if (!is_end) {
        c->st.frames++;
        c->last_data_ns = now_ns();
    }
    return 0;
}

typedef struct {
    ls_session *s;
    send_ctx_t *ctx;
    uint8_t *buf[2];
    int state[2];   /* 0 free, 1 filling, 2 ready, 3 sending */
    int is_end[2];
    uint32_t seq[2];
    int phase;
    uint64_t t0;
    uint64_t duration;
    ls_mutex_t mu;
    ls_cond_t cv;
    int done;
    int stop;
    uint64_t producer_cpu_ns;
} pipe_ctx_t;

static void pipe_producer_run(pipe_ctx_t *p)
{
    uint32_t seq = 0;
    uint64_t cpu0 = thread_cpu_ns();

    for (;;) {
        int idx = -1;
        int is_end;
        int i;
        uint64_t now, a, z;

        mu_lock(&p->mu);
        for (;;) {
            if (p->stop || atomic_load(&p->s->stop)) {
                mu_unlock(&p->mu);
                goto out;
            }
            for (i = 0; i < 2; i++) {
                if (p->state[i] == 0) {
                    idx = i;
                    p->state[i] = 1;
                    break;
                }
            }
            if (idx >= 0)
                break;
            cv_wait(&p->cv, &p->mu);
        }
        mu_unlock(&p->mu);

        now = now_ns();
        is_end = (now - p->t0 >= p->duration);
        a = now_ns();
        frame_put(p->buf[idx],
                  is_end ? (uint8_t)LS_FRAME_END : (uint8_t)LS_FRAME_DATA,
                  (uint8_t)p->phase, seq,
                  is_end ? (uint32_t)((now - p->t0) / 1000000ull) : 0);
        z = now_ns();
        p->ctx->st.prep_ns += z - a;

        mu_lock(&p->mu);
        p->is_end[idx] = is_end;
        p->seq[idx] = seq;
        p->state[idx] = 2;
        cv_broadcast(&p->cv);
        mu_unlock(&p->mu);
        if (is_end)
            break;
        seq++;
    }

out:
    mu_lock(&p->mu);
    p->done = 1;
    cv_broadcast(&p->cv);
    mu_unlock(&p->mu);
    p->producer_cpu_ns = thread_cpu_ns() - cpu0;
}

#ifdef _WIN32
static unsigned __stdcall pipe_entry(void *arg)
{
    pipe_producer_run((pipe_ctx_t *)arg);
    return 0;
}
#else
static void *pipe_entry(void *arg)
{
    pipe_producer_run((pipe_ctx_t *)arg);
    return NULL;
}
#endif

/* returns -3 if the helper thread could not be created (fallback allowed) */
static int send_pipeline(ls_session *s, send_ctx_t *c, uint8_t *b0, uint8_t *b1,
                         uint64_t *producer_cpu_ns)
{
    pipe_ctx_t p;
    ls_thread_t th;
    int spawned = 0;
    int rc = 0;

    memset(&p, 0, sizeof(p));
    p.s = s;
    p.ctx = c;
    p.buf[0] = b0;
    p.buf[1] = b1;
    p.phase = c->phase;
    p.t0 = c->t_phase;
    p.duration = c->duration;
    mu_init(&p.mu);
    cv_init(&p.cv);

#ifdef _WIN32
    {
        uintptr_t h = _beginthreadex(NULL, 0, pipe_entry, &p, 0, NULL);
        if (!h) {
            rc = -3;
        } else {
            th = (HANDLE)h;
            spawned = 1;
        }
    }
#else
    if (pthread_create(&th, NULL, pipe_entry, &p) != 0)
        rc = -3;
    else
        spawned = 1;
#endif

    if (!spawned) {
        cv_destroy(&p.cv);
        mu_destroy(&p.mu);
        return -3;
    }

    for (;;) {
        int idx = -1;
        int i;
        mu_lock(&p.mu);
        for (;;) {
            if (atomic_load(&s->stop)) {
                rc = -2;
                mu_unlock(&p.mu);
                goto stop;
            }
            {
                int cand = -1;
                for (i = 0; i < 2; i++) {
                    if (p.state[i] != 2)
                        continue;
                    if (cand < 0 ||
                        (int32_t)(p.seq[i] - p.seq[cand]) < 0)
                        cand = i;
                }
                idx = cand;
            }
            if (idx >= 0)
                break;
            if (p.done) {
                mu_unlock(&p.mu);
                rc = atomic_load(&s->stop) ? -2 : -1;
                goto stop;
            }
            cv_wait(&p.cv, &p.mu);
        }
        p.state[idx] = 3;
        mu_unlock(&p.mu);

        rc = send_prepared(c, p.buf[idx], p.is_end[idx]);

        mu_lock(&p.mu);
        p.state[idx] = 0;
        cv_broadcast(&p.cv);
        mu_unlock(&p.mu);

        if (rc != 0)
            goto stop;
        if (p.is_end[idx])
            break;
    }

    mu_lock(&p.mu);
    p.stop = 1;
    cv_broadcast(&p.cv);
    mu_unlock(&p.mu);
    goto join;

stop:
    mu_lock(&p.mu);
    p.stop = 1;
    cv_broadcast(&p.cv);
    mu_unlock(&p.mu);

join:
#ifdef _WIN32
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    cv_destroy(&p.cv);
    mu_destroy(&p.mu);
    if (producer_cpu_ns)
        *producer_cpu_ns = p.producer_cpu_ns;
    return rc;
}

static int send_inline(send_ctx_t *c, uint8_t *b0, uint8_t *b1)
{
    uint32_t seq = 0;
    for (;;) {
        uint8_t *b;
        uint64_t a;
        int rc;
        if (atomic_load(&c->s->stop))
            return -2;
        if (now_ns() - c->t_phase >= c->duration)
            break;
        b = (seq & 1u) ? b1 : b0;
        a = now_ns();
        frame_put(b, LS_FRAME_DATA, (uint8_t)c->phase, seq, 0);
        c->st.prep_ns += now_ns() - a;
        rc = send_prepared(c, b, 0);
        if (rc != 0)
            return rc;
        seq++;
    }
    {
        uint8_t *b = (seq & 1u) ? b1 : b0;
        uint32_t ms = (uint32_t)((now_ns() - c->t_phase) / 1000000ull);
        frame_put(b, LS_FRAME_END, (uint8_t)c->phase, seq, ms);
        return send_prepared(c, b, 1);
    }
}

static void log_send_summary(send_ctx_t *c, uint64_t span_ns, int rc,
                             int pipeline, uint64_t producer_cpu_ns)
{
    double span_s = (double)span_ns / 1e9;
    double avg = calc_mbps(c->st.bytes, span_ns);
    double cpu_ns = (double)(c->st.cpu_end_ns - c->st.cpu_start_ns);
    double cpu_pct = span_ns ? cpu_ns * 100.0 / (double)span_ns : 0.0;
    double win_avg = c->st.win_n ? c->st.win_sum / c->st.win_n : 0.0;
    double win_min = c->st.win_n ? c->st.win_min : 0.0;
    ls_log("[P%d SEND] pipe=%d rc=%d bytes=%llu frames=%llu span=%.3fs avg=%.2f Mbps "
           "win_n=%d raw_min=%.1f raw_avg=%.1f raw_max=%.1f "
           "calls=%llu small=%llu sys_wall_ms=%.1f sys_max_ms=%.1f "
           "cpu_ms=%.1f cpu_pct=%.1f prep_ms=%.1f prod_cpu_ms=%.1f",
           c->phase, pipeline, rc,
           (unsigned long long)c->st.bytes,
           (unsigned long long)c->st.frames,
           span_s, avg, c->st.win_n, win_min, win_avg,
           c->st.win_n ? c->st.win_max : 0.0,
           (unsigned long long)c->st.calls,
           (unsigned long long)c->st.partial,
           (double)c->st.sys_ns / 1e6,
           (double)c->st.sys_max_ns / 1e6,
           (double)(c->st.cpu_end_ns - c->st.cpu_start_ns) / 1e6,
           cpu_pct,
           (double)c->st.prep_ns / 1e6,
           (double)producer_cpu_ns / 1e6);
}

static int send_phase(ls_session *s, uint8_t *b0, uint8_t *b1, int phase,
                      uint64_t *out_bytes, uint64_t *out_ns)
{
    send_ctx_t c;
    int rc;
    int pipeline = s->pipeline ? 1 : 0;
    uint64_t producer_cpu_ns = 0;

    *out_bytes = 0;
    *out_ns = 0;
    send_ctx_init(&c, s, phase);

    if (pipeline) {
        rc = send_pipeline(s, &c, b0, b1, &producer_cpu_ns);
        if (rc == -3) {
            ls_log("[P%d SEND] pipeline thread create failed, fallback inline",
                   phase);
            pipeline = 0;
            rc = send_inline(&c, b0, b1);
        }
    } else {
        rc = send_inline(&c, b0, b1);
    }

    c.st.cpu_end_ns = thread_cpu_ns();
    log_send_summary(&c, (c.last_data_ns > c.t_phase)
                            ? (c.last_data_ns - c.t_phase) : 0,
                     rc, pipeline, producer_cpu_ns);
    log_tcp_info(s, phase == 1 ? "P1-end" : "P2-end");
    *out_bytes = c.st.bytes;
    *out_ns = (c.last_data_ns > c.t_phase) ? (c.last_data_ns - c.t_phase) : 0;
    return rc;
}

static int recv_phase(ls_session *s, uint8_t *b0, uint8_t *b1, int phase,
                      uint64_t *out_bytes, uint64_t *out_ns)
{
    uint64_t t0 = now_ns();
    uint64_t last_rx = t0;
    uint64_t win_t = t0;
    uint64_t live_bytes = 0;
    uint64_t data_bytes = 0;
    uint64_t last_data = 0;
    uint32_t idx = 0;
    uint32_t expect_seq = 0;
    int got = 0;
    int errors = 0;
    uint64_t duration = (uint64_t)s->seconds * 1000000000ull;
    int ui_phase = (phase == 1) ? LS_PHASE_UPLINK : LS_PHASE_DOWNLINK;
    phase_stats_t st;
    live_rate_t lr;
    uint8_t *b = b0;
    int off = 0;

    *out_bytes = 0;
    *out_ns = 0;
    stats_init(&st);
    live_reset(&lr, t0, 0);
    st.last_rx_ns = t0;
    st_live(s, ui_phase, 0.0, 0.0, 0.0);

    for (;;) {
        uint64_t a, z;
        int n, e;
        int rem;

        if (atomic_load(&s->stop))
            return -2;

        if (off == 0)
            b = (idx & 1u) ? b1 : b0;
        rem = (int)LS_BLOCK_SIZE - off;
        a = now_ns();
        n = recv(s->sock, (char *)b + off, rem, 0);
        z = now_ns();
        stats_io(&st, z - a);

        if (n > 0) {
            if (z - last_rx > LS_GAP_LOG_NS) {
                st.gaps++;
                if (z - last_rx > st.gap_max_ns)
                    st.gap_max_ns = z - last_rx;
                ls_log("[P%d RECV] gap %.1f ms before %d bytes at t=%.2f",
                       phase, (double)(z - last_rx) / 1e6, n,
                       (double)(z - t0) / 1e9);
            }
            last_rx = z;
            if (n < 65536)
                st.partial++;
            off += n;
            live_bytes += (uint64_t)n;

            if (off == (int)LS_BLOCK_SIZE) {
                ls_frame_hdr_t *h = (ls_frame_hdr_t *)b;
                uint32_t tag1 = 0, tag2 = 0;
                if (h->magic != LS_MAGIC) {
                    st_fail(s, "bad frame magic 0x%08X", (unsigned)h->magic);
                    goto fail;
                }
                if (h->phase != (uint8_t)phase) {
                    st_fail(s, "bad frame phase %u (expected %d)",
                            (unsigned)h->phase, phase);
                    goto fail;
                }
                if (h->type == LS_FRAME_END)
                    break;
                if (h->type != LS_FRAME_DATA) {
                    st_fail(s, "unexpected frame type %u", (unsigned)h->type);
                    goto fail;
                }
                if (h->seq != expect_seq)
                    errors++;
                expect_seq = h->seq + 1;
                memcpy(&tag1, b + sizeof(ls_frame_hdr_t), sizeof(tag1));
                memcpy(&tag2, b + LS_BLOCK_SIZE - sizeof(tag2), sizeof(tag2));
                if (tag1 != h->seq || tag2 != h->seq)
                    errors++;
                data_bytes += LS_BLOCK_SIZE;
                st.frames++;
                idx++;
                got = 1;
                off = 0;
                last_data = z;
            }

            if (z - win_t >= LS_RATE_WINDOW) {
                double raw, smooth, prog;
                smooth = live_sample(&lr, z, live_bytes, &raw);
                stats_window(&st, raw);
                prog = (double)(z - t0) / (double)duration;
                st_live(s, ui_phase, smooth, raw, prog);
                ls_log("[P%d RECV] tid=%lu t=%.2f raw=%.1f smooth=%.1f "
                       "live_bytes=%llu frames=%llu",
                       phase, tid_now(), (double)(z - t0) / 1e9,
                       raw, smooth,
                       (unsigned long long)live_bytes,
                       (unsigned long long)st.frames);
                win_t = z;
                if ((st.win_n % 5) == 0)
                    log_tcp_info(s, phase == 1 ? "P1" : "P2");
            }
            continue;
        }

        if (n == 0) {
            st_fail(s, "connection closed in phase %d", phase);
            goto fail;
        }
        e = LS_SOCK_ERRNO;
        if (sock_intr(e))
            continue;
        if (sock_timeout(e)) {
            if (z - last_rx > LS_STALL_NS) {
                st_fail(s, "receive stalled or connection lost (phase %d)", phase);
                goto fail;
            }
            continue;
        }
        st_fail(s, "recv failed in phase %d (errno %d)", phase, e);
        goto fail;
    }

    if (!got) {
        st_fail(s, "phase %d ended with no data", phase);
        goto fail;
    }

    st.cpu_end_ns = thread_cpu_ns();
    {
        double span_ns = (double)(last_data > t0 ? last_data - t0 : 0);
        double span_s = span_ns / 1e9;
        double avg = calc_mbps(data_bytes, (uint64_t)span_ns);
        double cpu_ns = (double)(st.cpu_end_ns - st.cpu_start_ns);
        double cpu_pct = span_ns > 0 ? cpu_ns * 100.0 / span_ns : 0.0;
        double win_avg = st.win_n ? st.win_sum / st.win_n : 0.0;
        ls_log("[P%d RECV] bytes=%llu frames=%llu span=%.3fs avg=%.2f Mbps "
               "win_n=%d raw_min=%.1f raw_avg=%.1f raw_max=%.1f "
               "calls=%llu small=%llu recv_wall_ms=%.1f recv_max_ms=%.1f "
               "cpu_ms=%.1f cpu_pct=%.1f gaps=%llu gap_max_ms=%.1f errors=%d",
               phase,
               (unsigned long long)data_bytes,
               (unsigned long long)st.frames,
               span_s, avg, st.win_n,
               st.win_n ? st.win_min : 0.0, win_avg,
               st.win_n ? st.win_max : 0.0,
               (unsigned long long)st.calls,
               (unsigned long long)st.partial,
               (double)st.sys_ns / 1e6,
               (double)st.sys_max_ns / 1e6,
               (double)(st.cpu_end_ns - st.cpu_start_ns) / 1e6,
               cpu_pct,
               (unsigned long long)st.gaps,
               (double)st.gap_max_ns / 1e6,
               errors);
    }
    log_tcp_info(s, phase == 1 ? "P1-end" : "P2-end");
    *out_bytes = data_bytes;
    *out_ns = (last_data > t0) ? (last_data - t0) : 0;
    if (errors)
        st_info(s, "frame check warnings: %d", errors);
    return 0;

fail:
    st.cpu_end_ns = thread_cpu_ns();
    ls_log("[P%d RECV] failed rc data_bytes=%llu live_bytes=%llu calls=%llu "
           "cpu_ms=%.1f", phase,
           (unsigned long long)data_bytes,
           (unsigned long long)live_bytes,
           (unsigned long long)st.calls,
           (double)(st.cpu_end_ns - st.cpu_start_ns) / 1e6);
    return -1;
}

static int recv_control(ls_session *s, uint8_t *buf, uint8_t type,
                        uint8_t phase, uint32_t *value)
{
    ls_frame_hdr_t *h;
    int rc = recv_exact(s, buf, (int)LS_BLOCK_SIZE, LS_STALL_NS);
    if (rc != 0) {
        st_fail(s, "waiting for control frame failed (type %u)", (unsigned)type);
        return -1;
    }
    h = (ls_frame_hdr_t *)buf;
    if (h->magic != LS_MAGIC || h->type != type || h->phase != phase) {
        st_fail(s, "bad control frame (got type %u phase %u)",
                (unsigned)h->type, (unsigned)h->phase);
        return -1;
    }
    if (value)
        *value = h->value;
    return 0;
}

static int send_done(ls_session *s, uint8_t *buf, uint8_t phase,
                     uint64_t bytes, uint64_t ns)
{
    ls_result_t r;
    r.bytes = bytes;
    r.ns = ns;
    frame_put(buf, LS_FRAME_DONE, phase, 0, 0);
    memcpy(buf + sizeof(ls_frame_hdr_t), &r, sizeof(r));
    return send_all(s, buf, (int)LS_BLOCK_SIZE);
}

static int recv_done(ls_session *s, uint8_t *buf, uint8_t phase,
                     uint64_t *bytes, uint64_t *ns)
{
    ls_frame_hdr_t *h;
    ls_result_t r;
    int rc = recv_exact(s, buf, (int)LS_BLOCK_SIZE, LS_STALL_NS);
    if (rc != 0)
        return -1;
    h = (ls_frame_hdr_t *)buf;
    if (h->magic != LS_MAGIC || h->type != LS_FRAME_DONE || h->phase != phase)
        return -1;
    memcpy(&r, buf + sizeof(ls_frame_hdr_t), sizeof(r));
    if (r.ns == 0)
        return -1;
    *bytes = r.bytes;
    *ns = r.ns;
    return 0;
}

/* ---------- role workers ---------- */

static int run_phone(ls_session *s)
{
    uint8_t *b0 = block_alloc((uint8_t)LS_PATTERN_A);
    uint8_t *b1 = block_alloc((uint8_t)LS_PATTERN_B);
    uint64_t up_b = 0, up_ns = 0, down_b = 0, down_ns = 0;
    uint64_t sb = 0, sn = 0;
    int up_valid = 0, down_valid = 0;
    int rc;

    if (!b0 || !b1) {
        st_fail(s, "out of memory for 2 x 1 MiB buffers");
        block_free(b0);
        block_free(b1);
        return -1;
    }

    rc = server_wait(s);
    if (rc != 0)
        goto out;

    st_phase(s, LS_PHASE_HANDSHAKE);
    if (do_hello(s) != 0)
        goto out;

    st_phase(s, LS_PHASE_UPLINK);
    rc = send_control(s, b0, LS_FRAME_START, 1, 0);
    if (rc != 0) {
        if (rc != -2)
            st_fail(s, "sending START failed");
        goto out;
    }
    rc = send_phase(s, b0, b1, 1, &sb, &sn);
    if (rc != 0) {
        if (rc != -2)
            st_fail(s, "uplink send failed");
        goto out;
    }

    rc = send_control(s, b0, LS_FRAME_START, 2, 0);
    if (rc != 0) {
        if (rc != -2)
            st_fail(s, "sending START(2) failed");
        goto out;
    }

    st_phase(s, LS_PHASE_DOWNLINK);
    rc = recv_phase(s, b0, b1, 2, &down_b, &down_ns);
    if (rc != 0)
        goto out;
    down_valid = (down_ns > 0);

    if (send_done(s, b0, 2, down_b, down_ns) != 0)
        st_info(s, "sending local result failed");

    if (recv_done(s, b1, 1, &up_b, &up_ns) == 0)
        up_valid = (up_ns > 0);
    else
        st_info(s, "peer result not received");

    st_done(s, up_valid, up_b, up_ns, down_valid, down_b, down_ns, NULL);
    rc = 0;

out:
    block_free(b0);
    block_free(b1);
    return rc;
}

static int run_client(ls_session *s)
{
    uint8_t *b0 = block_alloc((uint8_t)LS_PATTERN_A);
    uint8_t *b1 = block_alloc((uint8_t)LS_PATTERN_B);
    uint64_t up_b = 0, up_ns = 0, down_b = 0, down_ns = 0;
    uint64_t sb = 0, sn = 0;
    int up_valid = 0, down_valid = 0;
    int rc;

    if (!b0 || !b1) {
        st_fail(s, "out of memory for 2 x 1 MiB buffers");
        block_free(b0);
        block_free(b1);
        return -1;
    }

    st_phase(s, LS_PHASE_CONNECTING);
    rc = client_connect(s);
    if (rc != 0) {
        if (rc != -2)
            st_fail(s, "connect failed");
        goto out;
    }

    st_phase(s, LS_PHASE_HANDSHAKE);
    if (do_hello(s) != 0)
        goto out;

    rc = recv_control(s, b0, LS_FRAME_START, 1, NULL);
    if (rc != 0)
        goto out;

    st_phase(s, LS_PHASE_UPLINK);
    rc = recv_phase(s, b0, b1, 1, &up_b, &up_ns);
    if (rc != 0)
        goto out;
    up_valid = (up_ns > 0);

    rc = recv_control(s, b0, LS_FRAME_START, 2, NULL);
    if (rc != 0)
        goto out;

    st_phase(s, LS_PHASE_DOWNLINK);
    rc = send_phase(s, b0, b1, 2, &sb, &sn);
    if (rc != 0) {
        if (rc != -2)
            st_fail(s, "downlink send failed");
        goto out;
    }

    if (recv_done(s, b1, 2, &down_b, &down_ns) == 0)
        down_valid = (down_ns > 0);
    else
        st_info(s, "peer result not received");

    if (send_done(s, b0, 1, up_b, up_ns) != 0)
        st_info(s, "sending local result failed");

    st_done(s, up_valid, up_b, up_ns, down_valid, down_b, down_ns, NULL);
    rc = 0;

out:
    block_free(b0);
    block_free(b1);
    return rc;
}

static void worker_main(ls_session *s)
{
    int rc;
    if (s->role == LS_ROLE_PHONE)
        rc = run_phone(s);
    else
        rc = run_client(s);

    log_tcp_info(s, "session-end");
    sock_close(s->sock);
    s->sock = LS_INVALID_SOCK;
    sock_close(s->listen_sock);
    s->listen_sock = LS_INVALID_SOCK;

    mu_lock(&s->mu);
    if (atomic_load(&s->stop) || rc == -2) {
        if (s->st.phase != LS_PHASE_DONE && s->st.phase != LS_PHASE_ERROR) {
            s->st.phase = LS_PHASE_IDLE;
            snprintf(s->st.err, sizeof(s->st.err), "stopped");
        }
        s->st.running = 0;
    } else if (rc != 0) {
        if (s->st.phase != LS_PHASE_ERROR) {
            s->st.phase = LS_PHASE_ERROR;
            s->st.error = 1;
            snprintf(s->st.err, sizeof(s->st.err), "test failed");
        }
        s->st.running = 0;
    }
    mu_unlock(&s->mu);

    {
        ls_status tmp;
        mu_lock(&s->mu);
        tmp = s->st;
        mu_unlock(&s->mu);
        ls_log("[end] role=%d phase=%d running=%d err='%s'",
               tmp.role, tmp.phase, tmp.running, tmp.err);
    }
}

#ifdef _WIN32
static unsigned __stdcall worker_entry(void *arg)
{
    worker_main((ls_session *)arg);
    return 0;
}
#else
static void *worker_entry(void *arg)
{
    worker_main((ls_session *)arg);
    return NULL;
}
#endif

/* ---------- public API ---------- */

static int create_error(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
    return -1;
}

ls_session *ls_session_create(int role, const char *peer_ip, int port, int seconds,
                              char *err, size_t err_len)
{
    ls_session *s;

    if (role != LS_ROLE_PC && role != LS_ROLE_PHONE) {
        create_error(err, err_len, "invalid role");
        return NULL;
    }
    if (port < 1 || port > 65535) {
        create_error(err, err_len, "invalid port");
        return NULL;
    }
    if (seconds < 1 || seconds > 600) {
        create_error(err, err_len, "invalid duration");
        return NULL;
    }
    if (ensure_wsa() != 0) {
        create_error(err, err_len, "WSAStartup failed");
        return NULL;
    }

    s = (ls_session *)calloc(1, sizeof(*s));
    if (!s) {
        create_error(err, err_len, "out of memory");
        return NULL;
    }
    s->role = role;
    s->port = port;
    s->seconds = seconds;
    s->pipeline = 1;
    if (!peer_ip || inet_pton(AF_INET, peer_ip, &s->peer_addr) != 1) {
        free(s);
        create_error(err, err_len, "invalid peer IPv4 address");
        return NULL;
    }
    snprintf(s->cfg_peer_ip, sizeof(s->cfg_peer_ip), "%s", peer_ip);
    s->sock = LS_INVALID_SOCK;
    s->listen_sock = LS_INVALID_SOCK;
    atomic_init(&s->stop, 0);
    mu_init(&s->mu);

    s->st.role = role;
    s->st.port = port;
    s->st.seconds = seconds;
    s->st.phase = LS_PHASE_IDLE;
    snprintf(s->st.peer_ip, sizeof(s->st.peer_ip), "%s", peer_ip);
    if (err && err_len)
        err[0] = '\0';
    return s;
}

int ls_session_start(ls_session *s)
{
    if (!s || s->thread_started)
        return -1;

    atomic_store(&s->stop, 0);
    mu_lock(&s->mu);
    s->st.running = 1;
    s->st.finished = 0;
    s->st.error = 0;
    s->st.phase = LS_PHASE_CONNECTING;
    s->st.connected = 0;
    s->st.cur_mbps = 0.0;
    s->st.cur_mbps_raw = 0.0;
    s->st.progress = 0.0;
    s->st.has_up = 0;
    s->st.has_down = 0;
    s->st.up_mbps = 0.0;
    s->st.down_mbps = 0.0;
    s->st.up_bytes = 0;
    s->st.down_bytes = 0;
    s->st.up_secs = 0.0;
    s->st.down_secs = 0.0;
    s->st.err[0] = '\0';
    mu_unlock(&s->mu);

    log_wall_stamp("session start");
    ls_log("[init] role=%s peer=%s:%d seconds=%d pipeline=%d",
           s->role == LS_ROLE_PHONE ? "PHONE" : "PC",
           s->cfg_peer_ip, s->port, s->seconds, s->pipeline);

    s->thread_started = 1;
#ifdef _WIN32
    {
        uintptr_t th = _beginthreadex(NULL, 0, worker_entry, s, 0, NULL);
        if (!th) {
            s->thread_started = 0;
            st_fail(s, "thread creation failed");
            return -1;
        }
        s->thread = (HANDLE)th;
    }
#else
    if (pthread_create(&s->thread, NULL, worker_entry, s) != 0) {
        s->thread_started = 0;
        st_fail(s, "thread creation failed");
        return -1;
    }
#endif
    return 0;
}

void ls_session_stop(ls_session *s)
{
    if (!s)
        return;
    atomic_store(&s->stop, 1);
    if (s->thread_started) {
#ifdef _WIN32
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
#else
        pthread_join(s->thread, NULL);
#endif
        s->thread_started = 0;
        mu_lock(&s->mu);
        if (s->st.phase != LS_PHASE_DONE && s->st.phase != LS_PHASE_ERROR) {
            s->st.phase = LS_PHASE_IDLE;
            snprintf(s->st.err, sizeof(s->st.err), "stopped");
        }
        s->st.running = 0;
        mu_unlock(&s->mu);
    } else {
        mu_lock(&s->mu);
        s->st.running = 0;
        mu_unlock(&s->mu);
    }
}

void ls_session_destroy(ls_session *s)
{
    if (!s)
        return;
    ls_session_stop(s);
    mu_destroy(&s->mu);
    free(s);
}

void ls_session_get_status(ls_session *s, ls_status *out)
{
    if (!s || !out)
        return;
    mu_lock(&s->mu);
    *out = s->st;
    mu_unlock(&s->mu);
}

int ls_local_ip_for_peer(const char *peer_ip, int port, char *out, size_t out_len)
{
    struct sockaddr_in a, la;
    socklen_t ll = (socklen_t)sizeof(la);
    ls_sock_t fd;
    char tmp[64];

    if (!peer_ip || !out || out_len == 0)
        return -1;
    if (ensure_wsa() != 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, peer_ip, &a.sin_addr) != 1)
        return -1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == LS_INVALID_SOCK)
        return -1;
    if (connect(fd, (struct sockaddr *)&a, (socklen_t)sizeof(a)) != 0) {
        sock_close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&la, &ll) != 0) {
        sock_close(fd);
        return -1;
    }
    sock_close(fd);
    addr_to_str(&la, tmp, sizeof(tmp));
    snprintf(out, out_len, "%s", tmp);
    return 0;
}

void ls_session_set_pipeline(ls_session *s, int enabled)
{
    if (s)
        s->pipeline = enabled ? 1 : 0;
}
