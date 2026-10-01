/*
 * parser.c - turns raw client bytes into a vc_input_state_t.
 * See parser.h for the contract.
 */
#include <string.h>

#include "parser.h"

/* ------------------------------------------------------------------------ */
/* Stateless decoding                                                       */
/* ------------------------------------------------------------------------ */

/* Offsets within the packet header; they mirror vc_packet_header_t. */
#define HDR_OFF_MAGIC     0
#define HDR_OFF_VERSION   2
#define HDR_OFF_TYPE      3
#define HDR_OFF_SEQUENCE  4
#define HDR_OFF_LENGTH    6

/* Offsets within the INPUT payload; they mirror vc_packet_input_t. */
#define IN_OFF_TOKEN      0
#define IN_OFF_TIMESTAMP  4
#define IN_OFF_BUTTONS    8
#define IN_OFF_LEFT_X     12
#define IN_OFF_LEFT_Y     14
#define IN_OFF_RIGHT_X    16
#define IN_OFF_RIGHT_Y    18
#define IN_OFF_TRIGGER_L  20
#define IN_OFF_TRIGGER_R  21
#define IN_OFF_FLAGS      22
#define IN_OFF_RESERVED   23

/* Size of one vc_packet_touch_point_t on the wire. */
#define TOUCH_POINT_SIZE  (VC_PACKET_TOUCH_SIZE / VC_TOUCH_POINTS)

/* Fields are little-endian on the wire and read byte by byte, so the host's
 * byte order and alignment rules do not matter. */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t read_i16(const uint8_t *p)
{
    uint16_t raw = read_u16(p);
    return (int16_t)(raw >= 0x8000u ? (int32_t)raw - 0x10000 : (int32_t)raw);
}

vc_parser_result_t vc_parser_decode(const uint8_t *data, size_t size,
                                    vc_parser_packet_t *out)
{
    const uint8_t     *payload;
    const uint8_t     *block;
    size_t             length;
    size_t             expected;
    uint32_t           buttons;
    uint8_t            flags;
    vc_parser_packet_t packet;
    int                i;

    if (data == NULL || out == NULL || size < VC_PACKET_HEADER_SIZE) {
        return VC_PARSE_TOO_SHORT;
    }
    if (read_u16(data + HDR_OFF_MAGIC) != VC_PROTOCOL_MAGIC) {
        return VC_PARSE_BAD_MAGIC;
    }
    if (data[HDR_OFF_VERSION] != VC_PROTOCOL_VERSION) {
        return VC_PARSE_BAD_VERSION;
    }
    if (data[HDR_OFF_TYPE] != VC_MSG_INPUT) {
        return VC_PARSE_BAD_TYPE;
    }

    length = read_u16(data + HDR_OFF_LENGTH);
    if (length != size - VC_PACKET_HEADER_SIZE || length < VC_PACKET_INPUT_SIZE) {
        return VC_PARSE_BAD_LENGTH;
    }

    payload = data + VC_PACKET_HEADER_SIZE;
    flags = payload[IN_OFF_FLAGS];
    if ((flags & ~VC_INPUT_FLAGS_MASK) != 0) {
        return VC_PARSE_BAD_FIELD;
    }

    expected = VC_PACKET_INPUT_SIZE;
    if (flags & VC_INPUT_HAS_MOTION) {
        expected += VC_PACKET_MOTION_SIZE;
    }
    if (flags & VC_INPUT_HAS_TOUCH) {
        expected += VC_PACKET_TOUCH_SIZE;
    }
    if (length != expected) {
        return VC_PARSE_BAD_LENGTH;
    }

    buttons = read_u32(payload + IN_OFF_BUTTONS);
    if ((buttons & ~VC_BTN_VALID_MASK) != 0 || payload[IN_OFF_RESERVED] != 0) {
        return VC_PARSE_BAD_FIELD;
    }

    memset(&packet, 0, sizeof packet);
    packet.sequence = read_u16(data + HDR_OFF_SEQUENCE);
    packet.token = read_u32(payload + IN_OFF_TOKEN);
    packet.state.timestamp_us = read_u32(payload + IN_OFF_TIMESTAMP);
    packet.state.buttons = buttons;
    packet.state.left.x = read_i16(payload + IN_OFF_LEFT_X);
    packet.state.left.y = read_i16(payload + IN_OFF_LEFT_Y);
    packet.state.right.x = read_i16(payload + IN_OFF_RIGHT_X);
    packet.state.right.y = read_i16(payload + IN_OFF_RIGHT_Y);
    packet.state.trigger_left = payload[IN_OFF_TRIGGER_L];
    packet.state.trigger_right = payload[IN_OFF_TRIGGER_R];
    packet.state.flags = flags;

    block = payload + VC_PACKET_INPUT_SIZE;
    if (flags & VC_INPUT_HAS_MOTION) {
        for (i = 0; i < 3; i++) {
            packet.state.motion.gyro[i] = read_i16(block + 2 * i);
            packet.state.motion.accel[i] = read_i16(block + 6 + 2 * i);
        }
        block += VC_PACKET_MOTION_SIZE;
    }
    if (flags & VC_INPUT_HAS_TOUCH) {
        for (i = 0; i < VC_TOUCH_POINTS; i++) {
            const uint8_t *point = block + TOUCH_POINT_SIZE * i;
            if (point[1] != 0) {
                return VC_PARSE_BAD_FIELD;
            }
            packet.state.touch[i].active = (point[0] & VC_TOUCH_STATE_ACTIVE) != 0;
            packet.state.touch[i].id = point[0] & VC_TOUCH_STATE_ID_MASK;
            packet.state.touch[i].x = read_u16(point + 2);
            packet.state.touch[i].y = read_u16(point + 4);
        }
    }

    *out = packet;
    return VC_PARSE_OK;
}

