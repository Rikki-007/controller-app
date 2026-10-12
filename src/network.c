/*
 * network.c - sockets for the mobile clients: UDP input and TCP control.
 * See network.h for the contract.
 */

/* Platform headers. The feature macros must come before any system header. */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll, inet_pton */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200112L
#  endif
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network.h"

#ifdef _WIN32
typedef SOCKET vc_socket_t;
#  define VC_INVALID_SOCKET  INVALID_SOCKET
#  define VC_POLL_READABLE   POLLRDNORM
#  define VC_POLL_WRITABLE   POLLWRNORM
#  define VC_SHUT_SEND       SD_SEND
/* Windows reports an ICMP "port unreachable" (or "TTL expired") answering an
 * earlier sendto() as WSAECONNRESET (WSAENETRESET) on the next recvfrom()
 * of the same UDP socket. These ioctls turn that off. Values from mstcpip.h,
 * which not every MinGW toolchain ships. */
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#  endif
#  ifndef SIO_UDP_NETRESET
#    define SIO_UDP_NETRESET  _WSAIOW(IOC_VENDOR, 15)
#  endif
#else
typedef int vc_socket_t;
#  define VC_INVALID_SOCKET  (-1)
#  define VC_POLL_READABLE   POLLIN
#  define VC_POLL_WRITABLE   POLLOUT
#  define VC_SHUT_SEND       SHUT_WR
#endif

/* A peer that has gone away must not kill the daemon with SIGPIPE. Linux
 * takes a per-call flag; macOS and the BSDs a socket option (see accept). */
#ifdef MSG_NOSIGNAL
#  define VC_SEND_FLAGS MSG_NOSIGNAL
#else
#  define VC_SEND_FLAGS 0
#endif

/* Larger than any valid datagram so an oversized one is seen as oversized
 * rather than silently truncated to a valid-looking size. */
#define VC_NET_RECV_BUFFER       256
/* Datagrams handled per vc_net_poll() call, so a flood cannot starve the caller. */
#define VC_NET_MAX_BATCH         64
/* Back-to-back receive failures after which the socket is declared dead. */
#define VC_NET_MAX_RECV_FAILURES 16
/* How often vc_net_run() checks the stop flag. */
#define VC_NET_RUN_TIMEOUT_MS    100
/* Pending output per control connection. Control messages are tiny and rare,
 * so filling this means the peer has stopped reading. */
#define VC_NET_OUT_BUFFER        1024
/* Bytes read per recv() on a control connection, and recv() calls per poll. */
#define VC_NET_TCP_CHUNK         512
#define VC_NET_TCP_MAX_READS     8
#define VC_NET_LISTEN_BACKLOG    8

typedef struct {
    int         open;
    int         failed;     /* a send failed; waiting for vc_net_disconnect() */
    vc_socket_t sock;
    size_t      out_len;
    uint8_t     out[VC_NET_OUT_BUFFER];
} conn_t;

struct vc_net {
    vc_socket_t          sock;          /* UDP */
    vc_socket_t          listen_sock;   /* TCP, or VC_INVALID_SOCKET */
    uint16_t             port;
    vc_net_input_cb      on_input;
    vc_net_connect_cb    on_connect;
    vc_net_data_cb       on_data;
    vc_net_disconnect_cb on_disconnect;
    void                *user;
    volatile int         stop_requested;
    unsigned             recv_failures;   /* consecutive */
    uint64_t             send_to_failures;
    vc_net_stats_t       stats;
    conn_t               conns[VC_NET_MAX_CONNS];
};

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

static vc_net_log_cb g_log_cb;
static void         *g_log_user;

void vc_net_set_log(vc_net_log_cb callback, void *user)
{
    g_log_cb = callback;
    g_log_user = user;
}

