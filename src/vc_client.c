/*
 * vc_client.c - the phone side of the protocol: one controller's link to vcd.
 * See vc_client.h for the contract.
 */

/* Feature macros before any system header. Darwin hides SO_NOSIGPIPE and
 * SO_NET_SERVICE_TYPE under strict POSIX, so ask for its full API there. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#  include <sys/prctl.h>      /* PR_SET_TIMERSLACK (Linux, Android) */
#endif
#if defined(__APPLE__)
#  include <pthread.h>        /* pthread_set_qos_class_self_np */
#endif

#include "encoder.h"
#include "protocol.h"
#include "vc_client.h"

#define DEFAULT_RATE_HZ            120
#define DEFAULT_MIN_INTERVAL_US    2000
#define DEFAULT_CONNECT_TIMEOUT_MS 3000
/* Client PINGs, for the round-trip time in the stats. The host has its own. */
#define PING_INTERVAL_US           1000000u
/* Below this much time to the next deadline, sleep precisely instead of
 * polling (poll() only has millisecond resolution). */
#define PRECISE_SLEEP_US           1500u
/* DSCP EF (46) in the TOS byte. Wi-Fi access points map it to the WMM voice
 * queue, the one with the shortest contention window. */
#define TOS_EXPEDITED              0xB8

#ifdef MSG_NOSIGNAL
#  define SEND_FLAGS MSG_NOSIGNAL
#else
#  define SEND_FLAGS 0   /* Darwin: SO_NOSIGPIPE on the socket instead */
#endif

/* Triple buffer: `middle` holds a buffer index, plus DIRTY when the input
 * thread has published a state the sender has not taken yet. */
#define TB_DIRTY 4u
#define TB_INDEX 3u

struct vc_client {
    /* Configuration; read-only after create. */
    struct sockaddr_in addr;
    char               host_text[INET_ADDRSTRLEN];
    char               name[VC_CLIENT_NAME_LEN + 1];
    uint8_t            controller_type;
    uint8_t            caps;
    uint64_t           interval_us;
    uint64_t           min_interval_us;
    int                rate_only;
    int                connect_timeout_ms;
    void             (*on_event)(const vc_client_event_t *event, void *user);
    void              *user;

    /* Input handoff. buffers[back] belongs to the input thread,
     * buffers[front] to the sender, buffers[middle] to whoever swaps next. */
    vc_input_state_t   buffers[3];
    atomic_uint        middle;
    unsigned           back;
    unsigned           front;

    /* Shared between threads. */
    atomic_int         stop;
    int                wake[2];   /* pipe: poked to wake the sender */
    atomic_uint        sent;
    atomic_uint        send_errors;
    atomic_uint        rtt_us;

