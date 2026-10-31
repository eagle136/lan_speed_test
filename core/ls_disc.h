#ifndef LS_DISC_H
#define LS_DISC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port used by LAN discovery beacons (independent from TCP 5201..5204). */
#define LS_DISC_DEFAULT_PORT 5310
#define LS_DISC_MAX_PEERS    8

typedef struct {
    char ip[64];
    char name[64];
    char id[40];
    int  tcp_port;
    long long last_ms;   /* monotonic ms of the last time this peer was heard */
} ls_peer_info;

/* Start the passive listener and send one startup beacon. Returns 0 on success. */
int  ls_disc_start(int udp_port, int tcp_port, const char *name);
void ls_disc_stop(void);

/* One active discovery round: broadcast query + /24 unicast sweep, up to timeout_ms.
 * Returns 0 and fills *out when a peer was heard during (or just before) the round. */
int  ls_disc_scan(int timeout_ms, ls_peer_info *out);

/* Most recently heard peer (passive or from a scan). Returns 0 on success. */
int  ls_disc_latest(ls_peer_info *out);
int  ls_disc_peer_count(void);
int  ls_disc_peer_at(int index, ls_peer_info *out);
int  ls_disc_self_id(char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif
