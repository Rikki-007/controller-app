/*
 * session.c - control-channel sessions and controller slots.
 * See session.h for the contract.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "session.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

#ifdef __GNUC__
static void slog(const vc_sessions_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#endif

static void slog(const vc_sessions_t *s, const char *fmt, ...)
{
    char    message[256];
    va_list args;

    if (s->cfg.log == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    s->cfg.log(message, s->cfg.user);
}

/* True for the 1st, 2nd, 4th, 8th... occurrence. */
static int should_log(uint64_t occurrence)
{
    return (occurrence & (occurrence - 1)) == 0;
}

/* "a.b.c.d:port" in a caller buffer. */
static const char *peer_text(const vc_net_peer_t *peer, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u:%u", peer->ip[0], peer->ip[1], peer->ip[2],
             peer->ip[3], (unsigned)peer->port);
    return buf;
}

static const char *type_name(vc_controller_type_t type)
{
    switch (type) {
    case VC_CONTROLLER_XBOX:   return "Xbox";
    case VC_CONTROLLER_PS5:    return "PS5";
    case VC_CONTROLLER_SWITCH: return "Switch";
    case VC_CONTROLLER_PS4:    return "PS4";
    default:                   return "?";
    }
}

static const char *target_name(vc_hid_target_t target)
{
    switch (target) {
    case VC_HID_TARGET_X360:  return "Xbox 360 pad";
    case VC_HID_TARGET_DS4:   return "DualShock 4";
    case VC_HID_TARGET_EVDEV: return "evdev pad";
    default:                  return "?";
    }
}

/* Capabilities the controller physically has; matches the parser profiles.
 * Rumble and LED are never granted yet: there is no feedback path. */
static uint8_t type_caps(vc_controller_type_t type)
{
    switch (type) {
    case VC_CONTROLLER_PS5:
    case VC_CONTROLLER_PS4:    return VC_CAP_MOTION | VC_CAP_TOUCH;
    case VC_CONTROLLER_SWITCH: return VC_CAP_MOTION;
    default:                   return 0;
    }
}

static const char *caps_text(uint8_t caps)
{
    static const char *const names[] = {
        "none", "motion", "touch", "motion+touch"
    };
    return names[caps & (VC_CAP_MOTION | VC_CAP_TOUCH)];
}

/* Client names are only for logs: keep printable ASCII, so a name cannot
 * forge log lines or break the quotes around it. */
static void sanitise_name(const char *in, char *out)
{
    size_t i;

    for (i = 0; i < VC_CLIENT_NAME_LEN && in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? (char)c : '?';
    }
    out[i] = '\0';
    if (i == 0) {
        strcpy(out, "unnamed");
    }
}

/* xorshift32; only used when the caller gives no random32. */
static uint32_t fallback_random(vc_sessions_t *s)
{
    uint32_t x = s->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng_state = x;
    return x;
}

/* A non-zero token no active session holds. */
static uint32_t new_token(vc_sessions_t *s)
{
    for (;;) {
        uint32_t token = s->cfg.random32 != NULL ? s->cfg.random32(s->cfg.user)
                                                 : fallback_random(s);
        size_t   i;
        int      taken = 0;

        if (token == 0) {
            continue;
        }
        for (i = 0; i < ARRAY_LEN(s->slots); i++) {
            if (s->slots[i].in_use && s->slots[i].token == token) {
                taken = 1;
            }
        }
        if (!taken) {
            return token;
        }
    }
}

static int conn_valid(const vc_sessions_t *s, int conn)
{
    return conn >= 0 && conn < VC_NET_MAX_CONNS && s->conns[conn].open;
}

/* Encodes and sends one message. Returns 0 or -1. */
static int send_msg(vc_sessions_t *s, int conn, vc_proto_msg_t *msg)
{
    uint8_t buf[VC_PROTO_MAX_FRAME];
    size_t  size;

    msg->sequence = s->conns[conn].tx_sequence++;
    size = vc_proto_encode(msg, buf, sizeof buf);
    if (size == 0) {
        return -1;
    }
    return s->cfg.send(conn, buf, size, s->cfg.user);
}

static int send_simple(vc_sessions_t *s, int conn, uint8_t type, uint8_t error)
{
    vc_proto_msg_t msg;

    memset(&msg, 0, sizeof msg);
    msg.type = type;
    msg.error = error;
    return send_msg(s, conn, &msg);
}

