/*
 * protocol.c - TCP control channel messages.
 * See protocol.h for the contract.
 */
#include <string.h>

#include "protocol.h"

/* Offsets within the packet header; they mirror vc_packet_header_t. */
#define HDR_OFF_MAGIC     0
#define HDR_OFF_VERSION   2
#define HDR_OFF_TYPE      3
#define HDR_OFF_SEQUENCE  4
#define HDR_OFF_LENGTH    6

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

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Payload size of a control message type, or -1 if it is not one. */
static int payload_size(uint8_t type)
{
    switch (type) {
    case VC_MSG_HELLO:    return VC_PACKET_HELLO_SIZE;
    case VC_MSG_WELCOME:  return VC_PACKET_WELCOME_SIZE;
    case VC_MSG_PING:     return VC_PACKET_PING_SIZE;
    case VC_MSG_PONG:     return VC_PACKET_PING_SIZE;
    case VC_MSG_FEEDBACK: return VC_PACKET_FEEDBACK_SIZE;
    case VC_MSG_ERROR:    return VC_PACKET_ERROR_SIZE;
    case VC_MSG_BYE:      return 0;
    default:              return -1;
    }
}

const char *vc_proto_type_name(uint8_t type)
{
    switch (type) {
    case VC_MSG_HELLO:    return "HELLO";
    case VC_MSG_WELCOME:  return "WELCOME";
    case VC_MSG_PING:     return "PING";
    case VC_MSG_PONG:     return "PONG";
    case VC_MSG_FEEDBACK: return "FEEDBACK";
    case VC_MSG_ERROR:    return "ERROR";
    case VC_MSG_BYE:      return "BYE";
    case VC_MSG_INPUT:    return "INPUT";
    default:              return "?";
    }
}

const char *vc_proto_strerror(vc_proto_result_t result)
{
    switch (result) {
    case VC_PROTO_OK:          return "ok";
    case VC_PROTO_NEED_MORE:   return "incomplete frame";
    case VC_PROTO_BAD_MAGIC:   return "bad magic (not a vcd client)";
    case VC_PROTO_BAD_VERSION: return "unsupported protocol version";
    case VC_PROTO_BAD_TYPE:    return "not a control message";
    case VC_PROTO_BAD_LENGTH:  return "length does not match the message type";
    }
    return "unknown error";
}

/* ------------------------------------------------------------------------ */
/* Encoding                                                                 */
/* ------------------------------------------------------------------------ */

size_t vc_proto_encode(const vc_proto_msg_t *msg, uint8_t *buf, size_t size)
{
    uint8_t *p;
    size_t   name_len;
    int      length;

    if (msg == NULL || buf == NULL) {
        return 0;
    }
    length = payload_size(msg->type);
    if (length < 0 || size < VC_PACKET_HEADER_SIZE + (size_t)length) {
        return 0;
    }

    write_u16(buf + HDR_OFF_MAGIC, VC_PROTOCOL_MAGIC);
    buf[HDR_OFF_VERSION] = VC_PROTOCOL_VERSION;
    buf[HDR_OFF_TYPE] = msg->type;
    write_u16(buf + HDR_OFF_SEQUENCE, msg->sequence);
    write_u16(buf + HDR_OFF_LENGTH, (uint16_t)length);
    p = buf + VC_PACKET_HEADER_SIZE;

    switch (msg->type) {
    case VC_MSG_HELLO:
        p[0] = msg->controller_type;
        p[1] = msg->caps;
        memset(p + 2, 0, VC_CLIENT_NAME_LEN);
        name_len = 0;
        while (name_len < VC_CLIENT_NAME_LEN && msg->name[name_len] != '\0') {
            name_len++;
        }
        memcpy(p + 2, msg->name, name_len);
        break;
    case VC_MSG_WELCOME:
        write_u32(p, msg->token);
        p[4] = msg->slot;
        p[5] = msg->caps;
        write_u16(p + 6, msg->timeout_ms);
        break;
    case VC_MSG_PING:
    case VC_MSG_PONG:
        write_u32(p, msg->echo);
        break;
    case VC_MSG_FEEDBACK:
        p[0] = msg->feedback.rumble_low;
        p[1] = msg->feedback.rumble_high;
        p[2] = msg->feedback.led[0];
        p[3] = msg->feedback.led[1];
        p[4] = msg->feedback.led[2];
        p[5] = msg->feedback.player;
        break;
    case VC_MSG_ERROR:
        p[0] = msg->error;
        break;
    default:   /* BYE: no payload */
        break;
    }
    return VC_PACKET_HEADER_SIZE + (size_t)length;
}

