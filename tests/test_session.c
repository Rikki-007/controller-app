/*
 * test_session.c - the session lifecycle against a fake transport and a
 * fake clock: handshake, slot limit, token routing, keepalive, timeouts,
 * protocol errors and every way a session ends.
 *
 * Build:  cc -std=c99 -Iinclude -Itests src/session.c src/protocol.c src/parser.c src/hid_output.c tests/test_session.c
 * (no backend flag: the stub HID backend, so no driver is needed)
 */
#include "session.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

/* ------------------------------------------------------------------------ */
/* Fake transport                                                           */
/* ------------------------------------------------------------------------ */

#define MAX_SEEN 64

typedef struct {
    vc_proto_reader_t reader[VC_NET_MAX_CONNS];
    vc_proto_msg_t    seen[VC_NET_MAX_CONNS][MAX_SEEN];   /* host -> client */
    int               count[VC_NET_MAX_CONNS];
    int               disconnects[VC_NET_MAX_CONNS];
    int               fail_send;
    int               states;
    int               last_state_slot;
    char              last_log[256];
    int               logs;
    const uint32_t   *tokens;      /* random32 output, in order */
    int               next_token;
} fake_t;

static fake_t g_fake;

static int fake_send(int conn, const uint8_t *data, size_t size, void *user)
{
    fake_t         *f = (fake_t *)user;
    vc_proto_msg_t  msg;

    CHECK(conn >= 0 && conn < VC_NET_MAX_CONNS);
    CHECK(f->disconnects[conn] == 0);   /* never write to a closed connection */
    if (f->fail_send) {
        return -1;
    }
    while (vc_proto_read(&f->reader[conn], &data, &size, &msg) == VC_PROTO_OK) {
        if (f->count[conn] < MAX_SEEN) {
            f->seen[conn][f->count[conn]] = msg;
        }
        f->count[conn]++;
    }
    CHECK(size == 0);
    return 0;
}

static void fake_disconnect(int conn, void *user)
{
    ((fake_t *)user)->disconnects[conn]++;
}

static uint32_t fake_random(void *user)
{
    fake_t *f = (fake_t *)user;
    return f->tokens[f->next_token++];
}

static void fake_log(const char *message, void *user)
{
    fake_t *f = (fake_t *)user;
    strncpy(f->last_log, message, sizeof f->last_log - 1);
    f->logs++;
}

static void fake_state(int slot, const vc_input_state_t *state, void *user)
{
    fake_t *f = (fake_t *)user;
    (void)state;
    f->states++;
    f->last_state_slot = slot;
}

/* Last message the host sent on `conn`, or NULL. */
static const vc_proto_msg_t *last_msg(int conn)
{
    int n = g_fake.count[conn];
    return n > 0 && n <= MAX_SEEN ? &g_fake.seen[conn][n - 1] : NULL;
}

static int last_type(int conn)
{
    const vc_proto_msg_t *m = last_msg(conn);
    return m != NULL ? m->type : -1;
}

/* ------------------------------------------------------------------------ */
/* Client side helpers                                                      */
/* ------------------------------------------------------------------------ */

static vc_net_peer_t peer_for(int conn)
{
    vc_net_peer_t peer;
    peer.ip[0] = 192;
    peer.ip[1] = 168;
    peer.ip[2] = 1;
    peer.ip[3] = (uint8_t)(10 + conn);
    peer.port = (uint16_t)(50000 + conn);
    return peer;
}

static void client_send(vc_sessions_t *s, int conn, vc_proto_msg_t *msg, uint64_t now)
{
    uint8_t buf[VC_PROTO_MAX_FRAME];
    size_t  size = vc_proto_encode(msg, buf, sizeof buf);

    CHECK(size > 0);
    vc_sessions_data(s, conn, buf, size, now);
}

static void connect_conn(vc_sessions_t *s, int conn, uint64_t now)
{
    vc_net_peer_t peer = peer_for(conn);

    vc_proto_reader_init(&g_fake.reader[conn]);
    g_fake.count[conn] = 0;
    g_fake.disconnects[conn] = 0;
    vc_sessions_connected(s, conn, &peer, now);
}

