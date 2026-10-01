/*
 * network.c - UDP listener for input packets from the mobile client.
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
#else
typedef int vc_socket_t;
#  define VC_INVALID_SOCKET  (-1)
#  define VC_POLL_READABLE   POLLIN
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

struct vc_net {
    vc_socket_t     sock;
    uint16_t        port;
    vc_net_input_cb on_input;
    void           *user;
    volatile int    stop_requested;
    unsigned        recv_failures;   /* consecutive */
    vc_net_stats_t  stats;
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

static int sock_poll(struct pollfd *fds, int timeout_ms)
{
#ifdef _WIN32
    return WSAPoll(fds, 1, timeout_ms);
#else
    return poll(fds, 1, timeout_ms);
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

/* ------------------------------------------------------------------------ */
/* Listener                                                                 */
/* ------------------------------------------------------------------------ */

int vc_net_open(const vc_net_config_t *config, vc_net_t **out)
{
    struct sockaddr_in addr;
    socklen_t          addr_len = sizeof addr;
    const char        *bind_text;
    vc_net_t          *net;
    char               err[200];

    if (out == NULL) {
        return -1;
    }
    *out = NULL;
    if (config == NULL || config->on_input == NULL) {
        log_msg(VC_NET_LOG_ERROR, "vc_net_open: config and on_input are required");
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
    net->on_input = config->on_input;
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

    log_msg(VC_NET_LOG_INFO, "listening for input on udp %s:%u", bind_text,
            (unsigned)net->port);
    *out = net;
    return 0;

fail:
    if (net->sock != VC_INVALID_SOCKET) {
        sock_close(net->sock);
    }
    free(net);
    platform_cleanup();
    return -1;
}

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

    memcpy(input.peer.ip, ip, sizeof input.peer.ip);
    input.peer.port = ntohs(from.sin_port);
    net->stats.inputs_delivered++;
    net->on_input(&input, net->user);
    return RECV_DELIVERED;
}

int vc_net_poll(vc_net_t *net, int timeout_ms)
{
    struct pollfd pfd;
    int           ready;
    int           delivered = 0;
    int           i;

    if (net == NULL) {
        return -1;
    }

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = net->sock;
    pfd.events = VC_POLL_READABLE;

    ready = sock_poll(&pfd, timeout_ms);
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
    if (pfd.revents & POLLNVAL) {
        log_msg(VC_NET_LOG_ERROR, "the listening socket is no longer valid");
        return -1;
    }

    /* POLLERR is not treated as fatal here: recvfrom() reports the actual
     * error and receive_one() decides whether it is worth giving up. */
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
    if (net == NULL) {
        return;
    }
    sock_close(net->sock);
    free(net);
    platform_cleanup();
}
