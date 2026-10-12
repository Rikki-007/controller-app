/*
 * network.h - sockets for the mobile clients: UDP input and TCP control.
 *
 * Binds one non-blocking IPv4 UDP socket, validates and unpacks every
 * VC_MSG_INPUT datagram (see controller.h) and hands the result to a
 * callback. Malformed datagrams are counted, logged and dropped.
 *
 * With `tcp` set in the config it also listens for TCP control connections
 * on the same port (up to VC_NET_MAX_CONNS at once), delivers their bytes
 * unframed, and sends bytes back; framing is protocol.h's job.
 *
 * The listener owns no thread. Either call vc_net_poll() from an existing
 * loop, or call vc_net_run() on a thread of your own and end it with
 * vc_net_stop() from any other thread.
 *
 * This layer does not know about sessions: checking the token and rejecting
 * stale sequence numbers is the caller's job, using the session's parser
 * (see parser.h), and so is deciding what a connection may do (session.h).
 *
 * On Windows the UDP socket ignores the ICMP "port unreachable" errors that
 * Windows otherwise reports on the next receive (SIO_UDP_CONNRESET), so
 * sending to a client that has gone away cannot break input from the others.
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

/* Most TCP control connections open at once. More than the daemon's slots,
 * so a surplus client can still be told "no free slot" instead of being
 * refused at the socket level. */
#define VC_NET_MAX_CONNS 8

/* Callbacks run on the thread that runs vc_net_poll() or vc_net_run(), and
 * their arguments are only valid for the duration of the call. Inside any of
 * them it is safe to call vc_net_send(), vc_net_send_to() and
 * vc_net_disconnect(). */
typedef void (*vc_net_input_cb)(const vc_net_input_t *input, void *user);

/* A control connection was accepted. `conn` is 0..VC_NET_MAX_CONNS-1 and
 * names it until on_disconnect, or until the caller's vc_net_disconnect(). */
typedef void (*vc_net_connect_cb)(int conn, const vc_net_peer_t *peer, void *user);

/* Bytes arrived on a control connection, in order, split arbitrarily. */
typedef void (*vc_net_data_cb)(int conn, const uint8_t *data, size_t size, void *user);

/* The peer closed the connection or it failed. Not called for connections
 * the caller closed with vc_net_disconnect(), nor by vc_net_close(). */
typedef void (*vc_net_disconnect_cb)(int conn, void *user);

typedef struct {
    const char          *bind_address;   /* dotted IPv4; NULL binds all interfaces */
    uint16_t             port;           /* 0 lets the OS choose, see vc_net_port() */
    vc_net_input_cb      on_input;       /* required */
    int                  tcp;            /* also accept control connections on the port */
    vc_net_connect_cb    on_connect;     /* required if tcp */
    vc_net_data_cb       on_data;        /* required if tcp */
    vc_net_disconnect_cb on_disconnect;  /* required if tcp */
    void                *user;           /* passed to every callback */
} vc_net_config_t;

typedef struct {
    uint64_t datagrams_received;
    uint64_t inputs_delivered;
    uint64_t dropped_malformed;
    uint64_t dropped_oversized;
    uint64_t receive_errors;
    uint64_t icmp_errors_ignored;   /* "port unreachable" and similar, see above */
    uint64_t connections_accepted;
    uint64_t connections_refused;   /* all VC_NET_MAX_CONNS in use */
} vc_net_stats_t;

/* Opens and binds the listener (UDP, and TCP on the same port if asked).
 * Returns 0 and sets *out, or -1 after logging the reason. */
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

/* Queues bytes on a control connection; what the socket cannot take at once
 * is sent from later polls. Returns 0, or -1 if `conn` is not open, the
 * connection has failed, or more than about 1 KiB is already waiting (a peer
 * that stops reading). On -1 the connection is unusable: close it with
 * vc_net_disconnect(). */
int vc_net_send(vc_net_t *net, int conn, const uint8_t *data, size_t size);

/* Sends anything still queued (best effort, without waiting), then closes
 * the connection. on_disconnect is not called. Ignores a closed `conn`. */
void vc_net_disconnect(vc_net_t *net, int conn);

/* Sends one datagram from the listening UDP socket, e.g. feedback to a
 * client's input address. Returns 0, or -1 after logging (rate-limited). */
int vc_net_send_to(vc_net_t *net, const vc_net_peer_t *peer, const uint8_t *data,
                   size_t size);

/* Copies the counters. Call from the polling thread, or after it has ended. */
void vc_net_get_stats(const vc_net_t *net, vc_net_stats_t *out);

/* Closes every socket and frees the listener. NULL is ignored. Not safe while
 * another thread is still inside vc_net_poll() or vc_net_run(). */
void vc_net_close(vc_net_t *net);

#endif /* VC_NETWORK_H */
