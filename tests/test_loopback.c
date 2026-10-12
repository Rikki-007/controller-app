/*
 * test_loopback.c - the whole input path in one process, over real sockets:
 *
 *   encoder -> UDP on localhost -> network.c -> parser.c (per session) -> hid_output.c (stub)
 *
 * Two clients with their own tokens and controller types stand in for two
 * phones. The standalone daemon and its slot handling are covered by
 * tests/e2e.sh, which drives build/<backend>/vcd with tools/vc_send.c.
 *
 * Build (Windows):  gcc -std=c99 -Iinclude -Itests src/encoder.c src/network.c src/parser.c src/hid_output.c tests/test_loopback.c -lws2_32
 * Build (POSIX):    cc  -std=c99 -Iinclude -Itests src/encoder.c src/network.c src/parser.c src/hid_output.c tests/test_loopback.c
 */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET test_socket_t;
#  define TEST_INVALID_SOCKET INVALID_SOCKET
#  define test_close_socket   closesocket
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200112L
#  endif
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int test_socket_t;
#  define TEST_INVALID_SOCKET (-1)
#  define test_close_socket   close
#endif

#include "encoder.h"
#include "hid_output.h"
#include "network.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

#define SESSIONS         2
#define PACKETS_EACH     50

typedef struct {
    uint32_t             token;
    vc_controller_type_t type;
    test_socket_t        sock;
    vc_parser_t          parser;
    vc_hid_device_t     *device;
    vc_input_state_t     last;        /* last state accepted, after the profile */
    int                  delivered;
} session_t;

typedef struct {
    session_t sessions[SESSIONS];
    int       unknown_token;
} loop_t;

/* What the daemon's main loop does, minus slot allocation. */
static void on_input(const vc_net_input_t *input, void *user)
{
    loop_t *loop = (loop_t *)user;
    int     i;

    for (i = 0; i < SESSIONS; i++) {
        session_t *s = &loop->sessions[i];
        if (s->token == input->packet.token) {
            s->delivered++;
            if (vc_parser_accept(&s->parser, &input->packet, &s->last) == VC_PARSE_OK) {
                CHECK(vc_hid_submit(s->device, &s->last) == 0);
            }
            return;
        }
    }
    loop->unknown_token++;
}

static void send_state(session_t *s, uint16_t port, const vc_input_state_t *state,
                       uint16_t sequence)
{
    struct sockaddr_in to;
    uint8_t            buf[VC_INPUT_DATAGRAM_MAX];
    size_t             size = vc_encode_input(state, s->token, sequence, buf, sizeof buf);
    long               sent;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sent = (long)sendto(s->sock, (const char *)buf, (int)size, 0,
                        (struct sockaddr *)&to, sizeof to);
    CHECK(size > 0 && sent == (long)size);
}

/* Polls until `want` datagrams have been delivered or about two seconds pass. */
static void pump(vc_net_t *net, const loop_t *loop, int want)
{
    int tries;
    for (tries = 0; tries < 40; tries++) {
        int total = loop->unknown_token + loop->sessions[0].delivered +
                    loop->sessions[1].delivered;
        if (total >= want) {
            break;
        }
        CHECK(vc_net_poll(net, 50) >= 0);
    }
}

