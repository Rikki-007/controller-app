/*
 * vc_client.h - the phone side of the protocol: one controller's link to vcd.
 *
 * Shared by the iOS app (through the VCCore Swift package) and the Android
 * app (through JNI), so the handshake, keepalive, sequencing and wire
 * format are written and tested once. The app shells only own the thread,
 * its priority, and the UI.
 *
 * Threads:
 *
 *   sender thread  The app creates it at high priority and calls
 *                  vc_client_run(), which connects, handshakes, then sends
 *                  until stopped or the host ends the session. on_event
 *                  callbacks run here.
 *   input thread   ONE thread (the UI thread) calls vc_client_set_state().
 *                  It never blocks: states are handed over through a
 *                  lock-free triple buffer, and the sender always sends the
 *                  newest one.
 *   any thread     vc_client_stop(), vc_client_get_stats().
 *
 * Sending: every input change is sent at once (but no closer than
 * min_interval_us to the previous datagram), and the current state is
 * re-sent at rate_hz regardless, so a lost datagram costs at most one
 * period. With rate_only set it sends strictly at rate_hz.
 *
 * POSIX sockets, C11 atomics. Builds on iOS, Android, Linux and macOS; not
 * on Windows (vcd's own platform), where nothing needs it.
 */
#ifndef VC_CLIENT_H
#define VC_CLIENT_H

#include <stdint.h>

#include "controller.h"

typedef struct vc_client vc_client_t;

/* Why vc_client_run() returned. */
typedef enum {
    VC_CLIENT_END_STOPPED = 0,      /* vc_client_stop(); BYE was sent */
    VC_CLIENT_END_CONNECT_FAILED,   /* no TCP connection (wrong address, vcd not running) */
    VC_CLIENT_END_REFUSED,          /* ERROR answered HELLO; see `error` */
    VC_CLIENT_END_HOST_ERROR,       /* ERROR during the session; see `error` */
    VC_CLIENT_END_HOST_BYE,         /* vcd shut down */
    VC_CLIENT_END_CONNECTION_LOST,  /* TCP closed or failed */
    VC_CLIENT_END_PROTOCOL,         /* the host sent something malformed */
    VC_CLIENT_END_TIMEOUT,          /* no answer to HELLO in time */
    VC_CLIENT_END_SYSTEM            /* a socket or pipe could not be created */
} vc_client_end_t;

typedef enum {
    VC_CLIENT_EVENT_CONNECTED = 1,  /* WELCOME received; input is flowing */
    VC_CLIENT_EVENT_FEEDBACK,       /* rumble / light bar from the host */
    VC_CLIENT_EVENT_CLOSED          /* last event of a run */
} vc_client_event_type_t;

typedef struct {
    vc_client_event_type_t type;
    /* CONNECTED */
    uint8_t              slot;          /* 0..3: player slot - 1 */
    uint8_t              caps;          /* VC_CAP_* granted */
    uint16_t             timeout_ms;    /* host's input timeout */
    /* FEEDBACK */
    vc_packet_feedback_t feedback;
    /* CLOSED */
    vc_client_end_t      end;
    uint8_t              error;         /* vc_error_code_t for REFUSED / HOST_ERROR, else 0 */
    const char          *message;       /* human-readable; valid during the callback */
} vc_client_event_t;

typedef struct {
    const char *host;               /* vcd's dotted IPv4 address (copied) */
    uint16_t    port;               /* 0: VC_DEFAULT_PORT */
    uint8_t     controller_type;    /* vc_controller_type_t */
    uint8_t     caps;               /* VC_CAP_* this phone can provide */
    const char *name;               /* shown in vcd's log; copied, max 32 bytes */

    unsigned    rate_hz;            /* re-send rate; 0: 120 */
    unsigned    min_interval_us;    /* spacing of change-driven sends; 0: 2000 */
    int         rate_only;          /* 1: no change-driven sends */
    unsigned    connect_timeout_ms; /* TCP connect and HELLO answer; 0: 3000 */

    /* Called on the sender thread. Keep it short: copy, then post to the
     * UI thread. */
    void      (*on_event)(const vc_client_event_t *event, void *user);
    void       *user;
} vc_client_config_t;

typedef struct {
    uint32_t sent;          /* input datagrams sent */
    uint32_t send_errors;   /* dropped by the OS (buffer full, ICMP errors) */
    uint32_t rtt_us;        /* last TCP PING round trip; 0 before the first */
} vc_client_stats_t;

/* Copies the config. Returns NULL if it is invalid (no host, bad address)
 * or memory or a pipe cannot be had. */
vc_client_t *vc_client_create(const vc_client_config_t *config);

/* Connects, handshakes and sends until stopped or the session ends; returns
 * why. Blocks: call it on the sender thread. Can be called again after it
 * returns, for a new session. */
vc_client_end_t vc_client_run(vc_client_t *client);

/* Publishes the controller's current state. Input thread only; never
 * blocks. `flags` beyond the granted caps are cleared before sending;
 * timestamp_us is filled in by the sender. */
void vc_client_set_state(vc_client_t *client, const vc_input_state_t *state);

/* Asks vc_client_run() to send BYE and return. Any thread; returns at once. */
void vc_client_stop(vc_client_t *client);

void vc_client_get_stats(const vc_client_t *client, vc_client_stats_t *out);

/* Frees the client. vc_client_run() must have returned. NULL is ignored. */
void vc_client_destroy(vc_client_t *client);

/* Name of an end reason, e.g. "connection lost". */
const char *vc_client_end_name(vc_client_end_t end);

#endif /* VC_CLIENT_H */
