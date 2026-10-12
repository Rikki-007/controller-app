/*
 * encoder.c - turns a vc_input_state_t into the bytes a client sends.
 * See encoder.h for the contract.
 */
#include "encoder.h"

/* Fields are little-endian on the wire and written byte by byte, so the
 * host's byte order and alignment rules do not matter. */
static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t *p, uint32_t v)
{
    write_u16(p, (uint16_t)(v & 0xFFFFu));
    write_u16(p + 2, (uint16_t)(v >> 16));
}

/* Two's complement, whatever the host's own representation. */
static void write_i16(uint8_t *p, int16_t v)
{
    write_u16(p, (uint16_t)(v < 0 ? (int32_t)v + 0x10000 : (int32_t)v));
}

size_t vc_encode_input(const vc_input_state_t *state, uint32_t token,
                       uint16_t sequence, uint8_t *buf, size_t size)
{
    size_t   length = VC_PACKET_INPUT_SIZE;
    uint8_t *p;
    int      i;

    if (state == NULL || buf == NULL) {
        return 0;
    }
    if (state->flags & VC_INPUT_HAS_MOTION) {
        length += VC_PACKET_MOTION_SIZE;
    }
    if (state->flags & VC_INPUT_HAS_TOUCH) {
        length += VC_PACKET_TOUCH_SIZE;
    }
    if (size < VC_PACKET_HEADER_SIZE + length) {
        return 0;
    }

    /* Header; offsets mirror vc_packet_header_t. */
    write_u16(buf + 0, VC_PROTOCOL_MAGIC);
    buf[2] = VC_PROTOCOL_VERSION;
    buf[3] = VC_MSG_INPUT;
    write_u16(buf + 4, sequence);
    write_u16(buf + 6, (uint16_t)length);

    /* Fixed part; offsets mirror vc_packet_input_t. */
    p = buf + VC_PACKET_HEADER_SIZE;
    write_u32(p + 0, token);
    write_u32(p + 4, state->timestamp_us);
    write_u32(p + 8, state->buttons);
    write_i16(p + 12, state->left.x);
    write_i16(p + 14, state->left.y);
    write_i16(p + 16, state->right.x);
    write_i16(p + 18, state->right.y);
    p[20] = state->trigger_left;
    p[21] = state->trigger_right;
    p[22] = state->flags;
    p[23] = 0;
    p += VC_PACKET_INPUT_SIZE;

    if (state->flags & VC_INPUT_HAS_MOTION) {
        for (i = 0; i < 3; i++) {
            write_i16(p + 2 * i, state->motion.gyro[i]);
            write_i16(p + 6 + 2 * i, state->motion.accel[i]);
        }
        p += VC_PACKET_MOTION_SIZE;
    }
    if (state->flags & VC_INPUT_HAS_TOUCH) {
        for (i = 0; i < VC_TOUCH_POINTS; i++) {
            const vc_touch_point_t *point = &state->touch[i];
            uint8_t                *out = p + (VC_PACKET_TOUCH_SIZE / VC_TOUCH_POINTS) * i;

            out[0] = (uint8_t)((point->active ? VC_TOUCH_STATE_ACTIVE : 0u) |
                               (point->id & VC_TOUCH_STATE_ID_MASK));
            out[1] = 0;
            write_u16(out + 2, point->x);
            write_u16(out + 4, point->y);
        }
    }
    return VC_PACKET_HEADER_SIZE + length;
}
