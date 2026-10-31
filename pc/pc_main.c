#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include <stdio.h>
#include <stdint.h>

#include "ls_core.h"
#include "ls_disc.h"

#define APP_TITLE   L"局域网测速  LAN Speed Test"
#define APP_CLASS   L"LanSpeedTestWindow"
#define ID_START    1001
#define ID_STOP     1002
#define ID_EDIT_IP  1003
#define ID_PIPELINE 1005
#define ID_SCAN     1006
#define ID_LOG      1007
#define ID_TIMER    1
#define ID_TIMER_DISC 2
#define WM_APP_SCAN_DONE (WM_APP + 1)
#define PORT        5201
#define STREAMS     4
#define MAX_STREAMS 8
#define SECONDS     10
#define DISC_PORT   LS_DISC_DEFAULT_PORT
#define DISC_SCAN_MS 4000

static HINSTANCE   g_inst;
static HWND        g_hwnd;
static HWND        g_edit_ip;
static HWND        g_btn_start;
static HWND        g_btn_stop;
static HWND        g_btn_scan;
static HWND        g_chk_pipeline;
static HWND        g_chk_log;
static HFONT       g_font_title;
static HFONT       g_font_sub;
static HFONT       g_font_label;
static HFONT       g_font_phase;
static HFONT       g_font_big;
static HFONT       g_font_unit;
static HFONT       g_font_result;
static HFONT       g_font_small;
static HFONT       g_font_button;
static HBRUSH      g_bg_brush;
static HBRUSH      g_panel_brush;
static HBRUSH      g_edit_brush;
static HBRUSH      g_track_brush;
static HBRUSH      g_accent_brush;
static int         g_dpi = 96;

static ls_session *g_sess[MAX_STREAMS];
static int          g_streams;
static ls_status    g_st;
static int          g_running_ui;
static int          g_role = LS_ROLE_PHONE;
static WCHAR        g_local_hint[64] = L"";
static WCHAR        g_error_text[256] = L"";
static WCHAR        g_peer_name[64] = L"";
static WCHAR        g_peer_id[40] = L"";
static WCHAR        g_auto_ip[64] = L"";
static char         g_self_name[64] = "pc";
static int          g_ip_touched;
static int          g_filling;
static int          g_scan_notfound;
static volatile LONG g_scan_active;
static WCHAR        g_scan_text[96] = L"";

#define COL_BG       RGB(17, 19, 25)
#define COL_PANEL    RGB(28, 31, 40)
#define COL_EDIT     RGB(30, 34, 43)
#define COL_TEXT     RGB(232, 237, 246)
#define COL_DIM      RGB(138, 148, 168)
#define COL_FAINT    RGB(92, 100, 118)
#define COL_ACCENT   RGB(64, 200, 255)
#define COL_GREEN    RGB(72, 214, 148)
#define COL_RED      RGB(255, 106, 106)

static int S(int v)
{
    return MulDiv(v, g_dpi, 96);
}

static void draw_text(HDC dc, const WCHAR *text, int x, int y, int w, int h,
                      HFONT font, COLORREF color, UINT fmt)
{
    RECT r;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    SelectObject(dc, font);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    DrawTextW(dc, text, -1, &r, fmt);
}

static void utf8_to_wide(const char *src, WCHAR *dst, int cap)
{
    if (cap <= 0)
        return;
    dst[0] = 0;
    if (!src || !src[0])
        return;
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
}

