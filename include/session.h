/*
 * session.h - control-channel sessions and controller slots.
 *
 * A client becomes a controller in two steps:
 *
 *   1. TCP: it connects and sends HELLO (controller type, capabilities,
 *      name). If a slot is free and the backend can present that type, the
 *      host plugs in a virtual gamepad and answers WELCOME with a random
 *      session token, the slot and the granted capabilities. Otherwise it
 *      answers ERROR and closes.
 *   2. UDP: it streams INPUT datagrams carrying that token, from the same
 *      IP address as its TCP connection.
 *
 * The session ends, the pad is released and unplugged and the slot freed,
 * when any of these happens:
 *
 *   client sends BYE                          "client said bye"
 *   TCP connection closes or fails            "connection lost"
 *   ping_misses PINGs go unanswered           "keepalive timeout"     ERROR TIMEOUT
 *   no input for idle_timeout_ms              "no input for ... ms"   ERROR TIMEOUT
 *   a malformed or unexpected message         "protocol error: ..."   ERROR PROTOCOL
 *   the virtual gamepad rejects input         "driver error"          ERROR DRIVER
 *
 * Shorter input silences (input_timeout_ms) only release every control
 * until input resumes. A connection that sends no HELLO within
 * hello_timeout_ms is closed with ERROR TIMEOUT.
 *
 * No sockets and no clock: the caller feeds network events in, passes the
 * current time to every call, and provides send/disconnect callbacks, so
 * the whole lifecycle can be tested with a fake clock (tests/test_session.c).
 * Single-threaded.
 */
#ifndef VC_SESSION_H
#define VC_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "controller.h"
#include "hid_output.h"
#include "network.h"
#include "parser.h"
#include "protocol.h"

#define VC_MAX_SLOTS 4

/* Defaults for the timing fields of vc_session_config_t left at 0. */
#define VC_SESSION_INPUT_TIMEOUT_MS  250
#define VC_SESSION_IDLE_TIMEOUT_MS   10000
#define VC_SESSION_HELLO_TIMEOUT_MS  5000
#define VC_SESSION_PING_INTERVAL_MS  1000
#define VC_SESSION_PING_MISSES       3

typedef struct {
    /* Transport, required. `conn` is a network.h connection index. send()
     * returns 0 or -1; after -1 the session is closed. disconnect() must not
     * call back into this module. */
    int  (*send)(int conn, const uint8_t *data, size_t size, void *user);
    void (*disconnect)(int conn, void *user);

    /* Optional. random32 issues session tokens; without it a fixed-seed
     * generator is used, which is predictable and only fit for tests. */
    uint32_t (*random32)(void *user);
    void     (*log)(const char *message, void *user);
    /* After every input accepted and passed to the virtual gamepad. */
    void     (*on_state)(int slot, const vc_input_state_t *state, void *user);
    void      *user;

    /* Timing in ms; 0 selects the VC_SESSION_* default. */
    unsigned input_timeout_ms;   /* silence that releases all controls */
    unsigned idle_timeout_ms;    /* silence that ends the session */
    unsigned hello_timeout_ms;   /* time a new connection has to send HELLO */
    unsigned ping_interval_ms;   /* host PING period */
    unsigned ping_misses;        /* unanswered PINGs that end the session */
} vc_session_config_t;

/* Everything below is caller-allocated; treat the fields as private and use
 * the functions. */

typedef struct {
    int               open;
    vc_net_peer_t     peer;
    uint64_t          connected_ms;
    vc_proto_reader_t reader;
    int               slot;          /* -1 until WELCOME */
    uint16_t          tx_sequence;   /* header sequence of our next message */
} vc_session_conn_t;

typedef struct {
    int                  in_use;
    int                  conn;
    uint32_t             token;
    vc_controller_type_t type;
    uint8_t              caps;                          /* granted */
    char                 name[VC_CLIENT_NAME_LEN + 1];  /* printable, for logs */
    vc_parser_t          parser;
    vc_hid_device_t     *device;
    int                  has_udp_peer;
    vc_net_peer_t        udp_peer;                      /* where input comes from */
    uint64_t             last_input_ms;
    int                  neutral;                       /* released, awaiting input */
    uint64_t             last_ping_ms;
    uint32_t             ping_echo;                     /* echo of the last PING sent */
    unsigned             pings_unanswered;
    int                  rtt_ms;                        /* -1 until the first PONG */
    uint64_t             wrong_address;                 /* inputs from another IP */
} vc_session_slot_t;

typedef struct {
    vc_session_config_t cfg;
    vc_session_conn_t   conns[VC_NET_MAX_CONNS];
    vc_session_slot_t   slots[VC_MAX_SLOTS];
    uint32_t            rng_state;
    uint32_t            ping_counter;
    uint64_t            unknown_token;    /* inputs with no matching session */
    uint64_t            refused;          /* HELLOs answered with ERROR */
} vc_sessions_t;

/* Returns 0, or -1 if a required callback is missing. vc_hid_init() must
 * have succeeded before the first HELLO arrives. */
int vc_sessions_init(vc_sessions_t *s, const vc_session_config_t *cfg);

/* Network events, to be called from the matching network.h callbacks. */
void vc_sessions_connected(vc_sessions_t *s, int conn, const vc_net_peer_t *peer,
                           uint64_t now_ms);
void vc_sessions_data(vc_sessions_t *s, int conn, const uint8_t *data, size_t size,
                      uint64_t now_ms);
void vc_sessions_disconnected(vc_sessions_t *s, int conn, uint64_t now_ms);
void vc_sessions_input(vc_sessions_t *s, const vc_net_input_t *input, uint64_t now_ms);

/* Timeouts and keepalive. Call at least every 50 ms or so: the input
 * timeout is only as precise as this. */
void vc_sessions_tick(vc_sessions_t *s, uint64_t now_ms);

/* Sends BYE to every client, releases and unplugs every pad, and closes
 * every connection. */
void vc_sessions_shutdown(vc_sessions_t *s);

/* Slots in use. */
int vc_sessions_active(const vc_sessions_t *s);

#endif /* VC_SESSION_H */
