/*
 * network.h - UDP listener for input packets from the mobile client.
 *
 * Binds one non-blocking IPv4 UDP socket, validates and unpacks every
 * VC_MSG_INPUT datagram (see controller.h) and hands the result to a
 * callback. Malformed datagrams are counted, logged and dropped.
 *
 * The listener owns no thread. Either call vc_net_poll() from an existing
 * loop, or call vc_net_run() on a thread of your own and end it with
 * vc_net_stop() from any other thread.
 *
 * This layer does not know about sessions: checking the token and rejecting
 * stale sequence numbers is the caller's job, using the session's parser
 * (see parser.h).
 */
#ifndef VC_NETWORK_H
#define VC_NETWORK_H

#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "parser.h"

/* ------------------------------------------------------------------------ */
/* Decoded input                                                            */
/* ------------------------------------------------------------------------ */

/* IPv4 sender of a datagram. ip[0] is the first octet. */
typedef struct {
    uint8_t  ip[4];
    uint16_t port;   /* host byte order */
} vc_net_peer_t;

/* One VC_MSG_INPUT datagram, unpacked by vc_parser_decode(). Pass `packet`
 * to the session's vc_parser_accept() for the token, sequence and profile
 * steps. */
typedef struct {
    vc_parser_packet_t packet;
    vc_net_peer_t      peer;
} vc_net_input_t;

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum {
    VC_NET_LOG_INFO = 0,
    VC_NET_LOG_WARN,
    VC_NET_LOG_ERROR
} vc_net_log_level_t;

typedef void (*vc_net_log_cb)(vc_net_log_level_t level, const char *message,
                              void *user);

/* Redirects log output; NULL restores the default, which writes to stderr.
 * Process-wide. Set it before opening a listener, not while one is running. */
void vc_net_set_log(vc_net_log_cb callback, void *user);

/* ------------------------------------------------------------------------ */
/* Listener                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct vc_net vc_net_t;

/* Called on the thread that runs vc_net_poll() or vc_net_run(). The input is
 * only valid for the duration of the call. */
typedef void (*vc_net_input_cb)(const vc_net_input_t *input, void *user);

typedef struct {
    const char     *bind_address;   /* dotted IPv4; NULL binds all interfaces */
    uint16_t        port;           /* 0 lets the OS choose, see vc_net_port() */
    vc_net_input_cb on_input;       /* required */
    void           *user;           /* passed to on_input */
} vc_net_config_t;

typedef struct {
    uint64_t datagrams_received;
    uint64_t inputs_delivered;
    uint64_t dropped_malformed;
    uint64_t dropped_oversized;
    uint64_t receive_errors;
} vc_net_stats_t;

/* Opens and binds the listener. Returns 0 and sets *out, or -1 after logging
 * the reason. */
int vc_net_open(const vc_net_config_t *config, vc_net_t **out);

/* Waits up to timeout_ms (0 returns at once, negative waits forever) and
 * delivers the datagrams already queued, up to an internal batch limit.
 * Returns the number of inputs delivered, or -1 if the socket is unusable. */
int vc_net_poll(vc_net_t *net, int timeout_ms);

/* Polls until vc_net_stop() is called. Returns 0 when stopped, -1 if the
 * socket became unusable. */
int vc_net_run(vc_net_t *net);

/* Asks vc_net_run() to return; it does so within about 100 ms. Safe to call
 * from another thread or from inside the input callback. */
void vc_net_stop(vc_net_t *net);

/* Port actually bound, in host byte order. */
uint16_t vc_net_port(const vc_net_t *net);

/* Copies the counters. Call from the polling thread, or after it has ended. */
void vc_net_get_stats(const vc_net_t *net, vc_net_stats_t *out);

/* Closes the socket and frees the listener. NULL is ignored. Not safe while
 * another thread is still inside vc_net_poll() or vc_net_run(). */
void vc_net_close(vc_net_t *net);

#endif /* VC_NETWORK_H */
