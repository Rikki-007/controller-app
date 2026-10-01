/*
 * test_network.c - the UDP listener over a real socket on localhost.
 * Packet decoding itself is covered by test_parser.c.
 *
 * Build (Windows):  gcc -std=c99 -Iinclude src/network.c src/parser.c tests/test_network.c -lws2_32
 * Build (POSIX):    cc  -std=c99 -Iinclude src/network.c src/parser.c tests/test_network.c
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

#include "network.h"
#include "test_util.h"

/* ------------------------------------------------------------------------ */
/* Listener over a real socket                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    int            calls;
    vc_net_input_t last;
    vc_net_t      *stop_after_first;
} capture_t;

static void on_input(const vc_net_input_t *input, void *user)
{
    capture_t *cap = (capture_t *)user;
    cap->calls++;
    cap->last = *input;
    if (cap->stop_after_first != NULL) {
        vc_net_stop(cap->stop_after_first);
    }
}

typedef struct {
    int info;
    int warn;
    int error;
} log_counts_t;

static void on_log(vc_net_log_level_t level, const char *message, void *user)
{
    log_counts_t *counts = (log_counts_t *)user;
    if (level == VC_NET_LOG_INFO) {
        counts->info++;
    } else if (level == VC_NET_LOG_WARN) {
        counts->warn++;
    } else {
        counts->error++;
    }
    printf("  log[%d] %s\n", (int)level, message);
}

static void send_to(test_socket_t sock, uint16_t port, const uint8_t *data, size_t size)
{
    struct sockaddr_in to;
    long               sent;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sent = (long)sendto(sock, (const char *)data, (int)size, 0,
                        (struct sockaddr *)&to, sizeof to);
    CHECK(sent == (long)size);
}

/* Polls until `want` inputs have arrived or about two seconds pass. */
static void pump(vc_net_t *net, const capture_t *cap, int want)
{
    int tries;
    for (tries = 0; tries < 40 && cap->calls < want; tries++) {
        CHECK(vc_net_poll(net, 50) >= 0);
    }
}

static void test_listener(void)
{
    vc_net_config_t cfg;
    vc_net_stats_t  stats;
    vc_net_t       *net = NULL;
    vc_net_t       *second = NULL;
    capture_t       cap;
    log_counts_t    logs;
    test_socket_t   client;
    uint8_t         buf[400];
    size_t          size;
    uint16_t        port;
    int             i;

    memset(&cap, 0, sizeof cap);
    memset(&logs, 0, sizeof logs);
    vc_net_set_log(on_log, &logs);

    /* Bad arguments are reported, not crashed on. */
    memset(&cfg, 0, sizeof cfg);
    CHECK(vc_net_open(&cfg, &net) == -1 && net == NULL);
    cfg.on_input = on_input;
    cfg.bind_address = "not-an-address";
    CHECK(vc_net_open(&cfg, &net) == -1 && net == NULL);
    CHECK(logs.error == 2);

    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.user = &cap;
    CHECK(vc_net_open(&cfg, &net) == 0);
    if (net == NULL) {
        return;
    }
    port = vc_net_port(net);
    CHECK(port != 0);
    CHECK(logs.info == 1);

    /* The port is taken now, so a second listener on it must fail cleanly. */
    cfg.port = port;
    CHECK(vc_net_open(&cfg, &second) == -1 && second == NULL);
    CHECK(logs.error == 3);

    /* Nothing sent yet: a zero-timeout poll returns immediately with nothing. */
    CHECK(vc_net_poll(net, 0) == 0);

    /* The listener's WSAStartup is process-wide, so this socket works on Windows too. */
    client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(client != TEST_INVALID_SOCKET);

    /* One good packet. */
    size = build_input(buf, VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH, 42);
    send_to(client, port, buf, size);
    pump(net, &cap, 1);
    CHECK(cap.calls == 1);
    CHECK(cap.last.packet.sequence == 42);
    CHECK(cap.last.packet.token == 0xDEADBEEFu);
    CHECK(cap.last.packet.state.left.x == -32768);
    CHECK(cap.last.packet.state.motion.accel[2] == -6);
    CHECK(cap.last.packet.state.touch[0].id == 5);
    CHECK(cap.last.peer.ip[0] == 127 && cap.last.peer.ip[3] == 1);
    CHECK(cap.last.peer.port != 0);

    /* Garbage, a truncated packet and an oversized one, then a good packet:
     * the bad ones are dropped and the listener keeps working. */
    memset(buf, 0xAB, sizeof buf);
    send_to(client, port, buf, 20);
    send_to(client, port, buf, 3);
    send_to(client, port, buf, 300);
    send_to(client, port, buf, 100);
    size = build_input(buf, 0, 43);
    send_to(client, port, buf, size - 4);
    send_to(client, port, buf, size);
    pump(net, &cap, 2);
    CHECK(cap.calls == 2);
    CHECK(cap.last.packet.sequence == 43);

    vc_net_get_stats(net, &stats);
    CHECK(stats.datagrams_received == 7);
    CHECK(stats.inputs_delivered == 2);
    CHECK(stats.dropped_malformed == 3);
    CHECK(stats.dropped_oversized == 2);
    CHECK(stats.receive_errors == 0);
    CHECK(logs.warn >= 2);

    /* A burst larger than one batch is still fully delivered across polls. */
    for (i = 0; i < 100; i++) {
        size = build_input(buf, 0, (unsigned)(1000 + i));
        send_to(client, port, buf, size);
    }
    pump(net, &cap, 102);
    CHECK(cap.calls == 102);
    CHECK(cap.last.packet.sequence == 1099);

    /* vc_net_run() returns once the callback asks it to stop. */
    cap.stop_after_first = net;
    size = build_input(buf, 0, 2000);
    send_to(client, port, buf, size);
    CHECK(vc_net_run(net) == 0);
    CHECK(cap.calls == 103);
    CHECK(vc_net_run(net) == 0);   /* still stopped: returns at once */

    CHECK(logs.error == 3);
    test_close_socket(client);
    vc_net_close(net);
    vc_net_close(NULL);
    vc_net_set_log(NULL, NULL);
}

int main(void)
{
    test_listener();
    return test_summary();
}
