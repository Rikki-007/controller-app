/*
 * vc_send.c - test client: plays the part of the phone app.
 *
 * Each simulated controller does what the app does: connects to vcd over
 * TCP, sends HELLO, gets a session token and slot in WELCOME, streams
 * scripted INPUT datagrams over UDP with that token, answers the host's
 * PINGs, and says BYE at the end:
 *
 *   build/tools/vc_send --type ps5 --script sweep --motion --touch
 *   build/tools/vc_send --clients 5 --bad          # slot limit, malformed and stale packets
 *   build/tools/vc_send --end close                # drop the connection without BYE
 *   build/tools/vc_send --duration 500 --linger 12000   # stall input, keep TCP: idle timeout
 *   build/tools/vc_send --no-pong --script idle --duration 6000   # keepalive timeout
 *   build/tools/vc_send --no-handshake --token deadbeef   # input nobody owns
 *
 * Every controller has its own TCP connection and UDP socket, so its own
 * source ports, exactly as separate phones would.
 */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* inet_pton, WSAPoll */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <mmsystem.h>           /* timeBeginPeriod */
typedef SOCKET send_socket_t;
#  define SEND_INVALID_SOCKET INVALID_SOCKET
#  define SEND_POLL_READABLE  POLLRDNORM
#  define SEND_POLL_WRITABLE  POLLWRNORM
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
#  include <time.h>
#  include <unistd.h>
typedef int send_socket_t;
#  define SEND_INVALID_SOCKET (-1)
#  define SEND_POLL_READABLE  POLLIN
#  define SEND_POLL_WRITABLE  POLLOUT
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller.h"
#include "encoder.h"
#include "protocol.h"

#ifdef MSG_NOSIGNAL
#  define SEND_FLAGS MSG_NOSIGNAL
#else
#  define SEND_FLAGS 0
#endif

#define MAX_CLIENTS          16
#define DEFAULT_HOST         "127.0.0.1"
#define DEFAULT_NAME         "vc_send"
#define DEFAULT_TOKEN        0x1234ABCDu
#define DEFAULT_RATE_HZ      120
#define DEFAULT_DURATION_MS  3000
/* How long to wait for the TCP connection and for the answer to HELLO. */
#define HANDSHAKE_TIMEOUT_MS 3000
/* The buttons script moves to the next button this often. */
#define BUTTON_STEP_US       250000u
/* Period of the stick, trigger and motion waves in the sweep script. */
#define SWEEP_PERIOD_US      2000000u
/* Number of VC_BTN_* bits a client may set (VC_BTN_VALID_MASK). */
#define BUTTON_COUNT         17

typedef enum {
    SCRIPT_BUTTONS,   /* one button at a time, in bit order */
    SCRIPT_SWEEP,     /* sticks trace a diamond, triggers ramp */
    SCRIPT_HOLD,      /* South held, left stick right, right trigger full */
    SCRIPT_IDLE       /* neutral state, as a phone with nothing touched */
} script_t;

typedef enum {
    END_BYE,          /* send BYE, then close */
    END_CLOSE         /* close the connection without a word */
} end_t;

typedef struct {
    const char *host;
    uint16_t    port;
    uint8_t     controller_type;
    const char *name;
    int         clients;
    script_t    script;
    unsigned    rate_hz;
    unsigned    duration_ms;   /* 0: until Ctrl+C */
    unsigned    linger_ms;     /* keep TCP open after input stops */
    end_t       end;
    uint8_t     flags;         /* VC_INPUT_HAS_* */
    int         bad;
    int         no_pong;
    int         no_handshake;
    uint32_t    token;         /* only with no_handshake */
    int         quiet;
} options_t;

typedef struct {
    send_socket_t     tcp;
    send_socket_t     udp;
    vc_proto_reader_t reader;
    char              name[VC_CLIENT_NAME_LEN + 1];
    int               live;          /* has a session and may send input */
    uint32_t          token;
    int               slot;          /* -1 without a session */
    uint8_t           caps;
    uint16_t          sequence;      /* next INPUT sequence */
    uint16_t          tx_sequence;   /* next control message sequence */
    unsigned long     sent;
    unsigned long     send_errors;
    unsigned long     pongs;
    char              outcome[96];   /* how it ended, for the summary */
} client_t;

/* ------------------------------------------------------------------------ */
/* Platform helpers                                                         */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER count;
    LARGE_INTEGER freq;
    uint64_t      c;
    uint64_t      f;

    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);
    c = (uint64_t)count.QuadPart;
    f = (uint64_t)freq.QuadPart;
    /* Split so a long uptime cannot overflow c * 1000000. */
    return (c / f) * 1000000u + (c % f) * 1000000u / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

static void sleep_us(uint64_t us)
{
#ifdef _WIN32
    /* Millisecond resolution; timeBeginPeriod(1) in main() makes it real. */
    Sleep((DWORD)((us + 999u) / 1000u));
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

static int net_startup(void)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }
#endif
    return 0;
}

static void net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

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
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS;
#endif
}

static void sock_close(send_socket_t sock)
{
    if (sock == SEND_INVALID_SOCKET) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

static int sock_set_nonblocking(send_socket_t sock)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(sock, (long)FIONBIO, &enabled) == 0 ? 0 : -1;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* Waits up to timeout_ms for `events` on one socket. Returns >0 if ready. */
static int sock_wait(send_socket_t sock, short events, int timeout_ms)
{
    struct pollfd pfd;

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = sock;
    pfd.events = events;
#ifdef _WIN32
    return WSAPoll(&pfd, 1, timeout_ms);
#else
    return poll(&pfd, 1, timeout_ms);
#endif
}

/* ------------------------------------------------------------------------ */
/* Control channel                                                          */
/* ------------------------------------------------------------------------ */

static int send_control(client_t *client, vc_proto_msg_t *msg)
{
    uint8_t buf[VC_PROTO_MAX_FRAME];
    size_t  size;
    long    sent;

    msg->sequence = client->tx_sequence++;
    size = vc_proto_encode(msg, buf, sizeof buf);
    if (size == 0) {
        return -1;
    }
    /* Frames are tiny and the socket buffer is nearly empty, so a short
     * write means the connection is broken. */
    sent = (long)send(client->tcp, (const char *)buf, (int)size, SEND_FLAGS);
    return sent == (long)size ? 0 : -1;
}

static const char *error_name(uint8_t code)
{
    switch (code) {
    case VC_ERROR_BAD_VERSION: return "bad version";
    case VC_ERROR_NO_SLOT:     return "no free slot";
    case VC_ERROR_UNSUPPORTED: return "controller type not supported";
    case VC_ERROR_PROTOCOL:    return "protocol error";
    case VC_ERROR_TIMEOUT:     return "timeout";
    case VC_ERROR_DRIVER:      return "driver error";
    default:                   return "unknown error";
    }
}

/* Ends the client's session on our side, recording why. */
static void finish(client_t *client, const char *outcome)
{
    if (client->live || client->outcome[0] == '\0') {
        snprintf(client->outcome, sizeof client->outcome, "%s", outcome);
    }
    client->live = 0;
    sock_close(client->tcp);
    client->tcp = SEND_INVALID_SOCKET;
}

static void handle_message(client_t *client, const vc_proto_msg_t *msg,
                           const options_t *opts)
{
    char text[96];

    switch (msg->type) {
    case VC_MSG_PING:
        if (!opts->no_pong) {
            vc_proto_msg_t pong;

            memset(&pong, 0, sizeof pong);
            pong.type = VC_MSG_PONG;
            pong.echo = msg->echo;
            if (send_control(client, &pong) == 0) {
                client->pongs++;
            }
        }
        break;
    case VC_MSG_ERROR:
        snprintf(text, sizeof text, "host sent ERROR %u (%s)", (unsigned)msg->error,
                 error_name(msg->error));
        finish(client, text);
        break;
    case VC_MSG_BYE:
        finish(client, "host sent BYE");
        break;
    default:   /* PONG, FEEDBACK: nothing to do yet */
        break;
    }
}

/* Reads whatever the host sent without waiting. */
static void service_control(client_t *client, const options_t *opts)
{
    uint8_t           buf[256];
    const uint8_t    *data;
    size_t            size;
    vc_proto_msg_t    msg;
    vc_proto_result_t result;
    long              received;

    while (client->tcp != SEND_INVALID_SOCKET) {
        received = (long)recv(client->tcp, (char *)buf, sizeof buf, 0);
        if (received < 0 && sock_would_block(sock_errno())) {
            return;
        }
        if (received <= 0) {
            finish(client, "connection closed by host");
            return;
        }
        data = buf;
        size = (size_t)received;
        while ((result = vc_proto_read(&client->reader, &data, &size, &msg)) == VC_PROTO_OK) {
            handle_message(client, &msg, opts);
            if (client->tcp == SEND_INVALID_SOCKET) {
                return;
            }
        }
        if (result != VC_PROTO_NEED_MORE) {
            finish(client, vc_proto_strerror(result));
            return;
        }
    }
}

/* Connects, sends HELLO and waits for WELCOME. Returns 0 with a session,
 * 1 if the host refused (ERROR), -1 on a network failure. */
static int handshake(client_t *client, const options_t *opts, const struct sockaddr_in *to)
{
    vc_proto_msg_t    msg;
    vc_proto_result_t result;
    uint8_t           buf[256];
    const uint8_t    *data;
    size_t            size;
    long              received;
    int               err = 0;
    socklen_t         err_len = sizeof err;
    uint64_t          deadline = now_us() + (uint64_t)HANDSHAKE_TIMEOUT_MS * 1000u;
    char              text[96];

    client->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->tcp == SEND_INVALID_SOCKET || sock_set_nonblocking(client->tcp) != 0) {
        finish(client, "could not create a TCP socket");
        return -1;
    }
    {
#ifdef _WIN32
        BOOL enabled = TRUE;
        setsockopt(client->tcp, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled, sizeof enabled);
#else
        int enabled = 1;
        setsockopt(client->tcp, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
#  ifdef SO_NOSIGPIPE
        setsockopt(client->tcp, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#  endif
#endif
    }

    /* Non-blocking connect, so an unreachable host fails in seconds. */
    if (connect(client->tcp, (const struct sockaddr *)to, sizeof *to) != 0 &&
        !sock_would_block(sock_errno())) {
        snprintf(text, sizeof text, "could not connect (error %d); is vcd running?", sock_errno());
        finish(client, text);
        return -1;
    }
    if (sock_wait(client->tcp, SEND_POLL_WRITABLE, HANDSHAKE_TIMEOUT_MS) <= 0 ||
        getsockopt(client->tcp, SOL_SOCKET, SO_ERROR, (char *)&err, &err_len) != 0 ||
        err != 0) {
        snprintf(text, sizeof text, "could not connect (error %d); is vcd running?", err);
        finish(client, text);
        return -1;
    }

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_HELLO;
    msg.controller_type = opts->controller_type;
    msg.caps = (uint8_t)(((opts->flags & VC_INPUT_HAS_MOTION) ? VC_CAP_MOTION : 0u) |
                         ((opts->flags & VC_INPUT_HAS_TOUCH) ? VC_CAP_TOUCH : 0u));
    memcpy(msg.name, client->name, sizeof msg.name);
    if (send_control(client, &msg) != 0) {
        finish(client, "could not send HELLO");
        return -1;
    }

    vc_proto_reader_init(&client->reader);
    for (;;) {
        uint64_t now = now_us();
        int      wait_ms;

        if (now >= deadline) {
            finish(client, "no answer to HELLO");
            return -1;
        }
        wait_ms = (int)((deadline - now) / 1000u) + 1;
        if (sock_wait(client->tcp, SEND_POLL_READABLE, wait_ms) <= 0) {
            continue;
        }
        received = (long)recv(client->tcp, (char *)buf, sizeof buf, 0);
        if (received < 0 && sock_would_block(sock_errno())) {
            continue;
        }
        if (received <= 0) {
            finish(client, "connection closed during the handshake");
            return -1;
        }
        data = buf;
        size = (size_t)received;
        result = vc_proto_read(&client->reader, &data, &size, &msg);
        if (result == VC_PROTO_NEED_MORE) {
            continue;
        }
        if (result != VC_PROTO_OK) {
            finish(client, vc_proto_strerror(result));
            return -1;
        }
        if (msg.type == VC_MSG_ERROR) {
            snprintf(text, sizeof text, "refused: ERROR %u (%s)", (unsigned)msg.error,
                     error_name(msg.error));
            finish(client, text);
            return 1;
        }
        if (msg.type != VC_MSG_WELCOME) {
            snprintf(text, sizeof text, "expected WELCOME, got %s", vc_proto_type_name(msg.type));
            finish(client, text);
            return -1;
        }
        client->token = msg.token;
        client->slot = msg.slot;
        client->caps = msg.caps;
        client->live = 1;
        /* Anything after WELCOME in the same read (unlikely) is handled by
         * the regular servicing; the reader keeps partial frames. */
        while (size > 0 && client->tcp != SEND_INVALID_SOCKET &&
               vc_proto_read(&client->reader, &data, &size, &msg) == VC_PROTO_OK) {
            handle_message(client, &msg, opts);
        }
        return 0;
    }
}

static void end_session(client_t *client, const options_t *opts)
{
    if (client->tcp == SEND_INVALID_SOCKET) {
        return;
    }
    if (opts->end == END_BYE) {
        vc_proto_msg_t bye;

        memset(&bye, 0, sizeof bye);
        bye.type = VC_MSG_BYE;
        send_control(client, &bye);
        finish(client, "sent BYE");
    } else {
        finish(client, "closed the connection without BYE");
    }
}

/* ------------------------------------------------------------------------ */
/* Scripts                                                                  */
/* ------------------------------------------------------------------------ */

/* Triangle wave over one period: phase 0..65535 maps to -32767..32767..-32767. */
static int16_t triangle(uint32_t phase)
{
    uint32_t p = phase & 0xFFFFu;
    uint32_t half = p < 0x8000u ? p : 0xFFFFu - p;   /* 0..32767 */
    return (int16_t)((int32_t)half * 2 - 32767);
}

/* Position within the sweep period, as 0..65535, offset per client. */
static uint32_t sweep_phase(uint64_t t_us, int client)
{
    uint64_t t = t_us + (uint64_t)client * (SWEEP_PERIOD_US / 8u);
    return (uint32_t)((t % SWEEP_PERIOD_US) * 65536u / SWEEP_PERIOD_US);
}

/* The state client number `client` presents t_us after the start. */
static void make_state(const options_t *opts, int client, uint64_t t_us,
                       vc_input_state_t *out)
{
    uint32_t phase = sweep_phase(t_us, client);

    memset(out, 0, sizeof *out);
    out->timestamp_us = (uint32_t)t_us;

    switch (opts->script) {
    case SCRIPT_BUTTONS:
        out->buttons = UINT32_C(1) << ((t_us / BUTTON_STEP_US + (uint64_t)client) % BUTTON_COUNT);
        break;
    case SCRIPT_SWEEP:
        out->left.x = triangle(phase);
        out->left.y = triangle(phase + 0x4000u);   /* a quarter period behind */
        out->right.x = triangle(phase + 0x8000u);
        out->right.y = triangle(phase + 0xC000u);
        out->trigger_left = (uint8_t)(((int32_t)triangle(phase) + 32767) / 257);
        out->trigger_right = (uint8_t)(VC_TRIGGER_MAX - out->trigger_left);
        break;
    case SCRIPT_HOLD:
        out->buttons = VC_BTN_SOUTH;
        out->left.x = VC_STICK_MAX;
        out->trigger_right = VC_TRIGGER_MAX;
        break;
    case SCRIPT_IDLE:
        break;
    }

    out->flags = opts->flags;
    if (opts->flags & VC_INPUT_HAS_MOTION) {
        /* Slow yaw back and forth, the device lying flat (1 g on z). */
        out->motion.gyro[1] = (int16_t)(triangle(phase) / 8);
        out->motion.accel[2] = 4096;
    }
    if (opts->flags & VC_INPUT_HAS_TOUCH) {
        /* One finger dragging across the middle of the pad. */
        out->touch[0].active = 1;
        out->touch[0].id = (uint8_t)(client + 1);
        out->touch[0].x = (uint16_t)((int32_t)triangle(phase) + 32767);
        out->touch[0].y = 32768;
    }
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

static void send_raw(client_t *client, const struct sockaddr_in *to,
                     const uint8_t *data, size_t size)
{
    long sent = (long)sendto(client->udp, (const char *)data, (int)size, 0,
                             (const struct sockaddr *)to, sizeof *to);

    if (sent == (long)size) {
        client->sent++;
    } else {
        /* Log the 1st, 2nd, 4th... failure, not every packet. */
        client->send_errors++;
        if ((client->send_errors & (client->send_errors - 1)) == 0) {
            fprintf(stderr, "%s: sendto failed (error %d, %lu so far)\n", client->name,
                    sock_errno(), client->send_errors);
        }
    }
}

static void send_state(client_t *client, const struct sockaddr_in *to,
                       const vc_input_state_t *state)
{
    uint8_t buf[VC_INPUT_DATAGRAM_MAX];
    size_t  size = vc_encode_input(state, client->token, client->sequence, buf, sizeof buf);

    client->sequence++;
    send_raw(client, to, buf, size);
}

/*
 * --bad: one datagram for each way the daemon must refuse input, then a
 * valid packet followed by a duplicate and an older one. vcd should report
 * six malformed and one oversized datagram, and close this session with
 * "2 stale".
 */
static void send_bad(client_t *client, const struct sockaddr_in *to, const options_t *opts)
{
    uint8_t          good[VC_INPUT_DATAGRAM_MAX];
    uint8_t          buf[300];
    vc_input_state_t state;
    size_t           size;
    uint16_t         sequence = client->sequence;

    make_state(opts, 0, 0, &state);
    size = vc_encode_input(&state, client->token, sequence, good, sizeof good);

    send_raw(client, to, good, 3);                            /* too short */
    memcpy(buf, good, size);
    buf[0] ^= 0xFFu;                                          /* bad magic */
    send_raw(client, to, buf, size);
    memcpy(buf, good, size);
    buf[2] = VC_PROTOCOL_VERSION + 1;                         /* bad version */
    send_raw(client, to, buf, size);
    memcpy(buf, good, size);
    buf[3] = VC_MSG_PING;                                     /* not INPUT */
    send_raw(client, to, buf, size);
    send_raw(client, to, good, size - 1);                     /* length mismatch */
    memcpy(buf, good, size);
    buf[VC_PACKET_HEADER_SIZE + 23] = 1;                      /* reserved byte set */
    send_raw(client, to, buf, size);
    memset(buf, 0, sizeof buf);                               /* oversized */
    send_raw(client, to, buf, sizeof buf);

    send_raw(client, to, good, size);                         /* valid */
    send_raw(client, to, good, size);                         /* duplicate: stale */
    size = vc_encode_input(&state, client->token, (uint16_t)(sequence - 1), good, sizeof good);
    send_raw(client, to, good, size);                         /* older: stale */

    client->sequence = (uint16_t)(sequence + 1);
}

/* ------------------------------------------------------------------------ */
/* Command line                                                             */
/* ------------------------------------------------------------------------ */

static const char *const script_names[] = { "buttons", "sweep", "hold", "idle" };

static void usage(const char *program)
{
    printf("usage: %s [options]\n"
           "\n"
           "Plays one or more phones at vcd: TCP handshake, then scripted UDP input.\n"
           "\n"
           "  --host ADDR      IPv4 address of vcd (default %s)\n"
           "  --port PORT      TCP and UDP port of vcd (default %d)\n"
           "  --type TYPE      xbox, ps4, ps5, switch, or a raw number (default xbox)\n"
           "  --name NAME      client name sent in HELLO; controller i is NAME-i\n"
           "                   (default %s)\n"
           "  --clients N      controllers to simulate, 1..%d (default 1)\n"
           "  --script NAME    buttons, sweep, hold or idle (default buttons)\n"
           "  --rate HZ        packets per second per controller, 1..1000 (default %d)\n"
           "  --duration MS    how long to send input; 0 runs until Ctrl+C (default %d)\n"
           "  --linger MS      keep the connection open, answering PINGs, this long\n"
           "                   after input stops (default 0)\n"
           "  --end HOW        bye: send BYE at the end; close: just close (default bye)\n"
           "  --motion         include gyro and accelerometer data\n"
           "  --touch          include touchpad data\n"
           "  --bad            first send malformed, oversized, duplicate and stale\n"
           "                   packets from the first controller\n"
           "  --no-pong        do not answer the host's PINGs\n"
           "  --no-handshake   skip TCP and send input with --token (which vcd drops)\n"
           "  --token HEX      token for --no-handshake (default %08X)\n"
           "  --quiet          print nothing but errors\n"
           "  --help           show this text\n",
           program, DEFAULT_HOST, VC_DEFAULT_PORT, DEFAULT_NAME, MAX_CLIENTS,
           DEFAULT_RATE_HZ, DEFAULT_DURATION_MS, DEFAULT_TOKEN);
}

static int parse_number(const char *text, int base, unsigned long min,
                        unsigned long max, unsigned long *out)
{
    char         *end;
    unsigned long value;

    if (text[0] == '-') {
        return -1;
    }
    value = strtoul(text, &end, base);
    if (end == text || *end != '\0' || value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_type(const char *text, uint8_t *out)
{
    static const struct {
        const char *name;
        uint8_t     type;
    } types[] = {
        { "xbox", VC_CONTROLLER_XBOX },
        { "ps4", VC_CONTROLLER_PS4 },
        { "ps5", VC_CONTROLLER_PS5 },
        { "switch", VC_CONTROLLER_SWITCH }
    };
    unsigned long number;
    size_t        i;

    for (i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (strcmp(text, types[i].name) == 0) {
            *out = types[i].type;
            return 0;
        }
    }
    /* A raw number lets tests send types the host must refuse. */
    if (parse_number(text, 10, 0, 255, &number) == 0) {
        *out = (uint8_t)number;
        return 0;
    }
    return -1;
}

/* Returns 0 to run, 1 to exit successfully (--help), -1 on a usage error. */
static int parse_options(int argc, char **argv, options_t *opts)
{
    int i;

    memset(opts, 0, sizeof *opts);
    opts->host = DEFAULT_HOST;
    opts->port = VC_DEFAULT_PORT;
    opts->controller_type = VC_CONTROLLER_XBOX;
    opts->name = DEFAULT_NAME;
    opts->clients = 1;
    opts->script = SCRIPT_BUTTONS;
    opts->rate_hz = DEFAULT_RATE_HZ;
    opts->duration_ms = DEFAULT_DURATION_MS;
    opts->end = END_BYE;
    opts->token = DEFAULT_TOKEN;

    for (i = 1; i < argc; i++) {
        const char   *arg = argv[i];
        const char   *value = i + 1 < argc ? argv[i + 1] : NULL;
        unsigned long number;
        size_t        s;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(arg, "--motion") == 0) {
            opts->flags |= VC_INPUT_HAS_MOTION;
            continue;
        } else if (strcmp(arg, "--touch") == 0) {
            opts->flags |= VC_INPUT_HAS_TOUCH;
            continue;
        } else if (strcmp(arg, "--bad") == 0) {
            opts->bad = 1;
            continue;
        } else if (strcmp(arg, "--no-pong") == 0) {
            opts->no_pong = 1;
            continue;
        } else if (strcmp(arg, "--no-handshake") == 0) {
            opts->no_handshake = 1;
            continue;
        } else if (strcmp(arg, "--quiet") == 0) {
            opts->quiet = 1;
            continue;
        }

        if (value == NULL) {
            fprintf(stderr, "%s: missing value or unknown option '%s'\n", argv[0], arg);
            return -1;
        }
        if (strcmp(arg, "--host") == 0) {
            opts->host = value;
        } else if (strcmp(arg, "--port") == 0) {
            if (parse_number(value, 10, 1, 65535, &number) != 0) {
                fprintf(stderr, "%s: '%s' is not a port number\n", argv[0], value);
                return -1;
            }
            opts->port = (uint16_t)number;
        } else if (strcmp(arg, "--type") == 0) {
            if (parse_type(value, &opts->controller_type) != 0) {
                fprintf(stderr, "%s: unknown controller type '%s'\n", argv[0], value);
                return -1;
            }
        } else if (strcmp(arg, "--name") == 0) {
            opts->name = value;
        } else if (strcmp(arg, "--token") == 0) {
            if (parse_number(value, 16, 1, 0xFFFFFFFFul, &number) != 0) {
                fprintf(stderr, "%s: '%s' is not a non-zero 32-bit hex token\n", argv[0], value);
                return -1;
            }
            opts->token = (uint32_t)number;
        } else if (strcmp(arg, "--clients") == 0) {
            if (parse_number(value, 10, 1, MAX_CLIENTS, &number) != 0) {
                fprintf(stderr, "%s: clients must be 1..%d\n", argv[0], MAX_CLIENTS);
                return -1;
            }
            opts->clients = (int)number;
        } else if (strcmp(arg, "--rate") == 0) {
            if (parse_number(value, 10, 1, 1000, &number) != 0) {
                fprintf(stderr, "%s: rate must be 1..1000 Hz\n", argv[0]);
                return -1;
            }
            opts->rate_hz = (unsigned)number;
        } else if (strcmp(arg, "--duration") == 0 || strcmp(arg, "--linger") == 0) {
            if (parse_number(value, 10, 0, 86400000ul, &number) != 0) {
                fprintf(stderr, "%s: '%s' is not a duration in ms\n", argv[0], value);
                return -1;
            }
            if (arg[2] == 'd') {
                opts->duration_ms = (unsigned)number;
            } else {
                opts->linger_ms = (unsigned)number;
            }
        } else if (strcmp(arg, "--end") == 0) {
            if (strcmp(value, "bye") == 0) {
                opts->end = END_BYE;
            } else if (strcmp(value, "close") == 0) {
                opts->end = END_CLOSE;
            } else {
                fprintf(stderr, "%s: --end must be bye or close\n", argv[0]);
                return -1;
            }
        } else if (strcmp(arg, "--script") == 0) {
            for (s = 0; s < sizeof script_names / sizeof script_names[0]; s++) {
                if (strcmp(value, script_names[s]) == 0) {
                    break;
                }
            }
            if (s == sizeof script_names / sizeof script_names[0]) {
                fprintf(stderr, "%s: unknown script '%s'\n", argv[0], value);
                return -1;
            }
            opts->script = (script_t)s;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            return -1;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    options_t          opts;
    client_t           clients[MAX_CLIENTS];
    struct sockaddr_in to;
    vc_input_state_t   state;
    uint64_t           start;
    uint64_t           next;
    uint64_t           now;
    uint64_t           interval;
    uint64_t           elapsed;
    uint64_t           linger_end;
    unsigned long      total = 0;
    int                parsed;
    int                live = 0;
    int                failed = 0;
    int                i;

    parsed = parse_options(argc, argv, &opts);
    if (parsed != 0) {
        if (parsed < 0) {
            usage(argv[0]);
        }
        return parsed > 0 ? EXIT_SUCCESS : 2;
    }

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(opts.port);
    if (net_startup() != 0) {
        return EXIT_FAILURE;
    }
    if (inet_pton(AF_INET, opts.host, &to.sin_addr) != 1) {
        fprintf(stderr, "%s: '%s' is not a valid IPv4 address\n", argv[0], opts.host);
        net_cleanup();
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef _WIN32
    timeBeginPeriod(1);
#else
    signal(SIGPIPE, SIG_IGN);   /* a closed connection must fail a send, not kill us */
#endif

    /* Handshakes, one controller after another, as phones would arrive. */
    memset(clients, 0, sizeof clients);
    for (i = 0; i < opts.clients; i++) {
        client_t *client = &clients[i];

        client->tcp = SEND_INVALID_SOCKET;
        client->slot = -1;
        client->sequence = 1;
        snprintf(client->name, sizeof client->name, "%s-%d", opts.name, i);
        client->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client->udp == SEND_INVALID_SOCKET) {
            finish(client, "could not create a UDP socket");
            failed++;
            continue;
        }
        if (opts.no_handshake) {
            client->token = opts.token + (uint32_t)i;
            client->live = 1;
            continue;
        }
        if (handshake(client, &opts, &to) < 0) {
            failed++;
        }
        if (!opts.quiet) {
            if (client->live) {
                printf("%s: slot %d, token %08lX, caps %s%s\n", client->name, client->slot,
                       (unsigned long)client->token,
                       (client->caps & VC_CAP_MOTION) ? "motion " : "",
                       (client->caps & VC_CAP_TOUCH) ? "touch" : "");
            } else {
                printf("%s: %s\n", client->name, client->outcome);
            }
        }
    }
    for (i = 0; i < opts.clients; i++) {
        live += clients[i].live;
    }

    if (live > 0 && opts.bad) {
        for (i = 0; i < opts.clients; i++) {
            if (clients[i].live) {
                send_bad(&clients[i], &to, &opts);
                break;
            }
        }
    }

    if (live > 0 && !opts.quiet) {
        printf("sending '%s' to %s:%u from %d controller%s at %u Hz%s\n",
               script_names[opts.script], opts.host, (unsigned)opts.port, live,
               live == 1 ? "" : "s", opts.rate_hz,
               opts.duration_ms == 0 ? ", Ctrl+C to stop" : "");
        fflush(stdout);
    }

    /* Fixed schedule: each tick is due interval after the previous one, so
     * the rate does not drift with how long sending takes. */
    interval = 1000000u / opts.rate_hz;
    start = now_us();
    next = start;
    while (live > 0 && !g_stop) {
        now = now_us();
        elapsed = now - start;
        if (opts.duration_ms != 0 && elapsed >= (uint64_t)opts.duration_ms * 1000u) {
            break;
        }
        if (now < next) {
            sleep_us(next - now);
            continue;
        }
        live = 0;
        for (i = 0; i < opts.clients; i++) {
            client_t *client = &clients[i];
            if (client->tcp != SEND_INVALID_SOCKET) {
                service_control(client, &opts);
            }
            if (client->live) {
                make_state(&opts, i, elapsed, &state);
                send_state(client, &to, &state);
                live++;
            }
        }
        next += interval;
        if (next + interval < now) {
            next = now;   /* fell more than a tick behind: skip, do not burst */
        }
    }
    elapsed = now_us() - start;

    /* Input has stopped; keep the control connections alive if asked, as a
     * phone that is still connected but has stopped sending would. */
    linger_end = now_us() + (uint64_t)opts.linger_ms * 1000u;
    while (!g_stop && now_us() < linger_end) {
        int open = 0;
        for (i = 0; i < opts.clients; i++) {
            if (clients[i].tcp != SEND_INVALID_SOCKET) {
                service_control(&clients[i], &opts);
                open += clients[i].tcp != SEND_INVALID_SOCKET;
            }
        }
        if (open == 0) {
            break;
        }
        sleep_us(10000u);
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    for (i = 0; i < opts.clients; i++) {
        client_t *client = &clients[i];

        if (client->tcp != SEND_INVALID_SOCKET) {
            service_control(client, &opts);   /* pick up a last ERROR or BYE */
        }
        end_session(client, &opts);
        if (opts.no_handshake && client->outcome[0] == '\0') {
            snprintf(client->outcome, sizeof client->outcome, "no handshake");
        }
        sock_close(client->udp);
        total += client->sent;
        if (!opts.quiet) {
            printf("%s: sent %lu datagrams, answered %lu pings, %s\n", client->name,
                   client->sent, client->pongs, client->outcome);
        }
    }
    if (!opts.quiet) {
        printf("sent %lu datagrams in %lu ms\n", total, (unsigned long)(elapsed / 1000u));
    }
    net_cleanup();
    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
