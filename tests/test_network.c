/*
 * test_network.c - the listener over real sockets on localhost: UDP input,
 * TCP control connections, and UDP sending with ICMP errors.
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
#  include <sys/select.h>
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

/* ------------------------------------------------------------------------ */
/* TCP control connections                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    vc_net_t     *net;
    int           inputs;
    int           connects;
    int           disconnects;
    int           last_conn;
    int           last_disconnected;
    vc_net_peer_t peer;
    uint8_t       data[256];
    size_t        data_len;
} tcp_capture_t;

static void tcp_on_input(const vc_net_input_t *input, void *user)
{
    (void)input;
    ((tcp_capture_t *)user)->inputs++;
}

static void tcp_on_connect(int conn, const vc_net_peer_t *peer, void *user)
{
    tcp_capture_t *cap = (tcp_capture_t *)user;
    cap->connects++;
    cap->last_conn = conn;
    cap->peer = *peer;
}

static void tcp_on_data(int conn, const uint8_t *data, size_t size, void *user)
{
    tcp_capture_t *cap = (tcp_capture_t *)user;
    (void)conn;
    if (cap->data_len + size <= sizeof cap->data) {
        memcpy(cap->data + cap->data_len, data, size);
        cap->data_len += size;
    }
}

static void tcp_on_disconnect(int conn, void *user)
{
    tcp_capture_t *cap = (tcp_capture_t *)user;
    cap->disconnects++;
    cap->last_disconnected = conn;
}

/* Polls until *counter reaches want or about two seconds pass. */
static void pump_until(vc_net_t *net, const int *counter, int want)
{
    int tries;
    for (tries = 0; tries < 40 && *counter < want; tries++) {
        CHECK(vc_net_poll(net, 50) >= 0);
    }
}

static test_socket_t tcp_connect(uint16_t port)
{
    struct sockaddr_in to;
    test_socket_t      sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    CHECK(sock != TEST_INVALID_SOCKET);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(sock, (struct sockaddr *)&to, sizeof to) == 0);
    return sock;
}

/* Waits up to two seconds, polling the listener meanwhile, for the client
 * socket to become readable, then reads. Returns what recv() returned, or
 * -2 on timeout. */
static long client_recv(vc_net_t *net, test_socket_t sock, uint8_t *buf, size_t size)
{
    int tries;

    for (tries = 0; tries < 40; tries++) {
        fd_set         readable;
        struct timeval tv;

        FD_ZERO(&readable);
        FD_SET(sock, &readable);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select((int)sock + 1, &readable, NULL, NULL, &tv) > 0) {
            return (long)recv(sock, (char *)buf, (int)size, 0);
        }
        vc_net_poll(net, 50);
    }
    return -2;
}