static void send_hello(vc_sessions_t *s, int conn, uint8_t type, uint8_t caps,
                       const char *name, uint64_t now)
{
    vc_proto_msg_t msg;

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_HELLO;
    msg.controller_type = type;
    msg.caps = caps;
    strncpy(msg.name, name, VC_CLIENT_NAME_LEN);
    client_send(s, conn, &msg, now);
}

static void send_type(vc_sessions_t *s, int conn, uint8_t type, uint32_t echo, uint64_t now)
{
    vc_proto_msg_t msg;

    memset(&msg, 0, sizeof msg);
    msg.type = type;
    msg.echo = echo;
    client_send(s, conn, &msg, now);
}

/* Connects and completes a handshake; returns the token from WELCOME. */
static uint32_t join(vc_sessions_t *s, int conn, uint8_t type, uint64_t now)
{
    const vc_proto_msg_t *welcome;

    connect_conn(s, conn, now);
    send_hello(s, conn, type, VC_CAP_MOTION | VC_CAP_TOUCH, "phone", now);
    welcome = last_msg(conn);
    CHECK(welcome != NULL && welcome->type == VC_MSG_WELCOME);
    return welcome != NULL ? welcome->token : 0;
}

/* One UDP input from the address of `from_conn`. */
static void send_input(vc_sessions_t *s, uint32_t token, uint16_t sequence,
                       int from_conn, uint32_t buttons, uint64_t now)
{
    vc_net_input_t input;

    memset(&input, 0, sizeof input);
    input.packet.token = token;
    input.packet.sequence = sequence;
    input.packet.state.buttons = buttons;
    input.peer = peer_for(from_conn);
    input.peer.port = 40000;   /* UDP source port differs from TCP's */
    vc_sessions_input(s, &input, now);
}

/* Answers the host's most recent PING on `conn`, if it is one. */
static void answer_ping(vc_sessions_t *s, int conn, uint64_t now)
{
    const vc_proto_msg_t *m = last_msg(conn);
    if (m != NULL && m->type == VC_MSG_PING) {
        send_type(s, conn, VC_MSG_PONG, m->echo, now);
    }
}

static const uint32_t g_tokens[] = {
    0,             /* never issued */
    0x11111111u,
    0x11111111u,   /* already held by slot 0: skipped */
    0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u,
    0x77777777u, 0x88888888u, 0x99999999u, 0xAAAAAAAAu, 0xBBBBBBBBu,
    0xCCCCCCCCu, 0xDDDDDDDDu, 0xEEEEEEEEu
};

