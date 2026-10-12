/*
 * test_client.c - the phone-side engine (vc_client.c) against the real host
 * stack in the same process: network.c + session.c + the stub HID backend
 * on the main thread, the client on its own thread, real sockets on
 * localhost. This is the code the iOS and Android apps run.
 *
 * POSIX and C11 only (threads, atomics), so not built on Windows.
 *
 * Build:  cc -std=c11 -Iinclude -Itests tests/test_client.c src/vc_client.c src/encoder.c
 *            src/protocol.c src/network.c src/session.c src/parser.c src/hid_output.c -pthread
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "hid_output.h"
#include "network.h"
#include "session.h"
#include "vc_client.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------------ */
/* Host (main thread)                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    vc_net_t        *net;
    vc_sessions_t    sessions;
    int              states;      /* inputs accepted by the host */
    vc_input_state_t last;
} host_t;

static host_t g_host;

static void host_input(const vc_net_input_t *input, void *user)
{
    (void)user;
    vc_sessions_input(&g_host.sessions, input, now_ms());
}

static void host_connect(int conn, const vc_net_peer_t *peer, void *user)
{
    (void)user;
    vc_sessions_connected(&g_host.sessions, conn, peer, now_ms());
}

static void host_data(int conn, const uint8_t *data, size_t size, void *user)
{
    (void)user;
    vc_sessions_data(&g_host.sessions, conn, data, size, now_ms());
}

static void host_disconnect(int conn, void *user)
{
    (void)user;
    vc_sessions_disconnected(&g_host.sessions, conn, now_ms());
}

static int host_send(int conn, const uint8_t *data, size_t size, void *user)
{
    (void)user;
    return vc_net_send(g_host.net, conn, data, size);
}

static void host_close_conn(int conn, void *user)
{
    (void)user;
    vc_net_disconnect(g_host.net, conn);
}

static void host_state(int slot, const vc_input_state_t *state, void *user)
{
    (void)slot;
    (void)user;
    g_host.states++;
    g_host.last = *state;
}

static uint16_t host_open(void)
{
    vc_session_config_t scfg;
    vc_net_config_t     ncfg;

    memset(&g_host, 0, sizeof g_host);
    memset(&scfg, 0, sizeof scfg);
    scfg.send = host_send;
    scfg.disconnect = host_close_conn;
    scfg.on_state = host_state;
    CHECK(vc_sessions_init(&g_host.sessions, &scfg) == 0);

    memset(&ncfg, 0, sizeof ncfg);
    ncfg.bind_address = "127.0.0.1";
    ncfg.on_input = host_input;
    ncfg.tcp = 1;
    ncfg.on_connect = host_connect;
    ncfg.on_data = host_data;
    ncfg.on_disconnect = host_disconnect;
    CHECK(vc_net_open(&ncfg, &g_host.net) == 0);
    return vc_net_port(g_host.net);
}

static void host_close(void)
{
    vc_sessions_shutdown(&g_host.sessions);
    vc_net_close(g_host.net);
    g_host.net = NULL;
}

static void host_pump_once(void)
{
    if (g_host.net != NULL) {
        vc_net_poll(g_host.net, 5);
        vc_sessions_tick(&g_host.sessions, now_ms());
    }
}

static void host_pump(unsigned ms)
{
    uint64_t until = now_ms() + ms;
    while (now_ms() < until) {
        host_pump_once();
    }
}

/* Runs the host until `cond` holds or timeout_ms passes. */
#define PUMP_UNTIL(cond, timeout_ms) \
    do { \
        uint64_t until_ = now_ms() + (timeout_ms); \
        while (!(cond) && now_ms() < until_) { \
            host_pump_once(); \
        } \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Client (its own thread)                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t lock;
    int             connected;
    int             closed;
    uint8_t         slot;
    uint8_t         caps;
    vc_client_end_t end;
    uint8_t         error;
} events_t;

typedef struct {
    vc_client_t    *client;
    events_t        events;
    pthread_t       thread;
    vc_client_end_t result;
} run_t;