static void make_fonts(void)
{
    const WCHAR *face = L"Microsoft YaHei UI";
    g_font_title  = CreateFontW(-S(30), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_sub    = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_label  = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_phase  = CreateFontW(-S(19), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_big    = CreateFontW(-S(62), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_unit   = CreateFontW(-S(20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_result = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_small  = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    g_font_button = CreateFontW(-S(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void delete_fonts(void)
{
    HFONT *fonts[] = { &g_font_title, &g_font_sub, &g_font_label, &g_font_phase,
                       &g_font_big, &g_font_unit, &g_font_result, &g_font_small,
                       &g_font_button };
    int i;
    for (i = 0; i < (int)(sizeof(fonts) / sizeof(fonts[0])); i++) {
        if (*fonts[i]) {
            DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }
}

static void layout_controls(void)
{
    int y = S(100);
    MoveWindow(g_edit_ip, S(114), y, S(160), S(34), TRUE);
    MoveWindow(g_btn_scan, S(282), S(98), S(76), S(38), TRUE);
    MoveWindow(g_chk_pipeline, S(366), y, S(140), S(34), TRUE);
    MoveWindow(g_chk_log, S(512), y, S(56), S(34), TRUE);
    MoveWindow(g_btn_start, S(576), S(96), S(90), S(38), TRUE);
    MoveWindow(g_btn_stop, S(674), S(96), S(90), S(38), TRUE);
}

static void set_ui_running(int running)
{
    g_running_ui = running;
    EnableWindow(g_edit_ip, !running);
    EnableWindow(g_btn_start, !running);
    EnableWindow(g_btn_stop, running);
    EnableWindow(g_btn_scan, !running && !g_scan_active);
    if (g_chk_pipeline)
        EnableWindow(g_chk_pipeline, !running);
    InvalidateRect(g_btn_start, NULL, TRUE);
    InvalidateRect(g_btn_stop, NULL, TRUE);
    InvalidateRect(g_btn_scan, NULL, TRUE);
    if (g_chk_pipeline)
        InvalidateRect(g_chk_pipeline, NULL, TRUE);
}

static int parse_ipv4_edit(HWND edit, char *out, size_t out_len)
{
    WCHAR w[64];
    char b[80];
    struct in_addr a;
    char norm[64];

    if (GetWindowTextW(edit, w, 64) <= 0)
        return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), NULL, NULL) <= 0)
        return 0;
    if (inet_pton(AF_INET, b, &a) != 1)
        return 0;
    if (!inet_ntop(AF_INET, &a, norm, sizeof(norm)))
        return 0;
    snprintf(out, out_len, "%s", norm);
    return 1;
}

static int cmp_ip(const char *a, const char *b)
{
    struct in_addr x, y;
    uint32_t va, vb;
    if (inet_pton(AF_INET, a, &x) != 1 || inet_pton(AF_INET, b, &y) != 1)
        return strcmp(a, b);
    va = ntohl(x.s_addr);
    vb = ntohl(y.s_addr);
    if (va == vb)
        return 0;
    return va < vb ? -1 : 1;
}

/* Both sides order (ip, id) the same way: the smaller one listens. */
static int decide_role(const char *local_ip, const char *peer_ip,
                       const char *self_id, const char *peer_id)
{
    int c = cmp_ip(local_ip, peer_ip);
    if (c == 0) {
        c = strcmp(self_id ? self_id : "", peer_id ? peer_id : "");
        if (c == 0)
            c = 1;
    }
    return c < 0 ? LS_ROLE_PHONE : LS_ROLE_PC;
}

static const WCHAR *role_name(void)
{
    return g_role == LS_ROLE_PHONE ? L"监听端" : L"连接端";
}

static const WCHAR *dir_up_text(void)
{
    return g_role == LS_ROLE_PHONE ? L"本机 → 对方" : L"对方 → 本机";
}

static const WCHAR *dir_down_text(void)
{
    return g_role == LS_ROLE_PHONE ? L"对方 → 本机" : L"本机 → 对方";
}

static void str_utf8_to_ui(const char *s)
{
    utf8_to_wide(s, g_error_text, (int)(sizeof(g_error_text) / sizeof(WCHAR)));
}

static void set_ip_text(const char *ip)
{
    WCHAR w[64];
    utf8_to_wide(ip, w, 64);
    g_filling = 1;
    SetWindowTextW(g_edit_ip, w);
    g_filling = 0;
    g_ip_touched = 0;
    wcscpy(g_auto_ip, w);
}

static void fill_peer(const char *ip, const char *name, const char *id, int force)
{
    char local[64];
    WCHAR w[64];
    WCHAR cur[64];
    int changed = 0;

    if (ip && ip[0]) {
        utf8_to_wide(ip, w, 64);
        GetWindowTextW(g_edit_ip, cur, 64);
        if (wcscmp(cur, w) != 0 && (force || !g_ip_touched)) {
            set_ip_text(ip);
            changed = 1;
        }
    }
    if (name && name[0]) {
        utf8_to_wide(name, w, 64);
        if (wcscmp(w, g_peer_name) != 0) {
            wcscpy(g_peer_name, w);
            changed = 1;
        }
    }
    if (id && id[0]) {
        utf8_to_wide(id, w, 40);
        if (wcscmp(w, g_peer_id) != 0) {
            wcscpy(g_peer_id, w);
            changed = 1;
        }
    }
    if (changed) {
        if (ip && ip[0] && ls_local_ip_for_peer(ip, PORT, local, sizeof(local)) == 0)
            utf8_to_wide(local, g_local_hint, 64);
        if (g_scan_notfound) {
            g_scan_notfound = 0;
            g_scan_text[0] = 0;
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* Looks up the discovery id of a peer by address. */
static int peer_id_of(const char *ip, char *out, size_t out_len)
{
    int i, n = ls_disc_peer_count();
    for (i = 0; i < n; i++) {
        ls_peer_info p;
        if (ls_disc_peer_at(i, &p) == 0 && strcmp(p.ip, ip) == 0) {
            snprintf(out, out_len, "%s", p.id);
            return 1;
        }
    }
    return 0;
}

static void set_scan_ui(int active, const WCHAR *text)
{
    if (text)
        wcsncpy(g_scan_text, text, 95);
    EnableWindow(g_btn_scan, !active && !g_running_ui);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static DWORD WINAPI scan_thread(LPVOID arg)
{
    ls_peer_info peer;
    int found;
    (void)arg;
    found = ls_disc_scan(DISC_SCAN_MS, &peer) == 0;
    InterlockedExchange(&g_scan_active, 0);
    PostMessage(g_hwnd, WM_APP_SCAN_DONE, (WPARAM)(found ? 1 : 0), 0);
    return 0;
}

static void start_scan(void)
{
    HANDLE th;
    if (g_running_ui || InterlockedCompareExchange(&g_scan_active, 1, 0) != 0)
        return;
    set_scan_ui(1, L"正在扫描局域网内的测速设备…");
    th = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&g_scan_active, 0);
}

static void start_test(HWND hwnd)
{
    char peer[64];
    char local[64] = "";
    char self_id[40] = "";
    char peer_id[40] = "";
    char err[192] = "";
    int role;

    if (!parse_ipv4_edit(g_edit_ip, peer, sizeof(peer))) {
        MessageBoxW(hwnd, L"请输入完整的 IPv4 地址，例如 192.168.5.12。",
                    L"IP 地址无效", MB_OK | MB_ICONWARNING);
        return;
    }
    if (ls_local_ip_for_peer(peer, PORT, local, sizeof(local)) != 0) {
        MessageBoxW(hwnd, L"无法确定本机到对端的路由地址，请检查网络连接。",
                    L"启动失败", MB_OK | MB_ICONERROR);
        return;
    }
    if (ls_disc_self_id(self_id, sizeof(self_id)) != 0)
        self_id[0] = '\0';
    if (!peer_id_of(peer, peer_id, sizeof(peer_id)))
        peer_id[0] = '\0';
    if (strcmp(local, peer) == 0 && !peer_id[0]) {
        MessageBoxW(hwnd, L"对方与本机地址相同，请先让两端自动发现一次。",
                    L"启动失败", MB_OK | MB_ICONWARNING);
        return;
    }
    role = decide_role(local, peer, self_id, peer_id);
    g_role = role;

    utf8_to_wide(local, g_local_hint, 64);

    {
        int i;
        int pipeline = SendMessageW(g_chk_pipeline, BM_GETCHECK, 0, 0) == BST_CHECKED;
        for (i = 0; i < MAX_STREAMS; i++) {
            if (g_sess[i]) {
                ls_session_destroy(g_sess[i]);
                g_sess[i] = NULL;
            }
        }
        g_streams = STREAMS;
        for (i = 0; i < g_streams; i++) {
            g_sess[i] = ls_session_create(role, peer, PORT + i, SECONDS,
                                          err, sizeof(err));
            if (g_sess[i] == NULL) {
                str_utf8_to_ui(err[0] ? err : "create session failed");
                MessageBoxW(hwnd, g_error_text, L"启动失败", MB_OK | MB_ICONERROR);
                return;
            }
            ls_session_set_pipeline(g_sess[i], pipeline);
        }
        for (i = 0; i < g_streams; i++) {
            if (ls_session_start(g_sess[i]) != 0) {
                int j;
                str_utf8_to_ui("failed to start worker thread");
                for (j = 0; j < g_streams; j++) {
                    if (g_sess[j]) {
                        ls_session_destroy(g_sess[j]);
                        g_sess[j] = NULL;
                    }
                }
                MessageBoxW(hwnd, g_error_text, L"启动失败", MB_OK | MB_ICONERROR);
                return;
            }
        }
    }

    memset(&g_st, 0, sizeof(g_st));
    g_st.phase = LS_PHASE_CONNECTING;
    g_st.port = PORT;
    g_st.seconds = SECONDS;
    g_error_text[0] = 0;
    set_ui_running(1);
    SetTimer(hwnd, ID_TIMER, 100, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void aggregate_status(ls_status *agg)
{
    int i;
    int n = g_streams;
    int any_err = 0, any_connected = 0, any_running = 0, all_done = 1;
    int min_phase = 99;
    int up_count = 0, down_count = 0, prog_n = 0;
    double cur = 0.0, prog = 0.0;
    uint64_t up_b = 0, down_b = 0;
    double up_s = 0.0, down_s = 0.0;
    char err[192] = "";
    char peer[64] = "";
    char local[64] = "";

    memset(agg, 0, sizeof(*agg));
    agg->port = PORT;
    agg->seconds = SECONDS;
    agg->phase = LS_PHASE_CONNECTING;

    for (i = 0; i < n; i++) {
        ls_status st;
        if (!g_sess[i])
            continue;
        ls_session_get_status(g_sess[i], &st);
        if (st.connected) {
            any_connected = 1;
            if (!peer[0]) {
                snprintf(peer, sizeof(peer), "%s", st.peer_ip);
                snprintf(local, sizeof(local), "%s", st.local_ip);
            }
        }
        if (st.running)
            any_running = 1;
        if (st.phase == LS_PHASE_ERROR) {
            any_err = 1;
            if (!err[0] && st.err[0])
                snprintf(err, sizeof(err), "%s", st.err);
        }
        if (st.phase != LS_PHASE_DONE)
            all_done = 0;
        if (st.phase != LS_PHASE_DONE && st.phase != LS_PHASE_ERROR &&
            st.phase != LS_PHASE_IDLE && st.phase < min_phase)
            min_phase = st.phase;
        if (st.phase == LS_PHASE_UPLINK || st.phase == LS_PHASE_DOWNLINK) {
            cur += st.cur_mbps;
            prog += st.progress;
            prog_n++;
        }
        if (st.has_up) {
            up_count++;
            up_b += st.up_bytes;
            if (st.up_secs > up_s)
                up_s = st.up_secs;
        }
        if (st.has_down) {
            down_count++;
            down_b += st.down_bytes;
            if (st.down_secs > down_s)
                down_s = st.down_secs;
        }
    }

    agg->connected = any_connected;
    agg->running = any_running;
    agg->finished = all_done;
    agg->error = any_err;
    if (any_err)
        agg->phase = LS_PHASE_ERROR;
    else if (all_done)
        agg->phase = LS_PHASE_DONE;
    else if (min_phase < 99)
        agg->phase = min_phase;
    agg->cur_mbps = cur;
    agg->progress = prog_n ? prog / prog_n : 0.0;
    agg->has_up = up_count > 0;
    agg->has_down = down_count > 0;
    agg->up_bytes = up_b;
    agg->down_bytes = down_b;
    agg->up_secs = up_s;
    agg->down_secs = down_s;
    agg->up_mbps = up_s > 0.0 ? (double)up_b * 8.0 / up_s / 1e6 : 0.0;
    agg->down_mbps = down_s > 0.0 ? (double)down_b * 8.0 / down_s / 1e6 : 0.0;
    snprintf(agg->peer_ip, sizeof(agg->peer_ip), "%s", peer);
    snprintf(agg->local_ip, sizeof(agg->local_ip), "%s", local);
    if (any_err)
        snprintf(agg->err, sizeof(agg->err), "%s", err);
    else if (all_done && up_count > 0 && up_count < n)
        snprintf(agg->err, sizeof(agg->err), "only %d/%d streams completed",
                 up_count, n);
}

static void stop_test(HWND hwnd, int user_stop)
{
    int i;
    KillTimer(hwnd, ID_TIMER);
    for (i = 0; i < g_streams; i++) {
        if (g_sess[i])
            ls_session_stop(g_sess[i]);
    }
    aggregate_status(&g_st);
    set_ui_running(0);
    if (user_stop) {
        g_st.phase = LS_PHASE_IDLE;
        g_st.running = 0;
        g_st.cur_mbps = 0.0;
        g_st.progress = 0.0;
        swprintf(g_error_text, 256, L"已停止");
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void poll_status(HWND hwnd)
{
    int i, any_running = 0;
    if (g_streams == 0)
        return;
    aggregate_status(&g_st);
    if (g_st.error) {
        for (i = 0; i < g_streams; i++) {
            if (g_sess[i])
                ls_session_stop(g_sess[i]);
        }
    }
    for (i = 0; i < g_streams; i++) {
        ls_status t;
        if (!g_sess[i])
            continue;
        ls_session_get_status(g_sess[i], &t);
        if (t.running) {
            any_running = 1;
            break;
        }
    }
    g_st.running = any_running;
    if (g_st.err[0] && (g_st.phase == LS_PHASE_ERROR || g_st.phase == LS_PHASE_DONE))
        str_utf8_to_ui(g_st.err);
    if (!any_running) {
        KillTimer(hwnd, ID_TIMER);
        set_ui_running(0);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Fills an empty IP field from passive discovery beacons. */
static void poll_discovery(HWND hwnd)
{
    ls_peer_info peer;
    (void)hwnd;
    if (g_running_ui || InterlockedCompareExchange(&g_scan_active, 0, 0) != 0)
        return;
    if (ls_disc_latest(&peer) == 0)
        fill_peer(peer.ip, peer.name, peer.id, 0);
}

static const WCHAR *phase_text(void)
{
    switch (g_st.phase) {
    case LS_PHASE_CONNECTING:
        return g_role == LS_ROLE_PHONE ? L"等待对方连接…" : L"正在连接对方…";
    case LS_PHASE_HANDSHAKE:
        return L"已连接，握手中…";
    case LS_PHASE_UPLINK:
        return dir_up_text();
    case LS_PHASE_DOWNLINK:
        return dir_down_text();
    case LS_PHASE_DONE:
        return L"测速完成";
    case LS_PHASE_ERROR:
        return L"出错";
    default:
        return L"就绪";
    }
}

static COLORREF phase_color(void)
{
    switch (g_st.phase) {
    case LS_PHASE_UPLINK:
    case LS_PHASE_DOWNLINK:
        return COL_ACCENT;
    case LS_PHASE_DONE:
        return COL_GREEN;
    case LS_PHASE_ERROR:
        return COL_RED;
    default:
        return COL_TEXT;
    }
}

static double display_mbps(void)
{
    if (g_st.phase == LS_PHASE_UPLINK || g_st.phase == LS_PHASE_DOWNLINK)
        return g_st.cur_mbps;
    if (g_st.phase == LS_PHASE_DONE) {
        if (g_st.has_down)
            return g_st.down_mbps;
        if (g_st.has_up)
            return g_st.up_mbps;
    }
    return 0.0;
}

static void draw_progress(HDC dc)
{
    RECT r;
    int w, h, fill;
    double p = 0.0;
    r.left = S(36);
    r.top = S(336);
    r.right = S(764);
    r.bottom = S(346);
    if (g_st.phase == LS_PHASE_DONE)
        p = 1.0;
    else
        p = g_st.progress;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    SelectObject(dc, GetStockObject(NULL_PEN));
    SelectObject(dc, g_track_brush);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));

    w = r.right - r.left;
    h = r.bottom - r.top;
    fill = (int)(w * p);
    if (fill > 0) {
        SelectObject(dc, g_accent_brush);
        if (fill < h)
            fill = h;
        RoundRect(dc, r.left, r.top, r.left + fill, r.bottom, S(8), S(8));
    }
}

static void draw_results(HDC dc)
{
    WCHAR line[256];
    int y1 = S(368);
    int y2 = S(402);
    double mb_up = g_st.has_up ? g_st.up_mbps : 0.0;
    double mb_down = g_st.has_down ? g_st.down_mbps : 0.0;

    if (g_st.has_up) {
        swprintf(line, 256,
                 L"%ls     %.2f Mbps    %.2f MB/s    %.2f s    %.2f GB",
                 dir_up_text(), mb_up, mb_up / 8.0, g_st.up_secs,
                 (double)g_st.up_bytes / 1e9);
    } else {
        swprintf(line, 256, L"%ls     --", dir_up_text());
    }
    draw_text(dc, line, S(36), y1, S(728), S(26), g_font_result,
              g_st.has_up ? COL_TEXT : COL_FAINT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (g_st.has_down) {
        swprintf(line, 256,
                 L"%ls     %.2f Mbps    %.2f MB/s    %.2f s    %.2f GB",
                 dir_down_text(), mb_down, mb_down / 8.0, g_st.down_secs,
                 (double)g_st.down_bytes / 1e9);
    } else {
        swprintf(line, 256, L"%ls     --", dir_down_text());
    }
    draw_text(dc, line, S(36), y2, S(728), S(26), g_font_result,
              g_st.has_down ? COL_TEXT : COL_FAINT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_status_line(HDC dc)
{
    WCHAR line[512];
    WCHAR edit_ip[64];
    WCHAR peer[64];
    WCHAR local[64];
    WCHAR suffix[96];
    const WCHAR *hint;

    GetWindowTextW(g_edit_ip, edit_ip, 64);
    if (g_peer_name[0])
        swprintf(suffix, 96, L"（%ls）", g_peer_name);
    else
        suffix[0] = 0;
    if (g_st.connected) {
        utf8_to_wide(g_st.peer_ip, peer, 64);
        utf8_to_wide(g_st.local_ip, local, 64);
        swprintf(line, 512, L"对方 %ls%ls    本机 %ls    本机为%ls    端口 %d-%d",
                 peer[0] ? peer : L"?", suffix,
                 local[0] ? local : L"?", role_name(),
                 PORT, PORT + STREAMS - 1);
    } else {
        swprintf(line, 512, L"对方 %ls%ls    本机 %ls    端口 %d-%d",
                 edit_ip[0] ? edit_ip : L"--", suffix,
                 g_local_hint[0] ? g_local_hint : L"--",
                 PORT, PORT + STREAMS - 1);
    }
    draw_text(dc, line, S(36), S(144), S(728), S(24), g_font_small, COL_DIM,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    switch (g_st.phase) {
    case LS_PHASE_CONNECTING:
        hint = g_role == LS_ROLE_PHONE
                ? L"本机为监听端：正在监听 5201-5204，等待对方连接。"
                : L"本机为连接端：正在连接对方 5201-5204，请确认对方已开始。";
        break;
    case LS_PHASE_HANDSHAKE:
        hint = L"TCP 已连接，正在交换参数。";
        break;
    case LS_PHASE_UPLINK:
        hint = g_role == LS_ROLE_PHONE
                ? L"第一阶段：本机持续发送 1 MiB 数据块。"
                : L"第一阶段：对方持续发送 1 MiB 数据块（本机接收）。";
        break;
    case LS_PHASE_DOWNLINK:
        hint = g_role == LS_ROLE_PHONE
                ? L"第二阶段：对方持续发送 1 MiB 数据块（本机接收）。"
                : L"第二阶段：本机持续发送 1 MiB 数据块。";
        break;
    case LS_PHASE_DONE:
        hint = L"结果以接收端实测为准；可再次点击“开始测速”重测。";
        break;
    case LS_PHASE_ERROR:
        hint = g_error_text[0] ? g_error_text : L"测速失败";
        break;
    default:
        hint = g_scan_text[0] ? g_scan_text
               : L"两台设备启动后自动互相发现并填入 IP；各自点击一次“开始测速”。";
        break;
    }
    draw_text(dc, hint, S(36), S(452), S(728), S(50), g_font_small,
              g_st.phase == LS_PHASE_ERROR ? COL_RED : COL_DIM,
              DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (g_st.err[0] && g_st.phase == LS_PHASE_DONE) {
        WCHAR werr[256];
        utf8_to_wide(g_st.err, werr, 256);
        draw_text(dc, werr, S(36), S(508), S(728), S(40), g_font_small,
                  COL_DIM, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old;
    WCHAR buf[64];
    double mbps;
    SIZE sz;
    int x, y;

    GetClientRect(hwnd, &rc);
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, g_bg_brush);

    draw_text(mem, L"局域网测速", S(36), S(16), S(500), S(40),
              g_font_title, COL_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(mem, L"任意两台设备（PC/手机/平板） · 1 MiB 数据块 · TCP 双缓冲 · 不写磁盘",
              S(38), S(56), S(728), S(24), g_font_sub, COL_DIM,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_text(mem, L"对方 IP", S(36), S(102), S(80), S(28),
              g_font_label, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_text(mem, phase_text(), S(36), S(184), S(500), S(30),
              g_font_phase, phase_color(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    mbps = display_mbps();
    swprintf(buf, 64, L"%.2f", mbps);
    draw_text(mem, buf, S(36), S(216), S(500), S(80),
              g_font_big, COL_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, g_font_big);
    GetTextExtentPoint32W(mem, buf, (int)wcslen(buf), &sz);
    x = S(36) + sz.cx + S(10);
    y = S(250);
    draw_text(mem, L"Mbps", x, y, S(120), S(32),
              g_font_unit, COL_ACCENT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    swprintf(buf, 64, L"≈ %.2f MB/s", mbps / 8.0);
    draw_text(mem, buf, S(38), S(300), S(500), S(26),
              g_font_result, COL_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_progress(mem);
    draw_results(mem);
    draw_status_line(mem);

    draw_text(mem, L"自动发现 · 端口 5201-5204 · 4 条 TCP 流 · 单方向 10 秒 · 日志默认关闭（勾选“日志”写 lanspeed_log.txt）",
              S(36), S(580), S(728), S(22), g_font_small, COL_FAINT,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void draw_owner_button(const DRAWITEMSTRUCT *dis)
{
    RECT r = dis->rcItem;
    WCHAR text[64];
    COLORREF bg, fg, border;
    HBRUSH br;
    HPEN pen;
    HGDIOBJ old_brush, old_pen;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    int pressed = (dis->itemState & ODS_SELECTED) != 0;

    FillRect(dis->hDC, &r, g_bg_brush);
    InflateRect(&r, -S(2), -S(2));

    if (dis->CtlID == ID_START) {
        bg = disabled ? RGB(43, 47, 56) : COL_ACCENT;
        fg = disabled ? RGB(112, 119, 132) : RGB(8, 22, 32);
        border = bg;
    } else {
        bg = pressed ? RGB(45, 50, 62) : COL_PANEL;
        fg = disabled ? RGB(105, 112, 126) : COL_TEXT;
        border = RGB(70, 77, 94);
    }
    if (pressed && dis->CtlID == ID_START && !disabled) {
        bg = RGB(42, 164, 210);
        border = bg;
    }

    br = CreateSolidBrush(bg);
    pen = CreatePen(PS_SOLID, 1, border);
    old_brush = SelectObject(dis->hDC, br);
    old_pen = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, r.left, r.top, r.right, r.bottom, S(10), S(10));
    SelectObject(dis->hDC, old_brush);
    SelectObject(dis->hDC, old_pen);
    DeleteObject(br);
    DeleteObject(pen);

    GetWindowTextW(dis->hwndItem, text, 64);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    SelectObject(dis->hDC, g_font_button);
    DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (dis->itemState & ODS_FOCUS)
        DrawFocusRect(dis->hDC, &r);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_bg_brush = CreateSolidBrush(COL_BG);
        g_panel_brush = CreateSolidBrush(COL_PANEL);
        g_edit_brush = CreateSolidBrush(COL_EDIT);
        g_track_brush = CreateSolidBrush(RGB(38, 42, 53));
        g_accent_brush = CreateSolidBrush(COL_ACCENT);
        make_fonts();

        g_edit_ip = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_EDIT_IP, g_inst, NULL);
        g_btn_scan = CreateWindowExW(0, L"BUTTON", L"扫描",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_SCAN, g_inst, NULL);
        g_btn_start = CreateWindowExW(0, L"BUTTON", L"开始测速",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_START, g_inst, NULL);
        g_btn_stop = CreateWindowExW(0, L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_STOP, g_inst, NULL);
        g_chk_pipeline = CreateWindowExW(0, L"BUTTON", L"流水线线程",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_PIPELINE, g_inst, NULL);
        if (g_chk_pipeline) {
            SendMessageW(g_chk_pipeline, WM_SETFONT, (WPARAM)g_font_small, TRUE);
            SendMessageW(g_chk_pipeline, BM_SETCHECK, BST_CHECKED, 0);
        }
        g_chk_log = CreateWindowExW(0, L"BUTTON", L"日志",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LOG, g_inst, NULL);
        if (g_chk_log)
            SendMessageW(g_chk_log, WM_SETFONT, (WPARAM)g_font_small, TRUE);

        SendMessageW(g_edit_ip, EM_SETLIMITTEXT, 15, 0);
        SendMessageW(g_edit_ip, WM_SETFONT, (WPARAM)g_font_phase, TRUE);
        ShowWindow(g_edit_ip, SW_SHOW);
        ShowWindow(g_btn_scan, SW_SHOW);
        ShowWindow(g_btn_start, SW_SHOW);
        ShowWindow(g_btn_stop, SW_SHOW);
        ShowWindow(g_chk_pipeline, SW_SHOW);
        ShowWindow(g_chk_log, SW_SHOW);
        layout_controls();
        set_ui_running(0);
        SetFocus(g_edit_ip);

        {
            DWORD n = 64;
            if (GetComputerNameA(g_self_name, &n) == 0)
                snprintf(g_self_name, sizeof(g_self_name), "pc");
        }
        ls_set_log_enabled(0);
        if (ls_disc_start(DISC_PORT, PORT, g_self_name) == 0)
            start_scan();
        SetTimer(hwnd, ID_TIMER_DISC, 1000, NULL);
        return 0;

    case WM_SIZE:
        layout_controls();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER) {
            poll_status(hwnd);
            return 0;
        }
        if (wp == ID_TIMER_DISC) {
            poll_discovery(hwnd);
            return 0;
        }
        break;

    case WM_APP_SCAN_DONE:
        {
            ls_peer_info peer;
            if (wp == 1 && ls_disc_latest(&peer) == 0) {
                fill_peer(peer.ip, peer.name, peer.id, 1);
                g_scan_text[0] = 0;
            } else {
                swprintf(g_scan_text, 96,
                         L"未发现其他设备：请确认对端也已打开本应用并在同一局域网。");
                g_scan_notfound = 1;
            }
            set_scan_ui(0, NULL);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_START && HIWORD(wp) == BN_CLICKED) {
            if (!g_running_ui)
                start_test(hwnd);
            return 0;
        }
        if (LOWORD(wp) == ID_STOP && HIWORD(wp) == BN_CLICKED) {
            if (g_running_ui)
                stop_test(hwnd, 1);
            return 0;
        }
        if (LOWORD(wp) == ID_SCAN && HIWORD(wp) == BN_CLICKED) {
            start_scan();
            return 0;
        }
        if (LOWORD(wp) == ID_LOG && HIWORD(wp) == BN_CLICKED) {
            ls_set_log_enabled(SendMessageW(g_chk_log, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (LOWORD(wp) == ID_EDIT_IP && HIWORD(wp) == EN_CHANGE) {
            if (!g_filling)
                g_ip_touched = 1;
            return 0;
        }
        break;

    case WM_DRAWITEM:
        draw_owner_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, IsWindowEnabled((HWND)lp) ? COL_TEXT : COL_FAINT);
        SetBkColor((HDC)wp, COL_EDIT);
        return (LRESULT)g_edit_brush;

    case WM_CTLCOLORSTATIC:
        if ((g_chk_pipeline && (HWND)lp == g_chk_pipeline) ||
            (g_chk_log && (HWND)lp == g_chk_log)) {
            SetTextColor((HDC)wp, IsWindowEnabled((HWND)lp) ? COL_DIM : COL_FAINT);
            SetBkColor((HDC)wp, COL_BG);
            return (LRESULT)g_bg_brush;
        }
        SetTextColor((HDC)wp, IsWindowEnabled((HWND)lp) ? COL_TEXT : COL_FAINT);
        SetBkColor((HDC)wp, COL_EDIT);
        return (LRESULT)g_edit_brush;

    case WM_CLOSE:
        stop_test(hwnd, 0);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        {
            int i;
            KillTimer(hwnd, ID_TIMER_DISC);
            ls_disc_stop();
            for (i = 0; i < MAX_STREAMS; i++) {
                if (g_sess[i]) {
                    ls_session_destroy(g_sess[i]);
                    g_sess[i] = NULL;
                }
            }
        }
        delete_fonts();
        if (g_bg_brush) { DeleteObject(g_bg_brush); g_bg_brush = NULL; }
        if (g_panel_brush) { DeleteObject(g_panel_brush); g_panel_brush = NULL; }
        if (g_edit_brush) { DeleteObject(g_edit_brush); g_edit_brush = NULL; }
        if (g_track_brush) { DeleteObject(g_track_brush); g_track_brush = NULL; }
        if (g_accent_brush) { DeleteObject(g_accent_brush); g_accent_brush = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    HDC screen;
    RECT wr;
    int cw, ch, x, y;
    DWORD style;

    (void)prev;
    (void)cmdline;
    g_inst = inst;

    SetProcessDPIAware();
    screen = GetDC(NULL);
    if (screen) {
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(NULL, screen);
    }
    if (g_dpi < 96)
        g_dpi = 96;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);

    cw = S(800);
    ch = S(616);
    style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    wr.left = 0; wr.top = 0; wr.right = cw; wr.bottom = ch;
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    x = (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    hwnd = CreateWindowExW(0, APP_CLASS, APP_TITLE, style,
                           x, y, wr.right - wr.left, wr.bottom - wr.top,
                           NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