    /* Sender thread only. */
    int                tcp;
    int                udp;
    vc_proto_reader_t  reader;
    int                connected;   /* WELCOME received */
    uint32_t           token;
    uint8_t            slot;
    uint8_t            granted;     /* VC_CAP_* */
    uint16_t           host_timeout_ms;
    uint8_t            allowed_flags;
    uint16_t           tx_sequence;
    uint16_t           input_sequence;
    uint32_t           ping_echo;
    uint64_t           ping_sent_us;
    uint64_t           epoch_us;
    vc_client_end_t    end;
    uint8_t            error;
    char               message[160];
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static void sleep_us(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static int set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

/* Latency-related socket options. Each is best effort: a missing one costs
 * some latency, not correctness. */
static void tune_socket(int fd, int tcp)
{
    int one = 1;

#ifdef IP_TOS
    {
        int tos = TOS_EXPEDITED;
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    }
#endif
#if defined(SO_NET_SERVICE_TYPE) && defined(NET_SERVICE_TYPE_VO)
    {
        /* Apple's per-socket service class; also selects the Wi-Fi queue. */
        int type = NET_SERVICE_TYPE_VO;
        setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &type, sizeof type);
    }
#endif
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (tcp) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void poke(vc_client_t *c)
{
    char byte = 1;
    if (write(c->wake[1], &byte, 1) < 0) {
        /* EAGAIN: the pipe is full, so a wake-up is already pending. */
    }
}

static void drain_wake(vc_client_t *c)
{
    char buf[64];
    while (read(c->wake[0], buf, sizeof buf) > 0) {
    }
}

static const char *error_name(uint8_t code)
{
    switch (code) {
    case VC_ERROR_BAD_VERSION: return "bad protocol version";
    case VC_ERROR_NO_SLOT:     return "no free slot";
    case VC_ERROR_UNSUPPORTED: return "controller type not supported by the host";
    case VC_ERROR_PROTOCOL:    return "protocol error";
    case VC_ERROR_TIMEOUT:     return "timeout";
    case VC_ERROR_DRIVER:      return "driver error on the host";
    default:                   return "unknown error";
    }
}

/* Records how the run ends. Returns -1, for `return set_end(...)`. */
#ifdef __GNUC__
static int set_end(vc_client_t *c, vc_client_end_t end, uint8_t error, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#endif

static int set_end(vc_client_t *c, vc_client_end_t end, uint8_t error, const char *fmt, ...)
{
    va_list args;

    c->end = end;
    c->error = error;
    va_start(args, fmt);
    vsnprintf(c->message, sizeof c->message, fmt, args);
    va_end(args);
    return -1;
}

static void emit(vc_client_t *c, const vc_client_event_t *event)
{
    if (c->on_event != NULL) {
        c->on_event(event, c->user);
    }
}

/* Timing settings for the calling (sender) thread, whatever created it.
 * Each is best effort. */
static void tune_thread(void)
{
#if defined(__linux__) && defined(PR_SET_TIMERSLACK)
    /* The kernel may defer a timer wake-up by the thread's "slack" to batch
     * wake-ups: 50 us by default, and Android sets far more for apps it
     * considers in the background. 1 us keeps our sub-millisecond sleeps
     * on time. */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL);
#endif
#if defined(__APPLE__)
    /* The app already starts the thread at .userInteractive; this makes it
     * so for any caller. Same class as the main (UI) thread. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

/* ------------------------------------------------------------------------ */
/* Input handoff                                                            */
/* ------------------------------------------------------------------------ */

void vc_client_set_state(vc_client_t *c, const vc_input_state_t *state)
{
    unsigned previous;

    if (c == NULL || state == NULL) {
        return;
    }
    c->buffers[c->back] = *state;
    /* Publish our buffer and take whichever one was in the middle. */
    previous = atomic_exchange_explicit(&c->middle, c->back | TB_DIRTY, memory_order_acq_rel);
    c->back = previous & TB_INDEX;
    if (!c->rate_only) {
        poke(c);
    }
}

static int has_fresh_state(vc_client_t *c)
{
    return (atomic_load_explicit(&c->middle, memory_order_acquire) & TB_DIRTY) != 0;
}

/* The newest published state (or the last one again if nothing is new). */
static void take_state(vc_client_t *c, vc_input_state_t *out)
{
    if (has_fresh_state(c)) {
        unsigned previous = atomic_exchange_explicit(&c->middle, c->front, memory_order_acq_rel);
        c->front = previous & TB_INDEX;
    }
    *out = c->buffers[c->front];
}

/* ------------------------------------------------------------------------ */
/* Control channel                                                          */
/* ------------------------------------------------------------------------ */

static int send_control(vc_client_t *c, vc_proto_msg_t *msg)
{
    uint8_t buf[VC_PROTO_MAX_FRAME];
    size_t  size;

    msg->sequence = c->tx_sequence++;
    size = vc_proto_encode(msg, buf, sizeof buf);
    /* Frames are a few bytes and the socket buffer nearly empty, so anything
     * short of a full write means the connection is unusable. */
    if (size == 0 || send(c->tcp, buf, size, SEND_FLAGS) != (ssize_t)size) {
        return set_end(c, VC_CLIENT_END_CONNECTION_LOST, 0, "could not send %s: %s",
                       vc_proto_type_name(msg->type), strerror(errno));
    }
    return 0;
}

static int handle_message(vc_client_t *c, const vc_proto_msg_t *msg)
{
    vc_proto_msg_t    reply;
    vc_client_event_t event;

    switch (msg->type) {
    case VC_MSG_WELCOME:
        if (c->connected) {
            return set_end(c, VC_CLIENT_END_PROTOCOL, 0, "second WELCOME");
        }
        c->connected = 1;
        c->token = msg->token;
        c->slot = msg->slot;
        c->granted = msg->caps;
        c->host_timeout_ms = msg->timeout_ms;
        c->allowed_flags = (uint8_t)(((msg->caps & VC_CAP_MOTION) ? VC_INPUT_HAS_MOTION : 0u) |
                                     ((msg->caps & VC_CAP_TOUCH) ? VC_INPUT_HAS_TOUCH : 0u));
        return 0;

    case VC_MSG_PING:
        memset(&reply, 0, sizeof reply);
        reply.type = VC_MSG_PONG;
        reply.echo = msg->echo;
        return send_control(c, &reply);

    case VC_MSG_PONG:
        if (msg->echo == c->ping_echo && c->ping_sent_us != 0) {
            atomic_store_explicit(&c->rtt_us, (unsigned)(now_us() - c->ping_sent_us),
                                  memory_order_relaxed);
            c->ping_sent_us = 0;
        }
        return 0;

    case VC_MSG_FEEDBACK:
        memset(&event, 0, sizeof event);
        event.type = VC_CLIENT_EVENT_FEEDBACK;
        event.feedback = msg->feedback;
        event.message = "feedback";
        emit(c, &event);
        return 0;

    case VC_MSG_ERROR:
        return set_end(c, c->connected ? VC_CLIENT_END_HOST_ERROR : VC_CLIENT_END_REFUSED,
                       msg->error, "host sent ERROR %u: %s", (unsigned)msg->error,
                       error_name(msg->error));

    case VC_MSG_BYE:
        return set_end(c, VC_CLIENT_END_HOST_BYE, 0, "host closed the session (BYE)");

    default:
        return set_end(c, VC_CLIENT_END_PROTOCOL, 0, "unexpected %s from the host",
                       vc_proto_type_name(msg->type));
    }
}

/* Reads and handles whatever the host has sent, without waiting. Returns 0,
 * or -1 when the session has ended. */
static int service_tcp(vc_client_t *c)
{
    uint8_t           buf[256];
    const uint8_t    *data;
    size_t            size;
    vc_proto_msg_t    msg;
    vc_proto_result_t result;
    ssize_t           received;

    for (;;) {
        received = recv(c->tcp, buf, sizeof buf, 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return set_end(c, VC_CLIENT_END_CONNECTION_LOST, 0, "connection lost: %s",
                           strerror(errno));
        }
        if (received == 0) {
            return set_end(c, VC_CLIENT_END_CONNECTION_LOST, 0, "host closed the connection");
        }

        data = buf;
        size = (size_t)received;
        while ((result = vc_proto_read(&c->reader, &data, &size, &msg)) == VC_PROTO_OK) {
            if (handle_message(c, &msg) != 0) {
                return -1;
            }
        }
        if (result != VC_PROTO_NEED_MORE) {
            return set_end(c, VC_CLIENT_END_PROTOCOL, 0, "bad data from the host: %s",
                           vc_proto_strerror(result));
        }
    }
}

/* Waits up to timeout_ms for a wake-up or control data, and handles the
 * data. Returns 0, or -1 when the session has ended. */
static int wait_events(vc_client_t *c, int timeout_ms, short tcp_events)
{
    struct pollfd fds[2];
    int           ready;

    memset(fds, 0, sizeof fds);
    fds[0].fd = c->wake[0];
    fds[0].events = POLLIN;
    fds[1].fd = c->tcp;
    fds[1].events = tcp_events;

    ready = poll(fds, 2, timeout_ms);
    if (ready < 0) {
        return errno == EINTR ? 0 : set_end(c, VC_CLIENT_END_SYSTEM, 0, "poll failed: %s",
                                            strerror(errno));
    }
    if (fds[0].revents & POLLIN) {
        drain_wake(c);
    }
    if ((tcp_events & POLLIN) && fds[1].revents != 0) {
        return service_tcp(c);
    }
    return 0;
}

static int remaining_ms(uint64_t deadline)
{
    uint64_t now = now_us();
    return now >= deadline ? 0 : (int)((deadline - now + 999u) / 1000u);
}

/* ------------------------------------------------------------------------ */
/* Session phases                                                           */
/* ------------------------------------------------------------------------ */

static int connect_tcp(vc_client_t *c)
{
    uint64_t  deadline = now_us() + (uint64_t)c->connect_timeout_ms * 1000u;
    int       err = 0;
    socklen_t err_len = sizeof err;

    c->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->tcp < 0 || set_nonblocking_cloexec(c->tcp) != 0) {
        return set_end(c, VC_CLIENT_END_SYSTEM, 0, "could not create a TCP socket: %s",
                       strerror(errno));
    }
    tune_socket(c->tcp, 1);

    if (connect(c->tcp, (const struct sockaddr *)&c->addr, sizeof c->addr) == 0) {
        return 0;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        return set_end(c, VC_CLIENT_END_CONNECT_FAILED, 0, "could not connect to %s:%u: %s",
                       c->host_text, (unsigned)ntohs(c->addr.sin_port), strerror(errno));
    }

    /* Wait for the connection, staying responsive to stop. */
    for (;;) {
        struct pollfd fds[2];
        int           wait = remaining_ms(deadline);

        if (atomic_load(&c->stop)) {
            return set_end(c, VC_CLIENT_END_STOPPED, 0, "stopped while connecting");
        }
        if (wait == 0) {
            return set_end(c, VC_CLIENT_END_CONNECT_FAILED, 0,
                           "no answer from %s:%u within %d ms", c->host_text,
                           (unsigned)ntohs(c->addr.sin_port), c->connect_timeout_ms);
        }
        memset(fds, 0, sizeof fds);
        fds[0].fd = c->wake[0];
        fds[0].events = POLLIN;
        fds[1].fd = c->tcp;
        fds[1].events = POLLOUT;
        if (poll(fds, 2, wait) < 0 && errno != EINTR) {
            return set_end(c, VC_CLIENT_END_SYSTEM, 0, "poll failed: %s", strerror(errno));
        }
        if (fds[0].revents & POLLIN) {
            drain_wake(c);
        }
        if (fds[1].revents != 0) {
            break;
        }
    }
    if (getsockopt(c->tcp, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
        return set_end(c, VC_CLIENT_END_CONNECT_FAILED, 0, "could not connect to %s:%u: %s",
                       c->host_text, (unsigned)ntohs(c->addr.sin_port),
                       strerror(err != 0 ? err : errno));
    }
    return 0;
}

static int handshake(vc_client_t *c)
{
    uint64_t       deadline = now_us() + (uint64_t)c->connect_timeout_ms * 1000u;
    vc_proto_msg_t hello;

    memset(&hello, 0, sizeof hello);
    hello.type = VC_MSG_HELLO;
    hello.controller_type = c->controller_type;
    hello.caps = c->caps;
    memcpy(hello.name, c->name, sizeof hello.name);
    if (send_control(c, &hello) != 0) {
        return -1;
    }

    while (!c->connected) {
        int wait = remaining_ms(deadline);

        if (atomic_load(&c->stop)) {
            return set_end(c, VC_CLIENT_END_STOPPED, 0, "stopped during the handshake");
        }
        if (wait == 0) {
            return set_end(c, VC_CLIENT_END_TIMEOUT, 0, "no answer to HELLO within %d ms",
                           c->connect_timeout_ms);
        }
        if (wait_events(c, wait, POLLIN) != 0) {
            return -1;   /* refused, closed, or bad data */
        }
    }
    return 0;
}

static int open_udp(vc_client_t *c)
{
    c->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (c->udp < 0 || set_nonblocking_cloexec(c->udp) != 0) {
        return set_end(c, VC_CLIENT_END_SYSTEM, 0, "could not create a UDP socket: %s",
                       strerror(errno));
    }
    tune_socket(c->udp, 0);
    /* A connected UDP socket: the route is looked up once, and send() is a
     * little cheaper than sendto() on every packet. */
    if (connect(c->udp, (const struct sockaddr *)&c->addr, sizeof c->addr) != 0) {
        return set_end(c, VC_CLIENT_END_SYSTEM, 0, "could not set up UDP to %s: %s",
                       c->host_text, strerror(errno));
    }
    return 0;
}

static void send_input(vc_client_t *c, uint64_t now)
{
    vc_input_state_t state;
    uint8_t          buf[VC_INPUT_DATAGRAM_MAX];
    size_t           size;

    take_state(c, &state);
    state.flags &= c->allowed_flags;              /* only what the host granted */
    state.timestamp_us = (uint32_t)(now - c->epoch_us);
    size = vc_encode_input(&state, c->token, c->input_sequence++, buf, sizeof buf);

    /* Non-blocking: if the OS buffer is full the datagram is dropped, which
     * is right for latest-wins input. ECONNREFUSED here only reports an ICMP
     * error for an earlier datagram (vcd restarting); keep going. */
    if (size > 0 && send(c->udp, buf, size, 0) == (ssize_t)size) {
        atomic_fetch_add_explicit(&c->sent, 1u, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&c->send_errors, 1u, memory_order_relaxed);
    }
}

/* Sleeps until `deadline` while handling wake-ups and control data. Returns
 * early on a wake-up. Returns -1 when the session has ended. */
static int wait_until(vc_client_t *c, uint64_t deadline)
{
    uint64_t now = now_us();
    uint64_t left;

    if (deadline <= now) {
        return wait_events(c, 0, POLLIN);
    }
    left = deadline - now;
    if (left > PRECISE_SLEEP_US) {
        /* Wake a little early; the next call sleeps the rest precisely. */
        return wait_events(c, (int)((left - PRECISE_SLEEP_US / 2u) / 1000u), POLLIN);
    }
    if (wait_events(c, 0, POLLIN) != 0) {
        return -1;
    }
    sleep_us(left);
    return 0;
}

/* The steady state: input out, keepalive in. Always ends via set_end(). */
static void send_loop(vc_client_t *c)
{
    uint64_t now = now_us();
    uint64_t next_due = now;            /* periodic re-send */
    uint64_t last_sent = 0;
    uint64_t next_ping = now + PING_INTERVAL_US;
    int      sent_any = 0;

    for (;;) {
        uint64_t deadline;
        int      fresh;

        if (atomic_load(&c->stop)) {
            vc_proto_msg_t bye;

            memset(&bye, 0, sizeof bye);
            bye.type = VC_MSG_BYE;
            if (send_control(c, &bye) == 0) {
                set_end(c, VC_CLIENT_END_STOPPED, 0, "stopped");
            }
            return;
        }

        now = now_us();
        fresh = !c->rate_only && has_fresh_state(c);
        if (now >= next_due ||
            (fresh && (!sent_any || now - last_sent >= c->min_interval_us))) {
            int periodic = now >= next_due;

            send_input(c, now);
            if (c->rate_only && periodic) {
                /* Fixed cadence, without drift; skip ahead if we fell behind. */
                next_due += c->interval_us;
                if (next_due <= now) {
                    next_due = now + c->interval_us;
                }
            } else {
                /* The re-send period restarts from every send. */
                next_due = now + c->interval_us;
            }
            last_sent = now;
            sent_any = 1;
            fresh = 0;
        }

        if (now >= next_ping) {
            vc_proto_msg_t ping;

            memset(&ping, 0, sizeof ping);
            ping.type = VC_MSG_PING;
            ping.echo = ++c->ping_echo;
            c->ping_sent_us = now;
            if (send_control(c, &ping) != 0) {
                return;
            }
            next_ping = now + PING_INTERVAL_US;
        }

        deadline = next_due < next_ping ? next_due : next_ping;
        if (fresh && last_sent + c->min_interval_us < deadline) {
            deadline = last_sent + c->min_interval_us;   /* a change waiting for its slot */
        }
        if (wait_until(c, deadline) != 0) {
            return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

vc_client_t *vc_client_create(const vc_client_config_t *config)
{
    vc_client_t *c;
    unsigned     rate;

    if (config == NULL || config->host == NULL) {
        return NULL;
    }
    c = (vc_client_t *)calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    c->wake[0] = c->wake[1] = -1;
    c->tcp = c->udp = -1;

    c->addr.sin_family = AF_INET;
    c->addr.sin_port = htons(config->port != 0 ? config->port : VC_DEFAULT_PORT);
    if (inet_pton(AF_INET, config->host, &c->addr.sin_addr) != 1) {
        free(c);
        return NULL;
    }
    snprintf(c->host_text, sizeof c->host_text, "%s", config->host);
    snprintf(c->name, sizeof c->name, "%s", config->name != NULL ? config->name : "");

    c->controller_type = config->controller_type;
    c->caps = config->caps;
    rate = config->rate_hz != 0 ? config->rate_hz : DEFAULT_RATE_HZ;
    if (rate > 1000) {
        rate = 1000;
    }
    c->interval_us = 1000000u / rate;
    c->min_interval_us = config->min_interval_us != 0 ? config->min_interval_us
                                                      : DEFAULT_MIN_INTERVAL_US;
    c->rate_only = config->rate_only != 0;
    c->connect_timeout_ms = (int)(config->connect_timeout_ms != 0 ? config->connect_timeout_ms
                                                                  : DEFAULT_CONNECT_TIMEOUT_MS);
    c->on_event = config->on_event;
    c->user = config->user;

    c->front = 0;
    c->back = 2;
    atomic_init(&c->middle, 1u);
    atomic_init(&c->stop, 0);
    atomic_init(&c->sent, 0u);
    atomic_init(&c->send_errors, 0u);
    atomic_init(&c->rtt_us, 0u);

    if (pipe(c->wake) != 0 || set_nonblocking_cloexec(c->wake[0]) != 0 ||
        set_nonblocking_cloexec(c->wake[1]) != 0) {
        vc_client_destroy(c);
        return NULL;
    }
    return c;
}

vc_client_end_t vc_client_run(vc_client_t *c)
{
    vc_client_event_t event;

    if (c == NULL) {
        return VC_CLIENT_END_SYSTEM;
    }
    tune_thread();

    /* Per-session state. */
    c->tcp = c->udp = -1;
    c->connected = 0;
    c->token = 0;
    c->granted = 0;
    c->allowed_flags = 0;
    c->tx_sequence = 0;
    c->input_sequence = 0;
    c->ping_echo = 0;
    c->ping_sent_us = 0;
    c->epoch_us = now_us();
    c->error = 0;
    c->message[0] = '\0';
    vc_proto_reader_init(&c->reader);

    if (connect_tcp(c) == 0 && handshake(c) == 0 && open_udp(c) == 0) {
        memset(&event, 0, sizeof event);
        event.type = VC_CLIENT_EVENT_CONNECTED;
        event.slot = c->slot;
        event.caps = c->granted;
        event.timeout_ms = c->host_timeout_ms;
        event.message = "connected";
        emit(c, &event);
        send_loop(c);
    }

    close_fd(&c->udp);
    close_fd(&c->tcp);
    /* A stop() that arrived for this run is used up; the next run starts fresh. */
    atomic_store(&c->stop, 0);
    drain_wake(c);

    memset(&event, 0, sizeof event);
    event.type = VC_CLIENT_EVENT_CLOSED;
    event.end = c->end;
    event.error = c->error;
    event.message = c->message;
    emit(c, &event);
    return c->end;
}

void vc_client_stop(vc_client_t *c)
{
    if (c != NULL) {
        atomic_store(&c->stop, 1);
        poke(c);
    }
}

void vc_client_get_stats(const vc_client_t *c, vc_client_stats_t *out)
{
    if (c == NULL || out == NULL) {
        return;
    }
    /* The fields are atomics; the casts only drop const for C11's
     * non-const atomic_load parameter on older compilers. */
    out->sent = atomic_load_explicit((atomic_uint *)&c->sent, memory_order_relaxed);
    out->send_errors = atomic_load_explicit((atomic_uint *)&c->send_errors,
                                            memory_order_relaxed);
    out->rtt_us = atomic_load_explicit((atomic_uint *)&c->rtt_us, memory_order_relaxed);
}

void vc_client_destroy(vc_client_t *c)
{
    if (c == NULL) {
        return;
    }
    close_fd(&c->udp);
    close_fd(&c->tcp);
    close_fd(&c->wake[0]);
    close_fd(&c->wake[1]);
    free(c);
}

const char *vc_client_end_name(vc_client_end_t end)
{
    switch (end) {
    case VC_CLIENT_END_STOPPED:         return "stopped";
    case VC_CLIENT_END_CONNECT_FAILED:  return "could not connect";
    case VC_CLIENT_END_REFUSED:         return "refused by the host";
    case VC_CLIENT_END_HOST_ERROR:      return "ended by a host error";
    case VC_CLIENT_END_HOST_BYE:        return "host shut down";
    case VC_CLIENT_END_CONNECTION_LOST: return "connection lost";
    case VC_CLIENT_END_PROTOCOL:        return "protocol error";
    case VC_CLIENT_END_TIMEOUT:         return "timed out";
    case VC_CLIENT_END_SYSTEM:          return "system error";
    }
    return "unknown";
}