#ifdef __GNUC__
static void log_msg(vc_net_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#endif

static void log_msg(vc_net_log_level_t level, const char *fmt, ...)
{
    static const char *const names[] = { "info", "warn", "error" };
    char    message[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    if (g_log_cb != NULL) {
        g_log_cb(level, message, g_log_user);
    } else {
        fprintf(stderr, "[net] %s: %s\n", names[level], message);
    }
}

/* True for the 1st, 2nd, 4th, 8th... occurrence. Keeps a peer that sends
 * garbage at 120 Hz from flooding the log while still showing it is ongoing. */
static int should_log(uint64_t occurrence)
{
    return (occurrence & (occurrence - 1)) == 0;
}

/* ------------------------------------------------------------------------ */
/* Socket portability                                                       */
/* ------------------------------------------------------------------------ */

static int sock_errno(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int sock_would_block(int err)
{
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

static int sock_interrupted(int err)
{
#ifdef _WIN32
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

/* UDP: an ICMP error about an earlier datagram we sent, not a fault of the
 * socket. Windows only reports these if SIO_UDP_CONNRESET could not be
 * turned off; POSIX only for connected sockets, which we never use. */
static int sock_icmp_error(int err)
{
#ifdef _WIN32
    return err == WSAECONNRESET || err == WSAENETRESET;
#else
    return err == ECONNREFUSED || err == EHOSTUNREACH || err == ENETUNREACH;
#endif
}

/* TCP accept: the client gave up between connecting and our accept(). */
static int sock_accept_aborted(int err)
{
#ifdef _WIN32
    return err == WSAECONNRESET;
#else
    return err == ECONNABORTED || err == EPROTO;
#endif
}

/* Describes a socket error code as "text (code)". */
static const char *sock_strerror(int err, char *buf, size_t size)
{
    char text[160];

#ifdef _WIN32
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        (DWORD)err, 0, text, (DWORD)sizeof text, NULL);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n' ||
                       text[len - 1] == '.' || text[len - 1] == ' ')) {
        len--;
    }
    text[len] = '\0';
    if (len == 0) {
        strcpy(text, "unknown error");
    }
#else
    if (strerror_r(err, text, sizeof text) != 0) {
        strcpy(text, "unknown error");
    }
#endif

    snprintf(buf, size, "%s (%d)", text, err);
    return buf;
}

static void sock_close(vc_socket_t sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

static int sock_set_nonblocking(vc_socket_t sock)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(sock, (long)FIONBIO, &enabled) == 0 ? 0 : -1;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* On POSIX, lets the daemon rebind right after a restart. On Windows the same
 * option would let another process steal the port, so claim it exclusively. */
static int sock_set_bind_policy(vc_socket_t sock)
{
#ifdef _WIN32
    BOOL enabled = TRUE;
    return setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                      (const char *)&enabled, sizeof enabled);
#else
    int enabled = 1;
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
#endif
}

/* Windows only: stop ICMP errors from surfacing on the UDP socket. */
static void sock_ignore_icmp_errors(vc_socket_t sock)
{
#ifdef _WIN32
    BOOL  off = FALSE;
    DWORD returned = 0;
    char  err[200];

    if (WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &returned,
                 NULL, NULL) != 0) {
        /* Not fatal: receive_one() also tolerates the error. */
        log_msg(VC_NET_LOG_WARN, "could not disable SIO_UDP_CONNRESET: %s",
                sock_strerror(sock_errno(), err, sizeof err));
    }
    if (WSAIoctl(sock, SIO_UDP_NETRESET, &off, sizeof off, NULL, 0, &returned,
                 NULL, NULL) != 0) {
        log_msg(VC_NET_LOG_WARN, "could not disable SIO_UDP_NETRESET: %s",
                sock_strerror(sock_errno(), err, sizeof err));
    }
#else
    (void)sock;
#endif
}

/* Options for an accepted control connection. Failures only cost latency or
 * robustness, so they are not fatal. */
static void sock_tune_connection(vc_socket_t sock)
{
#ifdef _WIN32
    BOOL enabled = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled, sizeof enabled);
#else
    int enabled = 1;
    /* Control messages are a few bytes each; do not hold them back. */
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
#  ifdef SO_NOSIGPIPE
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#  endif
#endif
}

static int sock_poll(struct pollfd *fds, int count, int timeout_ms)
{
#ifdef _WIN32
    return WSAPoll(fds, (ULONG)count, timeout_ms);
#else
    return poll(fds, (nfds_t)count, timeout_ms);
#endif
}

static int platform_startup(void)
{
#ifdef _WIN32
    WSADATA data;
    int     err = WSAStartup(MAKEWORD(2, 2), &data);
    if (err != 0) {
        char buf[200];
        log_msg(VC_NET_LOG_ERROR, "WSAStartup failed: %s",
                sock_strerror(err, buf, sizeof buf));
        return -1;
    }
#endif
    return 0;
}