static void test_tcp(void)
{
    static tcp_capture_t cap;
    vc_net_config_t      cfg;
    vc_net_stats_t       stats;
    vc_net_t            *net = NULL;
    test_socket_t        client;
    test_socket_t        many[VC_NET_MAX_CONNS + 1];
    uint8_t              buf[64];
    uint16_t             port;
    int                  conn;
    int                  i;

    memset(&cap, 0, sizeof cap);
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_address = "127.0.0.1";
    cfg.on_input = tcp_on_input;
    cfg.tcp = 1;
    cfg.user = &cap;

    /* tcp needs all three connection callbacks. */
    CHECK(vc_net_open(&cfg, &net) == -1 && net == NULL);
    cfg.on_connect = tcp_on_connect;
    cfg.on_data = tcp_on_data;
    cfg.on_disconnect = tcp_on_disconnect;
    CHECK(vc_net_open(&cfg, &net) == 0);
    if (net == NULL) {
        return;
    }
    cap.net = net;
    port = vc_net_port(net);

    /* Connect: one callback with the peer's address. */
    client = tcp_connect(port);
    pump_until(net, &cap.connects, 1);
    CHECK(cap.connects == 1);
    conn = cap.last_conn;
    CHECK(conn >= 0 && conn < VC_NET_MAX_CONNS);
    CHECK(cap.peer.ip[0] == 127 && cap.peer.ip[3] == 1 && cap.peer.port != 0);

    /* Bytes both ways. */
    CHECK(send(client, "hello", 5, 0) == 5);
    for (i = 0; i < 40 && cap.data_len < 5; i++) {
        CHECK(vc_net_poll(net, 50) >= 0);
    }
    CHECK(cap.data_len == 5 && memcmp(cap.data, "hello", 5) == 0);
    CHECK(vc_net_send(net, conn, (const uint8_t *)"world", 5) == 0);
    CHECK(client_recv(net, client, buf, sizeof buf) == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(vc_net_send(net, -1, buf, 1) == -1);
    CHECK(vc_net_send(net, VC_NET_MAX_CONNS, buf, 1) == -1);
    CHECK(vc_net_send(net, (conn + 1) % VC_NET_MAX_CONNS, buf, 1) == -1);   /* not open */

    /* The peer closes: on_disconnect for that connection, and it is gone. */
    test_close_socket(client);
    pump_until(net, &cap.disconnects, 1);
    CHECK(cap.disconnects == 1 && cap.last_disconnected == conn);
    CHECK(vc_net_send(net, conn, buf, 1) == -1);

    /* We close: what was queued is delivered, then EOF; no callback. */
    client = tcp_connect(port);
    pump_until(net, &cap.connects, 2);
    CHECK(cap.connects == 2);
    conn = cap.last_conn;
    CHECK(vc_net_send(net, conn, (const uint8_t *)"bye", 3) == 0);
    vc_net_disconnect(net, conn);
    vc_net_disconnect(net, conn);   /* already closed: ignored */
    CHECK(client_recv(net, client, buf, sizeof buf) == 3 && memcmp(buf, "bye", 3) == 0);
    CHECK(client_recv(net, client, buf, sizeof buf) == 0);
    CHECK(cap.disconnects == 1);
    test_close_socket(client);

    /* One more than the limit: the surplus connection is refused. */
    for (i = 0; i <= VC_NET_MAX_CONNS; i++) {
        many[i] = tcp_connect(port);
        vc_net_poll(net, 10);   /* accept as we go; the backlog is small */
    }
    pump_until(net, &cap.connects, 2 + VC_NET_MAX_CONNS);
    for (i = 0; i < 10; i++) {
        vc_net_poll(net, 20);
    }
    vc_net_get_stats(net, &stats);
    CHECK(cap.connects == 2 + VC_NET_MAX_CONNS);
    CHECK(stats.connections_accepted == 2 + VC_NET_MAX_CONNS);
    CHECK(stats.connections_refused == 1);
    for (i = 0; i <= VC_NET_MAX_CONNS; i++) {
        test_close_socket(many[i]);
    }

    /* UDP input still flows alongside TCP. */
    {
        test_socket_t udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        size_t        size = build_input(buf, 0, 1);

        send_to(udp, port, buf, size);
        pump_until(net, &cap.inputs, 1);
        CHECK(cap.inputs == 1);
        test_close_socket(udp);
    }

    /* vc_net_close() closes what is still open, without callbacks. */
    i = cap.disconnects;
    vc_net_close(net);
    CHECK(cap.disconnects == i);
}

/* ------------------------------------------------------------------------ */
/* UDP sending and ICMP errors                                              */
/* ------------------------------------------------------------------------ */

static void test_send_to(void)
{
    static tcp_capture_t cap;
    vc_net_config_t      cfg;
    vc_net_stats_t       stats;
    vc_net_t            *net = NULL;
    vc_net_peer_t        peer;
    test_socket_t        client;
    test_socket_t        gone;
    struct sockaddr_in   addr;
    socklen_t            addr_len = sizeof addr;
    uint8_t              buf[64];
    size_t               size;
    int                  i;

    memset(&cap, 0, sizeof cap);
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_address = "127.0.0.1";
    cfg.on_input = tcp_on_input;
    cfg.user = &cap;
    CHECK(vc_net_open(&cfg, &net) == 0);
    if (net == NULL) {
        return;
    }

    /* A port nobody listens on: bind a socket, note its port, close it. */
    gone = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(gone, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(getsockname(gone, (struct sockaddr *)&addr, &addr_len) == 0);
    test_close_socket(gone);

    /* Sending there makes the OS answer with ICMP "port unreachable". On
     * Windows that used to surface as WSAECONNRESET on the next recvfrom()
     * and count as a receive error; with SIO_UDP_CONNRESET off it must not. */
    peer.ip[0] = 127;
    peer.ip[1] = 0;
    peer.ip[2] = 0;
    peer.ip[3] = 1;
    peer.port = ntohs(addr.sin_port);
    for (i = 0; i < 3; i++) {
        CHECK(vc_net_send_to(net, &peer, (const uint8_t *)"feedback", 8) == 0);
    }

    client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    size = build_input(buf, 0, 1);
    send_to(client, vc_net_port(net), buf, size);
    pump_until(net, &cap.inputs, 1);
    CHECK(cap.inputs == 1);
    for (i = 0; i < 5; i++) {
        CHECK(vc_net_poll(net, 20) >= 0);
    }
    vc_net_get_stats(net, &stats);
    CHECK(stats.receive_errors == 0);

    /* And to a live socket the datagram arrives. */
    memset(&addr, 0, sizeof addr);
    addr_len = sizeof addr;
    CHECK(getsockname(client, (struct sockaddr *)&addr, &addr_len) == 0);
    peer.port = ntohs(addr.sin_port);
    CHECK(vc_net_send_to(net, &peer, (const uint8_t *)"rumble", 6) == 0);
    CHECK(client_recv(net, client, buf, sizeof buf) == 6 && memcmp(buf, "rumble", 6) == 0);

    CHECK(vc_net_send_to(net, NULL, buf, 1) == -1);
    test_close_socket(client);
    vc_net_close(net);
}

int main(void)
{
    test_listener();
    test_tcp();
    test_send_to();
    return test_summary();
}