const char *vc_parser_strerror(vc_parser_result_t result)
{
    switch (result) {
    case VC_PARSE_OK:          return "ok";
    case VC_PARSE_TOO_SHORT:   return "shorter than a packet header";
    case VC_PARSE_BAD_MAGIC:   return "bad magic";
    case VC_PARSE_BAD_VERSION: return "unsupported protocol version";
    case VC_PARSE_BAD_TYPE:    return "not an input packet";
    case VC_PARSE_BAD_LENGTH:  return "length does not match contents";
    case VC_PARSE_BAD_FIELD:   return "reserved or unknown bits set";
    case VC_PARSE_BAD_TOKEN:   return "wrong session token";
    case VC_PARSE_STALE:       return "duplicate or out-of-order sequence number";
    }
    return "unknown parse result";
}

/* ------------------------------------------------------------------------ */
/* Controller profiles                                                      */
/* ------------------------------------------------------------------------ */

#define BUTTON_COUNT 17   /* bits 0..16, see VC_BTN_VALID_MASK */

typedef struct {
    int         has_motion;
    int         has_touch;
    int         analog_triggers;
    /* Label per button bit; NULL means the controller has no such button. */
    const char *buttons[BUTTON_COUNT];
} profile_t;

/* Indexed by vc_controller_type_t. */
static const profile_t profiles[VC_CONTROLLER_TYPE_COUNT] = {
    /* VC_CONTROLLER_NONE */
    { 0, 0, 0, { NULL } },
    /* VC_CONTROLLER_XBOX */
    { 0, 0, 1,
      { "A", "B", "X", "Y", "LB", "RB", "LS", "RS", "Menu", "View", "Guide",
        "Share", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", NULL } },
    /* VC_CONTROLLER_PS5 */
    { 1, 1, 1,
      { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L3", "R3",
        "Options", "Create", "PS", "Mute", "D-pad Up", "D-pad Down",
        "D-pad Left", "D-pad Right", "Touchpad" } },
    /* VC_CONTROLLER_SWITCH */
    { 1, 0, 0,
      { "B", "A", "Y", "X", "L", "R", "LS", "RS", "Plus", "Minus", "Home",
        "Capture", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", NULL } },
    /* VC_CONTROLLER_PS4 */
    { 1, 1, 1,
      { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L3", "R3",
        "Options", "Share", "PS", NULL, "D-pad Up", "D-pad Down",
        "D-pad Left", "D-pad Right", "Touchpad" } }
};

static const profile_t *profile_for(vc_controller_type_t type)
{
    if ((int)type <= VC_CONTROLLER_NONE || (int)type >= VC_CONTROLLER_TYPE_COUNT) {
        return NULL;
    }
    return &profiles[type];
}

static uint32_t profile_button_mask(const profile_t *profile)
{
    uint32_t mask = 0;
    int      bit;

    for (bit = 0; bit < BUTTON_COUNT; bit++) {
        if (profile->buttons[bit] != NULL) {
            mask |= UINT32_C(1) << bit;
        }
    }
    return mask;
}

static uint8_t snap_trigger(uint8_t value)
{
    return value > VC_TRIGGER_MAX / 2 ? VC_TRIGGER_MAX : 0;
}

void vc_parser_apply_profile(vc_controller_type_t type, vc_input_state_t *state)
{
    const profile_t *profile = profile_for(type);

    if (profile == NULL || state == NULL) {
        return;
    }

    state->buttons &= profile_button_mask(profile);

    if (!profile->analog_triggers) {
        state->trigger_left = snap_trigger(state->trigger_left);
        state->trigger_right = snap_trigger(state->trigger_right);
    }
    if (!profile->has_motion) {
        memset(&state->motion, 0, sizeof state->motion);
        state->flags &= (uint8_t)~VC_INPUT_HAS_MOTION;
    }
    if (!profile->has_touch) {
        memset(state->touch, 0, sizeof state->touch);
        state->flags &= (uint8_t)~VC_INPUT_HAS_TOUCH;
    }
}

const char *vc_parser_button_name(vc_controller_type_t type, uint32_t button)
{
    const profile_t *profile = profile_for(type);
    int              bit;

    if (profile == NULL) {
        return NULL;
    }
    for (bit = 0; bit < BUTTON_COUNT; bit++) {
        if (button == (UINT32_C(1) << bit)) {
            return profile->buttons[bit];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Per-session parser                                                       */
/* ------------------------------------------------------------------------ */

int vc_parser_init(vc_parser_t *parser, vc_controller_type_t type, uint32_t token)
{
    if (parser == NULL || profile_for(type) == NULL) {
        return -1;
    }
    memset(parser, 0, sizeof *parser);
    parser->type = type;
    parser->token = token;
    return 0;
}

void vc_parser_reset(vc_parser_t *parser)
{
    if (parser != NULL) {
        parser->has_sequence = 0;
        parser->last_sequence = 0;
        parser->buffered = 0;
    }
}

/* Serial-number comparison on 16 bits: `sequence` is newer if it is ahead of
 * `last` by 1..32767, counting through the wrap from 65535 to 0. */
static int sequence_is_newer(uint16_t sequence, uint16_t last)
{
    uint16_t ahead = (uint16_t)(sequence - last);
    return ahead != 0 && ahead < 0x8000u;
}

vc_parser_result_t vc_parser_accept(vc_parser_t *parser,
                                    const vc_parser_packet_t *packet,
                                    vc_input_state_t *out)
{
    if (parser == NULL || packet == NULL || out == NULL) {
        return VC_PARSE_TOO_SHORT;
    }
    /* Token first, so a stranger's packet can never move the sequence window. */
    if (parser->token != 0 && packet->token != parser->token) {
        parser->stats.rejected_token++;
        return VC_PARSE_BAD_TOKEN;
    }
    if (parser->has_sequence &&
        !sequence_is_newer(packet->sequence, parser->last_sequence)) {
        parser->stats.rejected_stale++;
        return VC_PARSE_STALE;
    }

    parser->last_sequence = packet->sequence;
    parser->has_sequence = 1;
    parser->stats.accepted++;

    *out = packet->state;
    vc_parser_apply_profile(parser->type, out);
    return VC_PARSE_OK;
}

vc_parser_result_t vc_parser_parse(vc_parser_t *parser, const uint8_t *data,
                                   size_t size, vc_input_state_t *out)
{
    vc_parser_packet_t packet;
    vc_parser_result_t result;

    if (parser == NULL || out == NULL) {
        return VC_PARSE_TOO_SHORT;
    }
    result = vc_parser_decode(data, size, &packet);
    if (result != VC_PARSE_OK) {
        parser->stats.rejected_malformed++;
        return result;
    }
    return vc_parser_accept(parser, &packet, out);
}

/* ------------------------------------------------------------------------ */
/* Byte streams                                                             */
/* ------------------------------------------------------------------------ */

#define MAGIC_BYTE_0     (VC_PROTOCOL_MAGIC & 0xFFu)
#define MAGIC_BYTE_1     ((VC_PROTOCOL_MAGIC >> 8) & 0xFFu)
/* Largest payload any packet may declare. The reassembly buffer holds exactly
 * one such packet, so a complete frame always fits. */
#define MAX_PAYLOAD      (VC_INPUT_DATAGRAM_MAX - VC_PACKET_HEADER_SIZE)

static int is_known_type(uint8_t type)
{
    return (type >= VC_MSG_HELLO && type <= VC_MSG_BYE) || type == VC_MSG_INPUT;
}

static void drop_front(vc_parser_t *parser, size_t count)
{
    parser->buffered = (uint8_t)(parser->buffered - count);
    memmove(parser->buffer, parser->buffer + count, parser->buffered);
}

/* Out of sync: skip one byte and look for a header again. */
static void resync(vc_parser_t *parser)
{
    drop_front(parser, 1);
    parser->stats.bytes_discarded++;
}

/* Handles every complete packet in the buffer; returns when more bytes are
 * needed. */
static int drain(vc_parser_t *parser, vc_parser_state_cb on_state, void *user)
{
    int delivered = 0;

    for (;;) {
        const uint8_t     *buf = parser->buffer;
        vc_input_state_t   state;
        vc_parser_result_t result;
        size_t             frame;

        if (parser->buffered == 0) {
            break;
        }
        if (buf[0] != MAGIC_BYTE_0 ||
            (parser->buffered >= 2 && buf[1] != MAGIC_BYTE_1)) {
            resync(parser);
            continue;
        }
        if (parser->buffered < VC_PACKET_HEADER_SIZE) {
            break;
        }
        if (buf[HDR_OFF_VERSION] != VC_PROTOCOL_VERSION ||
            !is_known_type(buf[HDR_OFF_TYPE]) ||
            read_u16(buf + HDR_OFF_LENGTH) > MAX_PAYLOAD) {
            resync(parser);
            continue;
        }
        frame = VC_PACKET_HEADER_SIZE + (size_t)read_u16(buf + HDR_OFF_LENGTH);
        if (parser->buffered < frame) {
            break;
        }

        if (buf[HDR_OFF_TYPE] != VC_MSG_INPUT) {
            /* A control message: well-framed, but not ours to interpret. */
            parser->stats.frames_skipped++;
            drop_front(parser, frame);
            continue;
        }

        result = vc_parser_parse(parser, buf, frame, &state);
        if (result == VC_PARSE_OK) {
            drop_front(parser, frame);
            delivered++;
            if (on_state != NULL) {
                on_state(&state, user);
            }
        } else if (result == VC_PARSE_BAD_TOKEN || result == VC_PARSE_STALE) {
            /* Well-formed, just refused: the framing is still good. */
            drop_front(parser, frame);
        } else {
            /* The header looked right but the body is not. That may be a
             * false match inside garbage, so do not trust its length. */
            resync(parser);
        }
    }
    return delivered;
}

int vc_parser_feed(vc_parser_t *parser, const uint8_t *data, size_t size,
                   vc_parser_state_cb on_state, void *user)
{
    int delivered = 0;

    if (parser == NULL || (data == NULL && size != 0)) {
        return -1;
    }

    /* drain() always leaves room in the buffer, so each pass consumes input. */
    while (size > 0) {
        size_t space = sizeof parser->buffer - parser->buffered;
        size_t take = size < space ? size : space;

        memcpy(parser->buffer + parser->buffered, data, take);
        parser->buffered = (uint8_t)(parser->buffered + take);
        data += take;
        size -= take;

        delivered += drain(parser, on_state, user);
    }
    return delivered;
}