static void on_event(const vc_client_event_t *event, void *user)
{
    events_t *e = &((run_t *)user)->events;

    pthread_mutex_lock(&e->lock);
    if (event->type == VC_CLIENT_EVENT_CONNECTED) {
        e->connected++;
        e->slot = event->slot;
        e->caps = event->caps;
    } else if (event->type == VC_CLIENT_EVENT_CLOSED) {
        e->closed++;
        e->end = event->end;
        e->error = event->error;
        printf("  client closed: %s (%s)\n", vc_client_end_name(event->end), event->message);
    }
    pthread_mutex_unlock(&e->lock);
}

static int connected(run_t *r)
{
    int n;
    pthread_mutex_lock(&r->events.lock);
    n = r->events.connected;
    pthread_mutex_unlock(&r->events.lock);
    return n;
}

static int closed(run_t *r)
{
    int n;
    pthread_mutex_lock(&r->events.lock);
    n = r->events.closed;
    pthread_mutex_unlock(&r->events.lock);
    return n;
}

static void *client_thread(void *arg)
{
    run_t *r = (run_t *)arg;
    r->result = vc_client_run(r->client);
    return NULL;
}

static void client_config(vc_client_config_t *cfg, run_t *r, uint16_t port,
                          uint8_t type, uint8_t caps)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->host = "127.0.0.1";
    cfg->port = port;
    cfg->controller_type = type;
    cfg->caps = caps;
    cfg->name = "test-client";
    cfg->on_event = on_event;
    cfg->user = r;
}

static void start(run_t *r, const vc_client_config_t *cfg)
{
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->events.lock, NULL);
    r->client = vc_client_create(cfg);   /* cfg->user already points at r */
    CHECK(r->client != NULL);
    CHECK(pthread_create(&r->thread, NULL, client_thread, r) == 0);
}