static void test_pipeline(void)
{
    static loop_t        loop;
    vc_net_config_t      cfg;
    vc_net_t            *net = NULL;
    vc_hid_device_info_t info;
    vc_input_state_t     state;
    uint16_t             port;
    int                  i;
    int                  n;

    memset(&loop, 0, sizeof loop);
    loop.sessions[0].token = 0xA0000001u;
    loop.sessions[0].type = VC_CONTROLLER_PS5;
    loop.sessions[1].token = 0xB0000002u;
    loop.sessions[1].type = VC_CONTROLLER_SWITCH;

    CHECK(vc_hid_init() == 0);
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_address = "127.0.0.1";
    cfg.on_input = on_input;
    cfg.user = &loop;
    CHECK(vc_net_open(&cfg, &net) == 0);
    if (net == NULL) {
        return;
    }
    port = vc_net_port(net);

    for (i = 0; i < SESSIONS; i++) {
        session_t *s = &loop.sessions[i];
        CHECK(vc_parser_init(&s->parser, s->type, s->token) == 0);
        CHECK(vc_hid_create(s->type, &s->device) == 0);
        s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        CHECK(s->sock != TEST_INVALID_SOCKET);
    }

    /* Interleaved streams, one button per packet, everything optional set. */
    memset(&state, 0, sizeof state);
    for (n = 0; n < PACKETS_EACH; n++) {
        for (i = 0; i < SESSIONS; i++) {
            memset(&state, 0, sizeof state);
            state.buttons = UINT32_C(1) << (n % 17);
            state.left.x = (int16_t)(n * 600);
            state.trigger_left = (uint8_t)(n * 5);
            state.timestamp_us = (uint32_t)n * 8333u;
            state.flags = VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH;
            state.motion.accel[2] = 4096;
            state.touch[0].active = 1;
            state.touch[0].x = 30000;
            send_state(&loop.sessions[i], port, &state, (uint16_t)(65500 + n));   /* wraps */
        }
        /* Keep well inside the OS receive buffer. */
        if (n % 10 == 9) {
            pump(net, &loop, (n + 1) * SESSIONS);
        }
    }
    pump(net, &loop, PACKETS_EACH * SESSIONS);

    for (i = 0; i < SESSIONS; i++) {
        session_t *s = &loop.sessions[i];
        CHECK(s->delivered == PACKETS_EACH);
        CHECK(s->parser.stats.accepted == PACKETS_EACH);
        CHECK(s->parser.stats.rejected_stale == 0);
        vc_hid_get_info(s->device, &info);
        CHECK(info.submits == PACKETS_EACH);
        CHECK(info.type == s->type);
        /* The last packet (n = 49) arrived intact, sequence wrap included. */
        CHECK(s->last.buttons == (UINT32_C(1) << (49 % 17)));
        CHECK(s->last.left.x == 49 * 600);
        CHECK(s->last.timestamp_us == 49u * 8333u);
    }

    /* Profiles differ per session: PS5 keeps touch and analog triggers,
     * Switch drops touch and snaps triggers to 0 or 255. */
    CHECK(loop.sessions[0].last.flags == (VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH));
    CHECK(loop.sessions[0].last.trigger_left == 245);
    CHECK(loop.sessions[1].last.flags == VC_INPUT_HAS_MOTION);
    CHECK(loop.sessions[1].last.trigger_left == 0 || loop.sessions[1].last.trigger_left == 255);

    /* A replayed packet is delivered by the network but refused as stale,
     * and a stranger's token never reaches a session. */
    send_state(&loop.sessions[0], port, &state, (uint16_t)(65500 + PACKETS_EACH - 1));
    pump(net, &loop, PACKETS_EACH * SESSIONS + 1);
    CHECK(loop.sessions[0].parser.stats.rejected_stale == 1);
    vc_hid_get_info(loop.sessions[0].device, &info);
    CHECK(info.submits == PACKETS_EACH);

    loop.sessions[1].token = 0xC0000003u;   /* sends under a token nobody owns */
    send_state(&loop.sessions[1], port, &state, 1);
    loop.sessions[1].token = 0xB0000002u;
    pump(net, &loop, PACKETS_EACH * SESSIONS + 2);
    CHECK(loop.unknown_token == 1);

    for (i = 0; i < SESSIONS; i++) {
        test_close_socket(loop.sessions[i].sock);
        vc_hid_destroy(loop.sessions[i].device);
    }
    vc_net_close(net);
    vc_hid_shutdown();
}

int main(void)
{
    test_pipeline();
    return test_summary();
}