static void setup(vc_sessions_t *s)
{
    vc_session_config_t cfg;

    memset(&g_fake, 0, sizeof g_fake);
    g_fake.tokens = g_tokens;
    memset(&cfg, 0, sizeof cfg);
    cfg.send = fake_send;
    cfg.disconnect = fake_disconnect;
    cfg.random32 = fake_random;
    cfg.log = fake_log;
    cfg.on_state = fake_state;
    cfg.user = &g_fake;
    CHECK(vc_sessions_init(s, &cfg) == 0);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                    */
/* ------------------------------------------------------------------------ */

static void test_init(void)
{
    static vc_sessions_t s;
    vc_session_config_t  cfg;

    memset(&cfg, 0, sizeof cfg);
    CHECK(vc_sessions_init(&s, &cfg) == -1);   /* send and disconnect required */
    CHECK(vc_sessions_init(NULL, &cfg) == -1);
    setup(&s);
    CHECK(vc_sessions_active(&s) == 0);
}

static void test_handshake_and_slots(void)
{
    static vc_sessions_t  s;
    const vc_proto_msg_t *m;
    uint64_t              t = 1000;
    int                   i;

    setup(&s);

    /* First client: PS5 offering everything; rumble is not granted (no
     * feedback path yet). The name is sanitised for the logs. */
    connect_conn(&s, 0, t);
    CHECK(g_fake.count[0] == 0);   /* nothing until HELLO */
    send_hello(&s, 0, VC_CONTROLLER_PS5,
               VC_CAP_MOTION | VC_CAP_TOUCH | VC_CAP_RUMBLE, "ph\"one\x01", t);
    m = last_msg(0);
    CHECK(m != NULL && m->type == VC_MSG_WELCOME);
    if (m != NULL) {
        CHECK(m->token == 0x11111111u);   /* 0 is never issued */
        CHECK(m->slot == 0);
        CHECK(m->caps == (VC_CAP_MOTION | VC_CAP_TOUCH));
        CHECK(m->timeout_ms == VC_SESSION_INPUT_TIMEOUT_MS);
    }
    CHECK(strstr(g_fake.last_log, "opened for \"ph?one?\"") != NULL);
    CHECK(vc_sessions_active(&s) == 1);

    /* Second: Switch has motion but no touchpad; a duplicate token is skipped. */
    connect_conn(&s, 1, t);
    send_hello(&s, 1, VC_CONTROLLER_SWITCH, VC_CAP_MOTION | VC_CAP_TOUCH, "two", t);
    m = last_msg(1);
    CHECK(m != NULL && m->type == VC_MSG_WELCOME);
    if (m != NULL) {
        CHECK(m->token == 0x22222222u);
        CHECK(m->slot == 1);
        CHECK(m->caps == VC_CAP_MOTION);
    }

    /* Slots 2 and 3, then a fifth client is refused. */
    connect_conn(&s, 2, t);
    send_hello(&s, 2, VC_CONTROLLER_XBOX, VC_CAP_MOTION, "three", t);
    CHECK(last_type(2) == VC_MSG_WELCOME && last_msg(2)->caps == 0);   /* Xbox: no motion */
    connect_conn(&s, 3, t);
    send_hello(&s, 3, VC_CONTROLLER_PS4, 0, "", t);
    CHECK(last_type(3) == VC_MSG_WELCOME);
    CHECK(strstr(g_fake.last_log, "\"unnamed\"") != NULL);
    CHECK(vc_sessions_active(&s) == 4);

    connect_conn(&s, 4, t);
    send_hello(&s, 4, VC_CONTROLLER_XBOX, 0, "five", t);
    m = last_msg(4);
    CHECK(m != NULL && m->type == VC_MSG_ERROR && m->error == VC_ERROR_NO_SLOT);
    CHECK(g_fake.disconnects[4] == 1);
    CHECK(strstr(g_fake.last_log, "no free slot") != NULL);
    CHECK(vc_sessions_active(&s) == 4);

    /* An unknown controller type is refused even with a free slot. */
    send_type(&s, 0, VC_MSG_BYE, 0, t);   /* frees slot 0 */
    CHECK(vc_sessions_active(&s) == 3);
    connect_conn(&s, 5, t);
    send_hello(&s, 5, 9, 0, "odd", t);
    m = last_msg(5);
    CHECK(m != NULL && m->type == VC_MSG_ERROR && m->error == VC_ERROR_UNSUPPORTED);
    CHECK(g_fake.disconnects[5] == 1);
    send_hello(&s, 5, VC_CONTROLLER_XBOX, 0, "late", t);   /* closed: ignored */
    CHECK(g_fake.count[5] == 1);

    /* The freed slot is reused with a fresh token. */
    connect_conn(&s, 6, t);
    send_hello(&s, 6, VC_CONTROLLER_XBOX, 0, "again", t);
    m = last_msg(6);
    CHECK(m != NULL && m->type == VC_MSG_WELCOME && m->slot == 0);
    CHECK(m != NULL && m->token != 0x11111111u);

    vc_sessions_shutdown(&s);
    CHECK(vc_sessions_active(&s) == 0);
    for (i = 1; i <= 3; i++) {
        CHECK(last_type(i) == VC_MSG_BYE && g_fake.disconnects[i] == 1);
    }
    CHECK(last_type(6) == VC_MSG_BYE && g_fake.disconnects[6] == 1);
}

static void test_input_routing(void)
{
    static vc_sessions_t s;
    vc_hid_device_info_t info;
    uint32_t             a;
    uint32_t             b;
    uint64_t             t = 5000;

    setup(&s);
    a = join(&s, 0, VC_CONTROLLER_PS5, t);
    b = join(&s, 1, VC_CONTROLLER_XBOX, t);

    send_input(&s, a, 1, 0, VC_BTN_SOUTH, t);
    CHECK(g_fake.states == 1 && g_fake.last_state_slot == 0);
    send_input(&s, b, 1, 1, VC_BTN_EAST, t);
    CHECK(g_fake.states == 2 && g_fake.last_state_slot == 1);
    CHECK(strstr(g_fake.last_log, "input from 192.168.1.11:40000") != NULL);

    /* Unknown token: dropped and counted. */
    send_input(&s, 0x0BADBEEFu, 2, 0, 0, t);
    CHECK(g_fake.states == 2 && s.unknown_token == 1);

    /* A's token from B's address: dropped. */
    send_input(&s, a, 2, 1, 0, t);
    CHECK(g_fake.states == 2 && s.slots[0].wrong_address == 1);

    /* Stale: refused by the session's parser. */
    send_input(&s, a, 1, 0, 0, t);
    CHECK(g_fake.states == 2 && s.slots[0].parser.stats.rejected_stale == 1);

    /* Every accepted input reached the right pad. */
    vc_hid_get_info(s.slots[0].device, &info);
    CHECK(info.type == VC_CONTROLLER_PS5 && info.submits == 1);
    vc_hid_get_info(s.slots[1].device, &info);
    CHECK(info.type == VC_CONTROLLER_XBOX && info.submits == 1);

    vc_sessions_shutdown(&s);
}

static void test_input_timeouts(void)
{
    static vc_sessions_t s;
    uint32_t             token;
    uint64_t             t0 = 10000;
    uint64_t             t;
    vc_hid_device_info_t info;

    setup(&s);
    token = join(&s, 0, VC_CONTROLLER_XBOX, t0);

    /* No input yet: the new pad is already neutral, nothing to release. */
    vc_sessions_tick(&s, t0 + 500);
    CHECK(s.slots[0].neutral == 1);

    send_input(&s, token, 1, 0, VC_BTN_SOUTH, t0 + 1000);
    CHECK(s.slots[0].neutral == 0);
    vc_sessions_tick(&s, t0 + 1000 + 249);
    CHECK(s.slots[0].neutral == 0);
    vc_sessions_tick(&s, t0 + 1000 + 250);
    CHECK(s.slots[0].neutral == 1);
    CHECK(strstr(g_fake.last_log, "releasing all controls") != NULL);
    vc_hid_get_info(s.slots[0].device, &info);
    CHECK(info.submits == 2);   /* the input, then the release */

    /* Input resumes, even with a restarted sequence number. */
    send_input(&s, token, 1, 0, VC_BTN_SOUTH, t0 + 2000);
    CHECK(s.slots[0].neutral == 0 && g_fake.states == 2);

    /* Silence with the connection alive (PINGs answered): the session ends
     * after the idle timeout, with ERROR TIMEOUT. */
    for (t = t0 + 2000; t < t0 + 2000 + VC_SESSION_IDLE_TIMEOUT_MS; t += 50) {
        vc_sessions_tick(&s, t);
        answer_ping(&s, 0, t);
    }
    CHECK(vc_sessions_active(&s) == 1);
    vc_sessions_tick(&s, t0 + 2000 + VC_SESSION_IDLE_TIMEOUT_MS);
    CHECK(vc_sessions_active(&s) == 0);
    CHECK(last_type(0) == VC_MSG_ERROR && last_msg(0)->error == VC_ERROR_TIMEOUT);
    CHECK(g_fake.disconnects[0] == 1);
    CHECK(strstr(g_fake.last_log, "no input for 10000 ms") != NULL);

    /* Its token is dead now. */
    send_input(&s, token, 2, 0, 0, t + 100);
    CHECK(s.unknown_token == 1);
}

static void test_keepalive(void)
{
    static vc_sessions_t  s;
    const vc_proto_msg_t *m;
    uint32_t              token;
    uint64_t              t0 = 20000;
    uint64_t              t;
    uint16_t              seq = 1;

    setup(&s);
    token = join(&s, 0, VC_CONTROLLER_XBOX, t0);

    /* First PING one interval after WELCOME; answering it measures the RTT. */
    vc_sessions_tick(&s, t0 + 999);
    CHECK(last_type(0) == VC_MSG_WELCOME);
    vc_sessions_tick(&s, t0 + 1000);
    m = last_msg(0);
    CHECK(m != NULL && m->type == VC_MSG_PING);
    send_type(&s, 0, VC_MSG_PONG, m != NULL ? m->echo : 0, t0 + 1012);
    CHECK(s.slots[0].rtt_ms == 12 && s.slots[0].pings_unanswered == 0);

    /* Stop answering, keep sending input: PINGs at +2 s, +3 s, +4 s, then
     * the session ends at +5 s for 3 unanswered PINGs. */
    for (t = t0 + 1050; t < t0 + 5000; t += 50) {
        send_input(&s, token, seq++, 0, 0, t);
        vc_sessions_tick(&s, t);
    }
    CHECK(vc_sessions_active(&s) == 1);
    CHECK(s.slots[0].pings_unanswered == VC_SESSION_PING_MISSES);
    vc_sessions_tick(&s, t0 + 5000);
    CHECK(vc_sessions_active(&s) == 0);
    CHECK(last_type(0) == VC_MSG_ERROR && last_msg(0)->error == VC_ERROR_TIMEOUT);
    CHECK(strstr(g_fake.last_log, "keepalive timeout") != NULL);
    CHECK(g_fake.disconnects[0] == 1);
}

static void test_endings(void)
{
    static vc_sessions_t s;
    uint64_t             t = 30000;
    int                  sent;

    setup(&s);

    /* BYE: no reply, connection closed, slot freed. */
    join(&s, 0, VC_CONTROLLER_XBOX, t);
    sent = g_fake.count[0];
    send_type(&s, 0, VC_MSG_BYE, 0, t);
    CHECK(vc_sessions_active(&s) == 0);
    CHECK(g_fake.count[0] == sent && g_fake.disconnects[0] == 1);
    CHECK(strstr(g_fake.last_log, "client said bye") != NULL);

    /* TCP lost: nothing sent, the transport already closed it. */
    join(&s, 1, VC_CONTROLLER_XBOX, t);
    sent = g_fake.count[1];
    vc_sessions_disconnected(&s, 1, t);
    CHECK(vc_sessions_active(&s) == 0);
    CHECK(g_fake.count[1] == sent && g_fake.disconnects[1] == 0);
    CHECK(strstr(g_fake.last_log, "connection lost") != NULL);

    /* Lost before HELLO, and a second notice for the same index: harmless. */
    connect_conn(&s, 2, t);
    vc_sessions_disconnected(&s, 2, t);
    vc_sessions_disconnected(&s, 2, t);
    CHECK(g_fake.disconnects[2] == 0);

    /* No HELLO in time. */
    connect_conn(&s, 3, t);
    vc_sessions_tick(&s, t + VC_SESSION_HELLO_TIMEOUT_MS - 1);
    CHECK(g_fake.disconnects[3] == 0);
    vc_sessions_tick(&s, t + VC_SESSION_HELLO_TIMEOUT_MS);
    CHECK(last_type(3) == VC_MSG_ERROR && last_msg(3)->error == VC_ERROR_TIMEOUT);
    CHECK(g_fake.disconnects[3] == 1);

    /* A send failure ends the session quietly. */
    join(&s, 4, VC_CONTROLLER_XBOX, t);
    g_fake.fail_send = 1;
    vc_sessions_tick(&s, t + 1000);   /* PING fails */
    g_fake.fail_send = 0;
    CHECK(vc_sessions_active(&s) == 0 && g_fake.disconnects[4] == 1);
}

static void test_control_messages(void)
{
    static vc_sessions_t s;
    vc_proto_msg_t       msg;
    uint8_t              buf[VC_PROTO_MAX_FRAME];
    size_t               size;
    size_t               i;
    uint64_t             t = 40000;

    setup(&s);

    /* PING from the client is answered, before and after HELLO. */
    connect_conn(&s, 0, t);
    send_type(&s, 0, VC_MSG_PING, 77, t);
    CHECK(last_type(0) == VC_MSG_PONG && last_msg(0)->echo == 77);

    /* HELLO split into single bytes still works. */
    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_HELLO;
    msg.controller_type = VC_CONTROLLER_XBOX;
    strcpy(msg.name, "split");
    size = vc_proto_encode(&msg, buf, sizeof buf);
    for (i = 0; i < size; i++) {
        vc_sessions_data(&s, 0, buf + i, 1, t);
    }
    CHECK(last_type(0) == VC_MSG_WELCOME);
    send_type(&s, 0, VC_MSG_PING, 78, t);
    CHECK(last_type(0) == VC_MSG_PONG && last_msg(0)->echo == 78);

    /* Messages are numbered: WELCOME was 1 after the first PONG's 0. */
    CHECK(g_fake.seen[0][0].sequence == 0 && g_fake.seen[0][1].sequence == 1);

    /* A second HELLO is a protocol error and ends the session. */
    send_hello(&s, 0, VC_CONTROLLER_XBOX, 0, "again", t);
    CHECK(last_type(0) == VC_MSG_ERROR && last_msg(0)->error == VC_ERROR_PROTOCOL);
    CHECK(vc_sessions_active(&s) == 0 && g_fake.disconnects[0] == 1);

    /* Host-only messages from a client are protocol errors. */
    connect_conn(&s, 1, t);
    send_type(&s, 1, VC_MSG_WELCOME, 0, t);
    CHECK(last_type(1) == VC_MSG_ERROR && last_msg(1)->error == VC_ERROR_PROTOCOL);
    CHECK(strstr(g_fake.last_log, "unexpected WELCOME") != NULL);

    /* Garbage. */
    connect_conn(&s, 2, t);
    vc_sessions_data(&s, 2, (const uint8_t *)"GET / HTTP/1.1\r\n", 16, t);
    CHECK(last_type(2) == VC_MSG_ERROR && last_msg(2)->error == VC_ERROR_PROTOCOL);
    CHECK(g_fake.disconnects[2] == 1);

    /* A newer protocol version gets the specific error. */
    connect_conn(&s, 3, t);
    size = vc_proto_encode(&msg, buf, sizeof buf);
    buf[2] = VC_PROTOCOL_VERSION + 1;
    vc_sessions_data(&s, 3, buf, size, t);
    CHECK(last_type(3) == VC_MSG_ERROR && last_msg(3)->error == VC_ERROR_BAD_VERSION);

    /* HELLO and BYE in one read: the session opens and closes. */
    connect_conn(&s, 4, t);
    size = vc_proto_encode(&msg, buf, sizeof buf);
    {
        uint8_t        two[2 * VC_PROTO_MAX_FRAME];
        vc_proto_msg_t bye;

        memcpy(two, buf, size);
        memset(&bye, 0, sizeof bye);
        bye.type = VC_MSG_BYE;
        size += vc_proto_encode(&bye, two + size, sizeof two - size);
        vc_sessions_data(&s, 4, two, size, t);
    }
    CHECK(last_type(4) == VC_MSG_WELCOME);
    CHECK(vc_sessions_active(&s) == 0 && g_fake.disconnects[4] == 1);

    vc_sessions_shutdown(&s);
}

int main(void)
{
    CHECK(vc_hid_init() == 0);
    test_init();
    test_handshake_and_slots();
    test_input_routing();
    test_input_timeouts();
    test_keepalive();
    test_endings();
    test_control_messages();
    vc_hid_shutdown();
    return test_summary();
}
