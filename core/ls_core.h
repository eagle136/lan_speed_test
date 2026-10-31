#ifndef LS_CORE_H
#define LS_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_DEFAULT_PORT     5201
#define LS_DEFAULT_SECONDS  10
#define LS_BLOCK_SIZE       (1024u * 1024u)

enum {
    LS_ROLE_PC    = 1,
    LS_ROLE_PHONE = 2
};

enum {
    LS_PHASE_IDLE       = 0,
    LS_PHASE_CONNECTING = 1,
    LS_PHASE_HANDSHAKE  = 2,
    LS_PHASE_UPLINK     = 3,  /* phone -> pc */
    LS_PHASE_DOWNLINK   = 4,  /* pc -> phone */
    LS_PHASE_DONE       = 5,
    LS_PHASE_ERROR      = 6
};

typedef struct {
    int running;
    int phase;
    int role;
    int connected;
    int finished;
    int error;
    char err[192];

    char peer_ip[64];
    char local_ip[64];
    int port;
    int seconds;

    double cur_mbps;
    double cur_mbps_raw;
    double progress;

    int has_up;
    int has_down;
    double up_mbps;
    double down_mbps;
    uint64_t up_bytes;
    uint64_t down_bytes;
    double up_secs;
    double down_secs;
} ls_status;

typedef struct ls_session ls_session;

ls_session *ls_session_create(int role, const char *peer_ip, int port, int seconds,
                              char *err, size_t err_len);
int  ls_session_start(ls_session *s);
void ls_session_stop(ls_session *s);
void ls_session_destroy(ls_session *s);
void ls_session_get_status(ls_session *s, ls_status *out);

/* Experimental: 0 = single I/O thread, 1 = producer thread + send I/O thread. */
void ls_session_set_pipeline(ls_session *s, int enabled);
void ls_set_log_enabled(int enabled);
void ls_log_printf(const char *fmt, ...);

/* Local address that would be used to reach peer_ip:port (no packet is sent). */
int ls_local_ip_for_peer(const char *peer_ip, int port, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
