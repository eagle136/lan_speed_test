#include "ls_core.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
static void wait_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void wait_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static const char *phase_name(int p)
{
    switch (p) {
    case LS_PHASE_IDLE:       return "IDLE";
    case LS_PHASE_CONNECTING: return "CONNECTING";
    case LS_PHASE_HANDSHAKE:  return "HANDSHAKE";
    case LS_PHASE_UPLINK:     return "UPLINK";
    case LS_PHASE_DOWNLINK:   return "DOWNLINK";
    case LS_PHASE_DONE:       return "DONE";
    case LS_PHASE_ERROR:      return "ERROR";
    default:                  return "?";
    }
}

int main(int argc, char **argv)
{
    char err[192];
    const int port = 45201;
    const int secs = 2;
    int pipeline = 1;
    ls_session *phone;
    ls_session *pc;
    ls_status ps, cs;
    int i;
    int ok = 0;

    if (argc > 1 && argv[1][0] == '0')
        pipeline = 0;
    printf("pipeline=%d\n", pipeline);

    phone = ls_session_create(LS_ROLE_PHONE, "127.0.0.1", port, secs,
                              err, sizeof(err));
    if (!phone) {
        printf("FAIL create phone: %s\n", err);
        return 1;
    }
    ls_session_set_pipeline(phone, pipeline);
    if (ls_session_start(phone) != 0) {
        printf("FAIL start phone\n");
        ls_session_destroy(phone);
        return 1;
    }
    wait_ms(100);

    pc = ls_session_create(LS_ROLE_PC, "127.0.0.1", port, secs,
                           err, sizeof(err));
    if (!pc) {
        printf("FAIL create pc: %s\n", err);
        ls_session_stop(phone);
        ls_session_destroy(phone);
        return 1;
    }
    ls_session_set_pipeline(pc, pipeline);
    if (ls_session_start(pc) != 0) {
        printf("FAIL start pc\n");
        ls_session_destroy(pc);
        ls_session_destroy(phone);
        return 1;
    }

    for (i = 0; i < 400; i++) {
        ls_session_get_status(phone, &ps);
        ls_session_get_status(pc, &cs);
        if ((i % 5) == 0) {
            printf("t=%4d ms  phone: %-10s %8.2f Mbps p=%.2f | pc: %-10s %8.2f Mbps p=%.2f\n",
                   i * 100, phase_name(ps.phase), ps.cur_mbps, ps.progress,
                   phase_name(cs.phase), cs.cur_mbps, cs.progress);
        }
        if (ps.phase == LS_PHASE_ERROR || cs.phase == LS_PHASE_ERROR)
            break;
        if (ps.phase == LS_PHASE_DONE && cs.phase == LS_PHASE_DONE)
            break;
        wait_ms(100);
    }

    ls_session_get_status(phone, &ps);
    ls_session_get_status(pc, &cs);

    printf("\nphone: phase=%s err='%s'\n", phase_name(ps.phase), ps.err);
    printf("pc   : phase=%s err='%s'\n", phase_name(cs.phase), cs.err);
    printf("phone result: up=%d %.3f Mbps (%.6f s, %llu B) down=%d %.3f Mbps (%.6f s, %llu B)\n",
           ps.has_up, ps.up_mbps, ps.up_secs,
           (unsigned long long)ps.up_bytes,
           ps.has_down, ps.down_mbps, ps.down_secs,
           (unsigned long long)ps.down_bytes);
    printf("pc    result: up=%d %.3f Mbps (%.6f s, %llu B) down=%d %.3f Mbps (%.6f s, %llu B)\n",
           cs.has_up, cs.up_mbps, cs.up_secs,
           (unsigned long long)cs.up_bytes,
           cs.has_down, cs.down_mbps, cs.down_secs,
           (unsigned long long)cs.down_bytes);

    if (ps.phase == LS_PHASE_DONE && cs.phase == LS_PHASE_DONE &&
        ps.has_up && ps.has_down && cs.has_up && cs.has_down &&
        ps.up_bytes > 0 && ps.down_bytes > 0 &&
        cs.up_bytes > 0 && cs.down_bytes > 0) {
        double du = ps.up_mbps - cs.up_mbps;
        double dd = ps.down_mbps - cs.down_mbps;
        if (du < 0) du = -du;
        if (dd < 0) dd = -dd;
        printf("result delta: up=%.3f Mbps down=%.3f Mbps\n", du, dd);
        if (du < 0.01 && dd < 0.01)
            ok = 1;
    }

    ls_session_stop(pc);
    ls_session_stop(phone);
    ls_session_destroy(pc);
    ls_session_destroy(phone);

    printf(ok ? "SELFTEST OK\n" : "SELFTEST FAILED\n");
    return ok ? 0 : 1;
}
