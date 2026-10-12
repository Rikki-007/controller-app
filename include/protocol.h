/*
 * protocol.h - TCP control channel messages (PLAN.md s2.2).
 *
 * Encodes and decodes HELLO, WELCOME, PING, PONG, FEEDBACK, ERROR and BYE,
 * and reassembles them from a TCP byte stream. Used by the daemon (session.c)
 * and by the test client (tools/vc_send.c). INPUT, which travels over UDP,
 * is handled by parser.h and encoder.h instead.
 *
 * No sockets, no allocation, no OS calls.
 */
#ifndef VC_PROTOCOL_H
#define VC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "controller.h"

/* Largest control payload (HELLO), and so the largest frame. */
#define VC_PROTO_MAX_PAYLOAD  VC_PACKET_HELLO_SIZE
#define VC_PROTO_MAX_FRAME    (VC_PACKET_HEADER_SIZE + VC_PROTO_MAX_PAYLOAD)

/* One control message, decoded. Only the fields of `type` are meaningful. */
typedef struct {
    uint8_t  type;                              /* vc_message_type_t */
    uint16_t sequence;                          /* header sequence number */
    /* HELLO */
    uint8_t  controller_type;                   /* vc_controller_type_t */
    char     name[VC_CLIENT_NAME_LEN + 1];      /* always NUL-terminated */
    /* HELLO and WELCOME */
    uint8_t  caps;                              /* VC_CAP_* */
    /* WELCOME */
    uint32_t token;
    uint8_t  slot;
    uint16_t timeout_ms;
    /* PING and PONG */
    uint32_t echo;
    /* FEEDBACK */
    vc_packet_feedback_t feedback;
    /* ERROR */
    uint8_t  error;                             /* vc_error_code_t */
} vc_proto_msg_t;

/* Writes `msg` as one frame (header + payload) into buf. Returns the frame
 * size, or 0 if msg->type is not a control message, an argument is NULL or
 * buf is too small. For HELLO, `name` is copied up to its NUL and padded. */
size_t vc_proto_encode(const vc_proto_msg_t *msg, uint8_t *buf, size_t size);

typedef enum {
    VC_PROTO_OK = 0,        /* a message was decoded */
    VC_PROTO_NEED_MORE,     /* all input consumed, no complete frame yet */
    VC_PROTO_BAD_MAGIC,     /* not this protocol */
    VC_PROTO_BAD_VERSION,
    VC_PROTO_BAD_TYPE,      /* unknown type, or INPUT (which belongs on UDP) */
    VC_PROTO_BAD_LENGTH     /* length field does not match the type */
} vc_proto_result_t;

const char *vc_proto_strerror(vc_proto_result_t result);

/* Name of a control message type, e.g. "HELLO"; "?" if unknown. */
const char *vc_proto_type_name(uint8_t type);

/* Stream reassembly for one TCP connection. Caller-allocated; treat the
 * fields as private. */
typedef struct {
    uint8_t buffer[VC_PROTO_MAX_FRAME];
    size_t  used;
} vc_proto_reader_t;

void vc_proto_reader_init(vc_proto_reader_t *reader);

/*
 * Consumes bytes from *data (advancing *data and decreasing *size) until one
 * frame is complete, and decodes it into *out. Call it in a loop until it
 * stops returning VC_PROTO_OK:
 *
 *     while ((r = vc_proto_read(&reader, &data, &size, &msg)) == VC_PROTO_OK)
 *         handle(&msg);
 *     if (r != VC_PROTO_NEED_MORE)
 *         close the connection;
 *
 * The header is checked as soon as its 8 bytes are in, so a peer speaking
 * another protocol is caught without waiting for a payload. TCP cannot
 * resynchronise reliably, so any error is final for the connection.
 */
vc_proto_result_t vc_proto_read(vc_proto_reader_t *reader, const uint8_t **data,
                                size_t *size, vc_proto_msg_t *out);

#endif /* VC_PROTOCOL_H */