static void release_pad(vc_session_slot_t *slot)
{
    vc_input_state_t neutral;

    memset(&neutral, 0, sizeof neutral);
    vc_hid_submit(slot->device, &neutral);
    slot->neutral = 1;
}

/* ------------------------------------------------------------------------ */
/* Closing                                                                  */
/* ------------------------------------------------------------------------ */

/* How a session or connection ends. */
typedef enum {
    END_SILENT,       /* nothing to send: the peer is gone or said BYE */
    END_ERROR,        /* send ERROR first */
    END_BYE           /* send BYE first (host shutdown) */
} end_t;

/* Ends a connection that never got a slot. */
static void close_conn(vc_sessions_t *s, int conn, end_t end, uint8_t error,
                       int connected, const char *reason)
{
    vc_session_conn_t *c = &s->conns[conn];
    char               peer[32];

    if (end == END_ERROR) {
        send_simple(s, conn, VC_MSG_ERROR, error);
    } else if (end == END_BYE) {
        send_simple(s, conn, VC_MSG_BYE, 0);
    }
    slog(s, "connection %d from %s closed before HELLO: %s", conn,
         peer_text(&c->peer, peer, sizeof peer), reason);
    if (connected) {
        s->cfg.disconnect(conn, s->cfg.user);
    }
    memset(c, 0, sizeof *c);
    c->slot = -1;
}

/* Ends a session: tells the client why (if it is still there), releases and
 * unplugs the pad, frees the slot and closes the connection. `connected` is
 * 0 when the transport already lost the connection. */
static void close_session(vc_sessions_t *s, int index, end_t end, uint8_t error,
                          int connected, const char *reason)
{
    vc_session_slot_t       *slot = &s->slots[index];
    const vc_parser_stats_t *stats = &slot->parser.stats;
    int                      conn = slot->conn;

    if (connected && end == END_ERROR) {
        send_simple(s, conn, VC_MSG_ERROR, error);
    } else if (connected && end == END_BYE) {
        send_simple(s, conn, VC_MSG_BYE, 0);
    }

    release_pad(slot);
    vc_hid_destroy(slot->device);

    slog(s, "slot %d closed (\"%s\"): %s; %lu inputs accepted, %lu stale, "
         "%lu from a wrong address, rtt %d ms",
         index, slot->name, reason, (unsigned long)stats->accepted,
         (unsigned long)stats->rejected_stale, (unsigned long)slot->wrong_address,
         slot->rtt_ms);

    if (connected) {
        s->cfg.disconnect(conn, s->cfg.user);
    }
    memset(&s->conns[conn], 0, sizeof s->conns[conn]);
    s->conns[conn].slot = -1;
    memset(slot, 0, sizeof *slot);
}

/* Ends whatever is on `conn`: its session if it has one, else the bare
 * connection. */
static void end_conn(vc_sessions_t *s, int conn, end_t end, uint8_t error,
                     int connected, const char *reason)
{
    if (s->conns[conn].slot >= 0) {
        close_session(s, s->conns[conn].slot, end, error, connected, reason);
    } else {
        close_conn(s, conn, end, error, connected, reason);
    }
}

/* ------------------------------------------------------------------------ */
/* Control messages                                                         */
/* ------------------------------------------------------------------------ */

static void refuse(vc_sessions_t *s, int conn, uint8_t error, const char *name,
                   const char *why)
{
    char peer[32];

    s->refused++;
    slog(s, "refused \"%s\" from %s: %s", name,
         peer_text(&s->conns[conn].peer, peer, sizeof peer), why);
    send_simple(s, conn, VC_MSG_ERROR, error);
    s->cfg.disconnect(conn, s->cfg.user);
    memset(&s->conns[conn], 0, sizeof s->conns[conn]);
    s->conns[conn].slot = -1;
}

static void on_hello(vc_sessions_t *s, int conn, const vc_proto_msg_t *hello,
                     uint64_t now_ms)
{
    vc_session_conn_t   *c = &s->conns[conn];
    vc_controller_type_t type = (vc_controller_type_t)hello->controller_type;
    vc_session_slot_t   *slot = NULL;
    vc_proto_msg_t       welcome;
    char                 name[VC_CLIENT_NAME_LEN + 1];
    char                 why[96];
    char                 peer[32];
    int                  index;

    sanitise_name(hello->name, name);

    if (vc_hid_target_for(type) == VC_HID_TARGET_NONE) {
        snprintf(why, sizeof why, "controller type %u is not supported by the %s backend",
                 (unsigned)hello->controller_type, vc_hid_backend_name());
        refuse(s, conn, VC_ERROR_UNSUPPORTED, name, why);
        return;
    }
    for (index = 0; index < VC_MAX_SLOTS; index++) {
        if (!s->slots[index].in_use) {
            slot = &s->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        snprintf(why, sizeof why, "no free slot (all %d in use)", VC_MAX_SLOTS);
        refuse(s, conn, VC_ERROR_NO_SLOT, name, why);
        return;
    }

    memset(slot, 0, sizeof *slot);
    slot->token = new_token(s);
    if (vc_parser_init(&slot->parser, type, slot->token) != 0 ||
        vc_hid_create(type, &slot->device) != 0) {
        memset(slot, 0, sizeof *slot);
        refuse(s, conn, VC_ERROR_DRIVER, name, "the virtual gamepad could not be created");
        return;
    }
    slot->in_use = 1;
    slot->conn = conn;
    slot->type = type;
    slot->caps = (uint8_t)(hello->caps & type_caps(type));
    memcpy(slot->name, name, sizeof slot->name);
    slot->last_input_ms = now_ms;
    slot->neutral = 1;            /* a new pad starts released */
    slot->last_ping_ms = now_ms;
    slot->rtt_ms = -1;
    c->slot = index;

    memset(&welcome, 0, sizeof welcome);
    welcome.type = VC_MSG_WELCOME;
    welcome.token = slot->token;
    welcome.slot = (uint8_t)index;
    welcome.caps = slot->caps;
    welcome.timeout_ms = (uint16_t)s->cfg.input_timeout_ms;

    slog(s, "slot %d opened for \"%s\" from %s: %s as %s, token %08lx, caps %s",
         index, name, peer_text(&c->peer, peer, sizeof peer), type_name(type),
         target_name(vc_hid_target_for(type)), (unsigned long)slot->token,
         caps_text(slot->caps));

    if (send_msg(s, conn, &welcome) != 0) {
        close_session(s, index, END_SILENT, 0, 1, "could not send WELCOME");
    }
}

static void on_pong(vc_sessions_t *s, int conn, const vc_proto_msg_t *pong,
                    uint64_t now_ms)
{
    vc_session_slot_t *slot = &s->slots[s->conns[conn].slot];

    /* Any PONG proves the client is alive; only the latest one times it. */
    if (pong->echo == slot->ping_echo && slot->pings_unanswered > 0) {
        slot->rtt_ms = (int)(now_ms - slot->last_ping_ms);
    }
    slot->pings_unanswered = 0;
}

/* Handles one message. Returns 0 to keep reading, -1 if the connection was
 * closed. */
static int on_message(vc_sessions_t *s, int conn, const vc_proto_msg_t *msg,
                      uint64_t now_ms)
{
    int  has_slot = s->conns[conn].slot >= 0;
    char why[64];

    switch (msg->type) {
    case VC_MSG_HELLO:
        if (has_slot) {
            end_conn(s, conn, END_ERROR, VC_ERROR_PROTOCOL, 1, "protocol error: second HELLO");
            return -1;
        }
        on_hello(s, conn, msg, now_ms);
        return s->conns[conn].open ? 0 : -1;

    case VC_MSG_PING: {
        vc_proto_msg_t pong;

        memset(&pong, 0, sizeof pong);
        pong.type = VC_MSG_PONG;
        pong.echo = msg->echo;
        if (send_msg(s, conn, &pong) != 0) {
            end_conn(s, conn, END_SILENT, 0, 1, "send failed");
            return -1;
        }
        return 0;
    }

    case VC_MSG_PONG:
        if (has_slot) {
            on_pong(s, conn, msg, now_ms);
        }
        return 0;

    case VC_MSG_BYE:
        end_conn(s, conn, END_SILENT, 0, 1, "client said bye");
        return -1;

    default:
        /* WELCOME, FEEDBACK and ERROR only travel host -> client. */
        snprintf(why, sizeof why, "protocol error: unexpected %s",
                 vc_proto_type_name(msg->type));
        end_conn(s, conn, END_ERROR, VC_ERROR_PROTOCOL, 1, why);
        return -1;
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int vc_sessions_init(vc_sessions_t *s, const vc_session_config_t *cfg)
{
    size_t i;

    if (s == NULL || cfg == NULL || cfg->send == NULL || cfg->disconnect == NULL) {
        return -1;
    }
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    if (s->cfg.input_timeout_ms == 0) {
        s->cfg.input_timeout_ms = VC_SESSION_INPUT_TIMEOUT_MS;
    }
    if (s->cfg.idle_timeout_ms == 0) {
        s->cfg.idle_timeout_ms = VC_SESSION_IDLE_TIMEOUT_MS;
    }
    if (s->cfg.hello_timeout_ms == 0) {
        s->cfg.hello_timeout_ms = VC_SESSION_HELLO_TIMEOUT_MS;
    }
    if (s->cfg.ping_interval_ms == 0) {
        s->cfg.ping_interval_ms = VC_SESSION_PING_INTERVAL_MS;
    }
    if (s->cfg.ping_misses == 0) {
        s->cfg.ping_misses = VC_SESSION_PING_MISSES;
    }
    /* WELCOME carries the input timeout in 16 bits. */
    if (s->cfg.input_timeout_ms > 0xFFFFu) {
        s->cfg.input_timeout_ms = 0xFFFFu;
    }
    s->rng_state = 0x9E3779B9u;
    for (i = 0; i < ARRAY_LEN(s->conns); i++) {
        s->conns[i].slot = -1;
    }
    return 0;
}

void vc_sessions_connected(vc_sessions_t *s, int conn, const vc_net_peer_t *peer,
                           uint64_t now_ms)
{
    vc_session_conn_t *c;
    char               text[32];

    if (s == NULL || peer == NULL || conn < 0 || conn >= VC_NET_MAX_CONNS) {
        return;
    }
    c = &s->conns[conn];
    if (c->open) {
        /* The transport reused an index we still thought open: the old
         * connection's disconnect was missed. Close what it left behind. */
        end_conn(s, conn, END_SILENT, 0, 0, "connection lost");
    }
    memset(c, 0, sizeof *c);
    c->open = 1;
    c->peer = *peer;
    c->connected_ms = now_ms;
    c->slot = -1;
    vc_proto_reader_init(&c->reader);
    slog(s, "connection %d from %s", conn, peer_text(peer, text, sizeof text));
}

void vc_sessions_data(vc_sessions_t *s, int conn, const uint8_t *data, size_t size,
                      uint64_t now_ms)
{
    vc_proto_msg_t    msg;
    vc_proto_result_t result;
    char              why[96];

    if (s == NULL || !conn_valid(s, conn)) {
        return;
    }
    for (;;) {
        result = vc_proto_read(&s->conns[conn].reader, &data, &size, &msg);
        if (result != VC_PROTO_OK) {
            break;
        }
        if (on_message(s, conn, &msg, now_ms) != 0) {
            return;   /* closed; ignore whatever else it sent */
        }
    }
    if (result == VC_PROTO_NEED_MORE) {
        return;
    }
    snprintf(why, sizeof why, "protocol error: %s", vc_proto_strerror(result));
    end_conn(s, conn, END_ERROR,
             result == VC_PROTO_BAD_VERSION ? VC_ERROR_BAD_VERSION : VC_ERROR_PROTOCOL,
             1, why);
}

void vc_sessions_disconnected(vc_sessions_t *s, int conn, uint64_t now_ms)
{
    (void)now_ms;
    if (s == NULL || !conn_valid(s, conn)) {
        return;
    }
    end_conn(s, conn, END_SILENT, 0, 0, "connection lost");
}

void vc_sessions_input(vc_sessions_t *s, const vc_net_input_t *input, uint64_t now_ms)
{
    vc_session_slot_t *slot = NULL;
    vc_input_state_t   state;
    char               text[32];
    int                index;

    if (s == NULL || input == NULL) {
        return;
    }
    for (index = 0; index < VC_MAX_SLOTS; index++) {
        if (s->slots[index].in_use && s->slots[index].token == input->packet.token) {
            slot = &s->slots[index];
            break;
        }
    }
    if (slot == NULL) {
        s->unknown_token++;
        if (should_log(s->unknown_token)) {
            slog(s, "dropped input with unknown token %08lx from %s (%lu so far)",
                 (unsigned long)input->packet.token,
                 peer_text(&input->peer, text, sizeof text),
                 (unsigned long)s->unknown_token);
        }
        return;
    }

    /* The token is not secret from the LAN, so also insist on the address
     * that did the handshake. The port may differ (and change, behind NAT). */
    if (memcmp(input->peer.ip, s->conns[slot->conn].peer.ip, sizeof input->peer.ip) != 0) {
        slot->wrong_address++;
        if (should_log(slot->wrong_address)) {
            slog(s, "slot %d (\"%s\"): dropped input from %s, which is not the "
                 "client's address (%lu so far)", index, slot->name,
                 peer_text(&input->peer, text, sizeof text),
                 (unsigned long)slot->wrong_address);
        }
        return;
    }

    if (vc_parser_accept(&slot->parser, &input->packet, &state) != VC_PARSE_OK) {
        return;   /* stale or duplicate; counted by the parser */
    }
    if (!slot->has_udp_peer || slot->udp_peer.port != input->peer.port) {
        slog(s, "slot %d (\"%s\"): %s from %s", index, slot->name,
             slot->has_udp_peer ? "input moved to" : "input",
             peer_text(&input->peer, text, sizeof text));
        slot->has_udp_peer = 1;
        slot->udp_peer = input->peer;
    }
    if (vc_hid_submit(slot->device, &state) != 0) {
        close_session(s, index, END_ERROR, VC_ERROR_DRIVER, 1, "driver error");
        return;
    }
    slot->last_input_ms = now_ms;
    slot->neutral = 0;
    if (s->cfg.on_state != NULL) {
        s->cfg.on_state(index, &state, s->cfg.user);
    }
}

void vc_sessions_tick(vc_sessions_t *s, uint64_t now_ms)
{
    char why[64];
    int  i;

    if (s == NULL) {
        return;
    }

    for (i = 0; i < VC_NET_MAX_CONNS; i++) {
        const vc_session_conn_t *c = &s->conns[i];
        if (c->open && c->slot < 0 && now_ms - c->connected_ms >= s->cfg.hello_timeout_ms) {
            snprintf(why, sizeof why, "no HELLO within %u ms", s->cfg.hello_timeout_ms);
            close_conn(s, i, END_ERROR, VC_ERROR_TIMEOUT, 1, why);
        }
    }

    for (i = 0; i < VC_MAX_SLOTS; i++) {
        vc_session_slot_t *slot = &s->slots[i];
        uint64_t           quiet;

        if (!slot->in_use) {
            continue;
        }

        quiet = now_ms - slot->last_input_ms;
        if (quiet >= s->cfg.idle_timeout_ms) {
            snprintf(why, sizeof why, "no input for %u ms", s->cfg.idle_timeout_ms);
            close_session(s, i, END_ERROR, VC_ERROR_TIMEOUT, 1, why);
            continue;
        }
        if (!slot->neutral && quiet >= s->cfg.input_timeout_ms) {
            slog(s, "slot %d (\"%s\"): no input for %u ms, releasing all controls",
                 i, slot->name, s->cfg.input_timeout_ms);
            release_pad(slot);
            /* The client may restart its sequence numbers when it resumes. */
            vc_parser_reset(&slot->parser);
        }

        if (now_ms - slot->last_ping_ms >= s->cfg.ping_interval_ms) {
            vc_proto_msg_t ping;

            if (slot->pings_unanswered >= s->cfg.ping_misses) {
                snprintf(why, sizeof why, "keepalive timeout (%u pings unanswered)",
                         slot->pings_unanswered);
                close_session(s, i, END_ERROR, VC_ERROR_TIMEOUT, 1, why);
                continue;
            }
            memset(&ping, 0, sizeof ping);
            ping.type = VC_MSG_PING;
            ping.echo = ++s->ping_counter;
            slot->ping_echo = ping.echo;
            slot->last_ping_ms = now_ms;
            slot->pings_unanswered++;
            if (send_msg(s, slot->conn, &ping) != 0) {
                close_session(s, i, END_SILENT, 0, 1, "send failed");
            }
        }
    }
}

void vc_sessions_shutdown(vc_sessions_t *s)
{
    int i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < VC_MAX_SLOTS; i++) {
        if (s->slots[i].in_use) {
            close_session(s, i, END_BYE, 0, 1, "shutdown");
        }
    }
    for (i = 0; i < VC_NET_MAX_CONNS; i++) {
        if (s->conns[i].open) {
            close_conn(s, i, END_BYE, 0, 1, "shutdown");
        }
    }
}

int vc_sessions_active(const vc_sessions_t *s)
{
    int count = 0;
    int i;

    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < VC_MAX_SLOTS; i++) {
        count += s->slots[i].in_use != 0;
    }
    return count;
}