static void finish(run_t *r)
{
    pthread_join(r->thread, NULL);
    vc_client_destroy(r->client);
    pthread_mutex_destroy(&r->events.lock);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                    */
/* ------------------------------------------------------------------------ */

static void test_config(void)
{
    vc_client_config_t cfg;

    CHECK(vc_client_create(NULL) == NULL);
    memset(&cfg, 0, sizeof cfg);
    CHECK(vc_client_create(&cfg) == NULL);   /* no host */
    cfg.host = "not-an-address";
    CHECK(vc_client_create(&cfg) == NULL);
    CHECK(strcmp(vc_client_end_name(VC_CLIENT_END_HOST_BYE), "host shut down") == 0);
}

/* Handshake, periodic and change-driven input, stats, stop with BYE. */
static void test_session(void)
{
    static run_t       r;
    vc_client_config_t cfg;
    vc_client_stats_t  stats;
    vc_input_state_t   s;
    uint16_t           port = host_open();
    int                before;

    client_config(&cfg, &r, port, VC_CONTROLLER_PS5, VC_CAP_MOTION | VC_CAP_TOUCH);
    start(&r, &cfg);

    PUMP_UNTIL(connected(&r) == 1, 3000);
    CHECK(connected(&r) == 1);
    CHECK(r.events.slot == 0);
    CHECK(r.events.caps == (VC_CAP_MOTION | VC_CAP_TOUCH));
    CHECK(vc_sessions_active(&g_host.sessions) == 1);

    /* Nothing touched: the neutral state is re-sent at 120 Hz, which also
     * keeps the host from releasing or closing the pad. */
    host_pump(100);
    before = g_host.states;
    host_pump(500);
    CHECK(g_host.states - before >= 30);   /* ~60 expected; generous for CI */

    /* A full state, motion and touch included, arrives intact. */
    memset(&s, 0, sizeof s);
    s.buttons = VC_BTN_SOUTH | VC_BTN_DPAD_LEFT;
    s.left.x = 1234;
    s.right.y = -32768;
    s.trigger_right = 200;
    s.flags = VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH;
    s.motion.accel[2] = 4096;
    s.touch[0].active = 1;
    s.touch[0].id = 3;
    s.touch[0].x = 40000;
    vc_client_set_state(r.client, &s);
    PUMP_UNTIL(g_host.last.buttons == s.buttons, 1000);
    CHECK(g_host.last.buttons == s.buttons);
    CHECK(g_host.last.left.x == 1234 && g_host.last.right.y == -32768);
    CHECK(g_host.last.trigger_right == 200);
    CHECK(g_host.last.flags == (VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH));
    CHECK(g_host.last.motion.accel[2] == 4096);
    CHECK(g_host.last.touch[0].active == 1 && g_host.last.touch[0].x == 40000);

    host_pump(1200);   /* long enough for a client PING round trip */
    vc_client_get_stats(r.client, &stats);
    CHECK(stats.sent >= 100);
    CHECK(stats.send_errors == 0);
    CHECK(stats.rtt_us > 0 && stats.rtt_us < 1000000u);

    /* stop(): BYE, and the host frees the slot. */
    vc_client_stop(r.client);
    PUMP_UNTIL(closed(&r) == 1 && vc_sessions_active(&g_host.sessions) == 0, 2000);
    finish(&r);
    CHECK(r.result == VC_CLIENT_END_STOPPED);
    CHECK(r.events.closed == 1 && r.events.end == VC_CLIENT_END_STOPPED);
    CHECK(vc_sessions_active(&g_host.sessions) == 0);
    host_close();
}

/* With a 1 Hz re-send, only change-driven sending can deliver an input
 * within a few milliseconds; and flags beyond the granted caps are cleared
 * on the phone, before they cost bandwidth. */
static void test_change_driven(void)
{
    static run_t       r;
    vc_client_config_t cfg;
    vc_input_state_t   s;
    uint16_t           port = host_open();
    uint64_t           t0;
    uint64_t           latency;
    int                before;

    client_config(&cfg, &r, port, VC_CONTROLLER_PS5, VC_CAP_TOUCH);   /* no motion offered */
    cfg.rate_hz = 1;
    start(&r, &cfg);
    PUMP_UNTIL(connected(&r) == 1, 3000);
    CHECK(connected(&r) == 1 && r.events.caps == VC_CAP_TOUCH);

    host_pump(300);   /* past the first send at connect */
    before = g_host.states;
    memset(&s, 0, sizeof s);
    s.buttons = VC_BTN_EAST;
    s.flags = VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH;
    s.motion.gyro[0] = 99;
    t0 = now_ms();
    vc_client_set_state(r.client, &s);
    PUMP_UNTIL(g_host.states > before, 900);
    latency = now_ms() - t0;
    printf("  change-driven latency on localhost: %lu ms\n", (unsigned long)latency);
    CHECK(g_host.states > before);
    CHECK(latency < 200);
    CHECK(g_host.last.buttons == VC_BTN_EAST);
    CHECK(g_host.last.flags == VC_INPUT_HAS_TOUCH);   /* motion masked by the phone */
    CHECK(g_host.last.motion.gyro[0] == 0);

    /* Host shutdown reaches the phone as BYE. */
    vc_sessions_shutdown(&g_host.sessions);
    PUMP_UNTIL(closed(&r) == 1, 2000);
    finish(&r);
    CHECK(r.result == VC_CLIENT_END_HOST_BYE);
    host_close();
}

static void test_refused(void)
{
    static run_t       r;
    vc_client_config_t cfg;
    uint16_t           port = host_open();

    client_config(&cfg, &r, port, 9, 0);   /* no such controller type */
    start(&r, &cfg);
    PUMP_UNTIL(closed(&r) == 1, 3000);
    finish(&r);
    CHECK(r.result == VC_CLIENT_END_REFUSED);
    CHECK(r.events.connected == 0);
    CHECK(r.events.error == VC_ERROR_UNSUPPORTED);
    host_close();
}

static void test_no_host(void)
{
    static run_t       r;
    vc_client_config_t cfg;
    uint16_t           port = host_open();

    host_close();   /* the port is free again: nobody listens */
    client_config(&cfg, &r, port, VC_CONTROLLER_XBOX, 0);
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.events.lock, NULL);
    r.client = vc_client_create(&cfg);
    CHECK(r.client != NULL);
    CHECK(vc_client_run(r.client) == VC_CLIENT_END_CONNECT_FAILED);   /* on this thread */
    CHECK(r.events.closed == 1 && r.events.connected == 0);
    vc_client_destroy(r.client);
    pthread_mutex_destroy(&r.events.lock);
}

int main(void)
{
    /* The host side writes to sockets the client may have closed. */
    signal(SIGPIPE, SIG_IGN);
    CHECK(vc_hid_init() == 0);
    test_config();
    test_session();
    test_change_driven();
    test_refused();
    test_no_host();
    vc_hid_shutdown();
    return test_summary();
}