/* ------------------------------------------------------------------------ */
/* Decoding                                                                 */
/* ------------------------------------------------------------------------ */

/* Checks a complete header. */
static vc_proto_result_t check_header(const uint8_t *h)
{
    int length;

    if (read_u16(h + HDR_OFF_MAGIC) != VC_PROTOCOL_MAGIC) {
        return VC_PROTO_BAD_MAGIC;
    }
    if (h[HDR_OFF_VERSION] != VC_PROTOCOL_VERSION) {
        return VC_PROTO_BAD_VERSION;
    }
    length = payload_size(h[HDR_OFF_TYPE]);
    if (length < 0) {
        return VC_PROTO_BAD_TYPE;
    }
    if (read_u16(h + HDR_OFF_LENGTH) != (uint16_t)length) {
        return VC_PROTO_BAD_LENGTH;
    }
    return VC_PROTO_OK;
}

/* Decodes a complete, header-checked frame. */
static void decode_frame(const uint8_t *frame, vc_proto_msg_t *out)
{
    const uint8_t *p = frame + VC_PACKET_HEADER_SIZE;
    size_t         i;

    memset(out, 0, sizeof *out);
    out->type = frame[HDR_OFF_TYPE];
    out->sequence = read_u16(frame + HDR_OFF_SEQUENCE);

    switch (out->type) {
    case VC_MSG_HELLO:
        out->controller_type = p[0];
        out->caps = p[1];
        /* The name need not be terminated on the wire; out->name always is. */
        for (i = 0; i < VC_CLIENT_NAME_LEN && p[2 + i] != 0; i++) {
            out->name[i] = (char)p[2 + i];
        }
        out->name[i] = '\0';
        break;
    case VC_MSG_WELCOME:
        out->token = read_u32(p);
        out->slot = p[4];
        out->caps = p[5];
        out->timeout_ms = read_u16(p + 6);
        break;
    case VC_MSG_PING:
    case VC_MSG_PONG:
        out->echo = read_u32(p);
        break;
    case VC_MSG_FEEDBACK:
        out->feedback.rumble_low = p[0];
        out->feedback.rumble_high = p[1];
        out->feedback.led[0] = p[2];
        out->feedback.led[1] = p[3];
        out->feedback.led[2] = p[4];
        out->feedback.player = p[5];
        break;
    case VC_MSG_ERROR:
        out->error = p[0];
        break;
    default:   /* BYE */
        break;
    }
}

void vc_proto_reader_init(vc_proto_reader_t *reader)
{
    if (reader != NULL) {
        reader->used = 0;
    }
}

vc_proto_result_t vc_proto_read(vc_proto_reader_t *reader, const uint8_t **data,
                                size_t *size, vc_proto_msg_t *out)
{
    size_t            want;
    size_t            take;
    vc_proto_result_t result;

    if (reader == NULL || data == NULL || size == NULL || out == NULL ||
        (*data == NULL && *size > 0)) {
        return VC_PROTO_NEED_MORE;
    }

    for (;;) {
        /* Next target: the header, then the rest of the frame. */
        want = VC_PACKET_HEADER_SIZE;
        if (reader->used >= VC_PACKET_HEADER_SIZE) {
            want += read_u16(reader->buffer + HDR_OFF_LENGTH);
        }
        if (reader->used == want) {
            decode_frame(reader->buffer, out);
            reader->used = 0;
            return VC_PROTO_OK;
        }
        if (*size == 0) {
            return VC_PROTO_NEED_MORE;
        }

        take = want - reader->used;
        if (take > *size) {
            take = *size;
        }
        memcpy(reader->buffer + reader->used, *data, take);
        reader->used += take;
        *data += take;
        *size -= take;

        /* Validate the header the moment it is complete, before trusting its
         * length field to size the payload. */
        if (reader->used == VC_PACKET_HEADER_SIZE) {
            result = check_header(reader->buffer);
            if (result != VC_PROTO_OK) {
                return result;
            }
        }
    }
}