static void platform_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static void peer_from_addr(const struct sockaddr_in *addr, vc_net_peer_t *peer)
{
    /* s_addr is in network order: first octet first. */
    memcpy(peer->ip, &addr->sin_addr.s_addr, sizeof peer->ip);
    peer->port = ntohs(addr->sin_port);
}

/* ------------------------------------------------------------------------ */
/* Opening                                                                  */
/* ------------------------------------------------------------------------ */

static int open_tcp_listener(vc_net_t *net, struct sockaddr_in addr, const char *bind_text)
{
    char err[200];

    net->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (net->listen_sock == VC_INVALID_SOCKET) {
        log_msg(VC_NET_LOG_ERROR, "TCP socket() failed: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        return -1;
    }
    if (sock_set_bind_policy(net->listen_sock) != 0) {
        log_msg(VC_NET_LOG_WARN, "could not set the TCP bind policy: %s",
                sock_strerror(sock_errno(), err, sizeof err));
    }
    if (sock_set_nonblocking(net->listen_sock) != 0) {
        log_msg(VC_NET_LOG_ERROR, "could not make the TCP socket non-blocking: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        return -1;
    }
    /* Same port as UDP, so a client needs only one number. */
    addr.sin_port = htons(net->port);
    if (bind(net->listen_sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_msg(VC_NET_LOG_ERROR, "TCP bind to %s:%u failed: %s", bind_text,
                (unsigned)net->port, sock_strerror(sock_errno(), err, sizeof err));
        return -1;
    }
    if (listen(net->listen_sock, VC_NET_LISTEN_BACKLOG) != 0) {
        log_msg(VC_NET_LOG_ERROR, "listen() failed: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        return -1;
    }
    return 0;
}

int vc_net_open(const vc_net_config_t *config, vc_net_t **out)
{
    struct sockaddr_in addr;
    socklen_t          addr_len = sizeof addr;
    const char        *bind_text;
    vc_net_t          *net;
    char               err[200];
    int                i;

    if (out == NULL) {
        return -1;
    }
    *out = NULL;
    if (config == NULL || config->on_input == NULL) {
        log_msg(VC_NET_LOG_ERROR, "vc_net_open: config and on_input are required");
        return -1;
    }
    if (config->tcp && (config->on_connect == NULL || config->on_data == NULL ||
                        config->on_disconnect == NULL)) {
        log_msg(VC_NET_LOG_ERROR,
                "vc_net_open: tcp needs on_connect, on_data and on_disconnect");
        return -1;
    }
    bind_text = config->bind_address != NULL ? config->bind_address : "0.0.0.0";

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    if (config->bind_address == NULL) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, config->bind_address, &addr.sin_addr) != 1) {
        log_msg(VC_NET_LOG_ERROR, "'%s' is not a valid IPv4 address", bind_text);
        return -1;
    }

    if (platform_startup() != 0) {
        return -1;
    }

    net = (vc_net_t *)calloc(1, sizeof *net);
    if (net == NULL) {
        log_msg(VC_NET_LOG_ERROR, "out of memory allocating the listener");
        platform_cleanup();
        return -1;
    }
    net->listen_sock = VC_INVALID_SOCKET;
    for (i = 0; i < VC_NET_MAX_CONNS; i++) {
        net->conns[i].sock = VC_INVALID_SOCKET;
    }
    net->on_input = config->on_input;
    net->on_connect = config->on_connect;
    net->on_data = config->on_data;
    net->on_disconnect = config->on_disconnect;
    net->user = config->user;

    net->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (net->sock == VC_INVALID_SOCKET) {
        log_msg(VC_NET_LOG_ERROR, "socket() failed: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        goto fail;
    }
    if (sock_set_bind_policy(net->sock) != 0) {
        /* Not fatal: the listener still works, only rebinding is affected. */
        log_msg(VC_NET_LOG_WARN, "could not set the socket bind policy: %s",
                sock_strerror(sock_errno(), err, sizeof err));
    }
    if (sock_set_nonblocking(net->sock) != 0) {
        log_msg(VC_NET_LOG_ERROR, "could not make the socket non-blocking: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        goto fail;
    }
    sock_ignore_icmp_errors(net->sock);
    if (bind(net->sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_msg(VC_NET_LOG_ERROR, "bind to %s:%u failed: %s", bind_text,
                (unsigned)config->port,
                sock_strerror(sock_errno(), err, sizeof err));
        goto fail;
    }
    if (getsockname(net->sock, (struct sockaddr *)&addr, &addr_len) != 0) {
        log_msg(VC_NET_LOG_ERROR, "getsockname() failed: %s",
                sock_strerror(sock_errno(), err, sizeof err));
        goto fail;
    }
    net->port = ntohs(addr.sin_port);

    if (config->tcp) {
        if (open_tcp_listener(net, addr, bind_text) != 0) {
            goto fail;
        }
        log_msg(VC_NET_LOG_INFO, "listening on %s:%u (udp input, tcp control)",
                bind_text, (unsigned)net->port);
    } else {
        log_msg(VC_NET_LOG_INFO, "listening for input on udp %s:%u", bind_text,
                (unsigned)net->port);
    }
    *out = net;
    return 0;

fail:
    if (net->listen_sock != VC_INVALID_SOCKET) {
        sock_close(net->listen_sock);
    }
    if (net->sock != VC_INVALID_SOCKET) {
        sock_close(net->sock);
    }
    free(net);
    platform_cleanup();
    return -1;
}

/* ------------------------------------------------------------------------ */
/* UDP input                                                                */
/* ------------------------------------------------------------------------ */

typedef enum {
    RECV_EMPTY,       /* nothing left to read for now */
    RECV_DROPPED,     /* a datagram was consumed and discarded */
    RECV_DELIVERED,   /* a datagram was handed to the callback */
    RECV_FATAL        /* the socket is unusable */
} recv_result_t;

static recv_result_t receive_one(vc_net_t *net)
{
    uint8_t                buf[VC_NET_RECV_BUFFER];
    struct sockaddr_in     from;
    socklen_t              from_len = sizeof from;
    vc_net_input_t         input;
    vc_parser_result_t     decoded;
    const uint8_t         *ip;
    long                   received;

    memset(&from, 0, sizeof from);
    received = (long)recvfrom(net->sock, (char *)buf, sizeof buf, 0,
                              (struct sockaddr *)&from, &from_len);
    if (received < 0) {
        int  code = sock_errno();
        char err[200];

        if (sock_would_block(code)) {
            return RECV_EMPTY;
        }
        if (sock_interrupted(code)) {
            return RECV_DROPPED;
        }
        if (sock_icmp_error(code)) {
            /* About a datagram we sent earlier; nothing was received and the
             * socket is fine. Keep reading. */
            net->stats.icmp_errors_ignored++;
            return RECV_DROPPED;
        }
#ifdef _WIN32
        /* Windows reports a datagram larger than the buffer as an error and
         * discards it; POSIX truncates it, which the size check below catches. */
        if (code == WSAEMSGSIZE) {
            net->stats.datagrams_received++;
            net->stats.dropped_oversized++;
            if (should_log(net->stats.dropped_oversized)) {
                log_msg(VC_NET_LOG_WARN,
                        "dropped an oversized datagram (%lu so far)",
                        (unsigned long)net->stats.dropped_oversized);
            }
            return RECV_DROPPED;
        }
#endif
        net->stats.receive_errors++;
        net->recv_failures++;
        if (net->recv_failures >= VC_NET_MAX_RECV_FAILURES) {
            log_msg(VC_NET_LOG_ERROR,
                    "recvfrom() failed %u times in a row, giving up: %s",
                    net->recv_failures, sock_strerror(code, err, sizeof err));
            return RECV_FATAL;
        }
        if (should_log(net->stats.receive_errors)) {
            log_msg(VC_NET_LOG_WARN, "recvfrom() failed (%lu so far): %s",
                    (unsigned long)net->stats.receive_errors,
                    sock_strerror(code, err, sizeof err));
        }
        /* Leave the rest for the next poll instead of hammering the socket. */
        return RECV_EMPTY;
    }

    net->recv_failures = 0;
    net->stats.datagrams_received++;
    ip = (const uint8_t *)&from.sin_addr.s_addr;   /* network order: first octet first */

    if (received > VC_INPUT_DATAGRAM_MAX) {
        net->stats.dropped_oversized++;
        if (should_log(net->stats.dropped_oversized)) {
            log_msg(VC_NET_LOG_WARN,
                    "dropped an oversized datagram from %u.%u.%u.%u:%u (%lu so far)",
                    ip[0], ip[1], ip[2], ip[3], (unsigned)ntohs(from.sin_port),
                    (unsigned long)net->stats.dropped_oversized);
        }
        return RECV_DROPPED;
    }

    decoded = vc_parser_decode(buf, (size_t)received, &input.packet);
    if (decoded != VC_PARSE_OK) {
        net->stats.dropped_malformed++;
        if (should_log(net->stats.dropped_malformed)) {
            log_msg(VC_NET_LOG_WARN,
                    "dropped a %ld-byte datagram from %u.%u.%u.%u:%u: %s (%lu so far)",
                    received, ip[0], ip[1], ip[2], ip[3],
                    (unsigned)ntohs(from.sin_port),
                    vc_parser_strerror(decoded),
                    (unsigned long)net->stats.dropped_malformed);
        }
        return RECV_DROPPED;
    }

    peer_from_addr(&from, &input.peer);
    net->stats.inputs_delivered++;
    net->on_input(&input, net->user);
    return RECV_DELIVERED;
}

/* Returns the number of inputs delivered, or -1 if the socket is unusable. */
static int receive_batch(vc_net_t *net)
{
    int delivered = 0;
    int i;

    for (i = 0; i < VC_NET_MAX_BATCH; i++) {
        recv_result_t result = receive_one(net);
        if (result == RECV_FATAL) {
            return -1;
        }
        if (result == RECV_EMPTY) {
            break;
        }
        if (result == RECV_DELIVERED) {
            delivered++;
        }
    }
    return delivered;
}

int vc_net_send_to(vc_net_t *net, const vc_net_peer_t *peer, const uint8_t *data,
                   size_t size)
{
    struct sockaddr_in to;
    long               sent;

    if (net == NULL || peer == NULL || data == NULL) {
        return -1;
    }
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(peer->port);
    memcpy(&to.sin_addr.s_addr, peer->ip, sizeof peer->ip);

    sent = (long)sendto(net->sock, (const char *)data, (int)size, 0,
                        (struct sockaddr *)&to, sizeof to);
    if (sent != (long)size) {
        char err[200];

        net->send_to_failures++;
        if (should_log(net->send_to_failures)) {
            log_msg(VC_NET_LOG_WARN, "sendto %u.%u.%u.%u:%u failed (%lu so far): %s",
                    peer->ip[0], peer->ip[1], peer->ip[2], peer->ip[3],
                    (unsigned)peer->port, (unsigned long)net->send_to_failures,
                    sock_strerror(sock_errno(), err, sizeof err));
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* TCP control connections                                                  */
/* ------------------------------------------------------------------------ */

static int conn_valid(const vc_net_t *net, int conn)
{
    return net != NULL && conn >= 0 && conn < VC_NET_MAX_CONNS && net->conns[conn].open;
}

/* Frees a connection the peer closed or that failed, and tells the caller. */
static void drop_conn(vc_net_t *net, int conn)
{
    conn_t *c = &net->conns[conn];

    sock_close(c->sock);
    c->sock = VC_INVALID_SOCKET;
    c->open = 0;
    c->failed = 0;
    c->out_len = 0;
    net->on_disconnect(conn, net->user);
}

/* Sends what is queued until the socket would block. Returns 0, or -1 if the
 * connection is broken. */
static int flush_conn(conn_t *c)
{
    while (c->out_len > 0) {
        long sent = (long)send(c->sock, (const char *)c->out, (int)c->out_len,
                               VC_SEND_FLAGS);
        if (sent > 0) {
            c->out_len -= (size_t)sent;
            memmove(c->out, c->out + sent, c->out_len);
            continue;
        }
        if (sent < 0) {
            int code = sock_errno();
            if (sock_would_block(code)) {
                return 0;
            }
            if (sock_interrupted(code)) {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

int vc_net_send(vc_net_t *net, int conn, const uint8_t *data, size_t size)
{
    conn_t *c;

    if (!conn_valid(net, conn) || (data == NULL && size > 0)) {
        return -1;
    }
    c = &net->conns[conn];
    if (c->failed) {
        return -1;
    }
    if (size > sizeof c->out - c->out_len) {
        c->failed = 1;
        log_msg(VC_NET_LOG_WARN, "connection %d is not reading; output buffer full", conn);
        return -1;
    }
    memcpy(c->out + c->out_len, data, size);
    c->out_len += size;
    if (flush_conn(c) != 0) {
        c->failed = 1;
        return -1;
    }
    return 0;
}

void vc_net_disconnect(vc_net_t *net, int conn)
{
    conn_t *c;
    uint8_t discard[VC_NET_TCP_CHUNK];
    int     i;

    if (!conn_valid(net, conn)) {
        return;
    }
    c = &net->conns[conn];
    if (!c->failed) {
        flush_conn(c);
    }
    /* Half-close so what was sent (an ERROR, say) is delivered, and read
     * away anything unread: closing with unread data makes some stacks
     * reset the connection, which can discard our last message. */
    shutdown(c->sock, VC_SHUT_SEND);
    for (i = 0; i < VC_NET_TCP_MAX_READS; i++) {
        if (recv(c->sock, (char *)discard, sizeof discard, 0) <= 0) {
            break;
        }
    }
    sock_close(c->sock);
    c->sock = VC_INVALID_SOCKET;
    c->open = 0;
    c->failed = 0;
    c->out_len = 0;
}

static void read_conn(vc_net_t *net, int conn)
{
    uint8_t     buf[VC_NET_TCP_CHUNK];
    vc_socket_t sock = net->conns[conn].sock;
    int         i;

    for (i = 0; i < VC_NET_TCP_MAX_READS; i++) {
        long received = (long)recv(sock, (char *)buf, sizeof buf, 0);

        if (received > 0) {
            net->on_data(conn, buf, (size_t)received, net->user);
            /* The callback may have closed it. */
            if (!net->conns[conn].open) {
                return;
            }
            continue;
        }
        if (received < 0) {
            int code = sock_errno();
            if (sock_would_block(code)) {
                return;
            }
            if (sock_interrupted(code)) {
                continue;
            }
        }
        /* 0: orderly close by the peer. Anything else: reset or failure. */
        drop_conn(net, conn);
        return;
    }
}

static void accept_all(vc_net_t *net)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t          from_len = sizeof from;
        vc_socket_t        sock;
        vc_net_peer_t      peer;
        char               err[200];
        int                conn;

        memset(&from, 0, sizeof from);
        sock = accept(net->listen_sock, (struct sockaddr *)&from, &from_len);
        if (sock == VC_INVALID_SOCKET) {
            int code = sock_errno();
            if (sock_would_block(code)) {
                return;
            }
            if (sock_interrupted(code) || sock_accept_aborted(code)) {
                continue;
            }
            log_msg(VC_NET_LOG_WARN, "accept() failed: %s",
                    sock_strerror(code, err, sizeof err));
            return;
        }
        peer_from_addr(&from, &peer);

        for (conn = 0; conn < VC_NET_MAX_CONNS; conn++) {
            if (!net->conns[conn].open) {
                break;
            }
        }
        if (conn == VC_NET_MAX_CONNS) {
            net->stats.connections_refused++;
            if (should_log(net->stats.connections_refused)) {
                log_msg(VC_NET_LOG_WARN,
                        "refused a connection from %u.%u.%u.%u:%u: all %d in use (%lu so far)",
                        peer.ip[0], peer.ip[1], peer.ip[2], peer.ip[3], (unsigned)peer.port,
                        VC_NET_MAX_CONNS, (unsigned long)net->stats.connections_refused);
            }
            sock_close(sock);
            continue;
        }
        if (sock_set_nonblocking(sock) != 0) {
            log_msg(VC_NET_LOG_WARN, "could not make a connection non-blocking: %s",
                    sock_strerror(sock_errno(), err, sizeof err));
            sock_close(sock);
            continue;
        }
        sock_tune_connection(sock);

        net->conns[conn].open = 1;
        net->conns[conn].failed = 0;
        net->conns[conn].sock = sock;
        net->conns[conn].out_len = 0;
        net->stats.connections_accepted++;
        net->on_connect(conn, &peer, net->user);
    }
}

/* ------------------------------------------------------------------------ */
/* Polling                                                                  */
/* ------------------------------------------------------------------------ */

int vc_net_poll(vc_net_t *net, int timeout_ms)
{
    struct pollfd fds[2 + VC_NET_MAX_CONNS];
    int           conn_of[2 + VC_NET_MAX_CONNS];
    int           count = 0;
    int           listen_index = -1;
    int           ready;
    int           delivered = 0;
    int           i;

    if (net == NULL) {
        return -1;
    }

    memset(fds, 0, sizeof fds);
    fds[count].fd = net->sock;
    fds[count].events = VC_POLL_READABLE;
    conn_of[count++] = -1;
    if (net->listen_sock != VC_INVALID_SOCKET) {
        listen_index = count;
        fds[count].fd = net->listen_sock;
        fds[count].events = VC_POLL_READABLE;
        conn_of[count++] = -1;
    }
    for (i = 0; i < VC_NET_MAX_CONNS; i++) {
        const conn_t *c = &net->conns[i];
        if (c->open) {
            fds[count].fd = c->sock;
            fds[count].events = VC_POLL_READABLE;
            if (c->out_len > 0 && !c->failed) {
                fds[count].events |= VC_POLL_WRITABLE;
            }
            conn_of[count++] = i;
        }
    }

    ready = sock_poll(fds, count, timeout_ms);
    if (ready < 0) {
        int  code = sock_errno();
        char err[200];

        if (sock_interrupted(code)) {
            return 0;
        }
        log_msg(VC_NET_LOG_ERROR, "poll() failed: %s",
                sock_strerror(code, err, sizeof err));
        return -1;
    }
    if (ready == 0) {
        return 0;
    }

    /* UDP input. POLLERR is not treated as fatal here: recvfrom() reports the
     * actual error and receive_one() decides whether it is worth giving up. */
    if (fds[0].revents & POLLNVAL) {
        log_msg(VC_NET_LOG_ERROR, "the listening socket is no longer valid");
        return -1;
    }
    if (fds[0].revents != 0) {
        delivered = receive_batch(net);
        if (delivered < 0) {
            return -1;
        }
    }

    /* Existing connections, before accepting new ones, so an index in fds
     * still names the connection it was polled for. */
    for (i = 0; i < count; i++) {
        int conn = conn_of[i];
        if (conn < 0 || fds[i].revents == 0) {
            continue;
        }
        /* A callback earlier in this loop may have closed it. */
        if (!net->conns[conn].open || net->conns[conn].sock != fds[i].fd) {
            continue;
        }
        if ((fds[i].revents & VC_POLL_WRITABLE) && flush_conn(&net->conns[conn]) != 0) {
            drop_conn(net, conn);
            continue;
        }
        if (fds[i].revents & (VC_POLL_READABLE | POLLHUP | POLLERR | POLLNVAL)) {
            read_conn(net, conn);
        }
    }

    if (listen_index >= 0 && (fds[listen_index].revents & VC_POLL_READABLE)) {
        accept_all(net);
    }
    return delivered;
}

int vc_net_run(vc_net_t *net)
{
    if (net == NULL) {
        return -1;
    }
    while (!net->stop_requested) {
        if (vc_net_poll(net, VC_NET_RUN_TIMEOUT_MS) < 0) {
            return -1;
        }
    }
    return 0;
}

void vc_net_stop(vc_net_t *net)
{
    if (net != NULL) {
        net->stop_requested = 1;
    }
}

uint16_t vc_net_port(const vc_net_t *net)
{
    return net != NULL ? net->port : 0;
}

void vc_net_get_stats(const vc_net_t *net, vc_net_stats_t *out)
{
    if (net != NULL && out != NULL) {
        *out = net->stats;
    }
}

void vc_net_close(vc_net_t *net)
{
    int i;

    if (net == NULL) {
        return;
    }
    for (i = 0; i < VC_NET_MAX_CONNS; i++) {
        if (net->conns[i].open) {
            sock_close(net->conns[i].sock);
        }
    }
    if (net->listen_sock != VC_INVALID_SOCKET) {
        sock_close(net->listen_sock);
    }
    sock_close(net->sock);
    free(net);
    platform_cleanup();
}
