/*
 * parser.h - turns raw client bytes into a vc_input_state_t.
 *
 * Three layers, each usable on its own:
 *
 *   vc_parser_decode()   stateless: validates one packet's framing and unpacks it
 *   vc_parser_accept()   per session: checks token and sequence number, then
 *                        applies the active controller profile
 *   vc_parser_feed()     for byte streams: reassembles packets from arbitrary
 *                        chunks, resynchronises after garbage, and runs the
 *                        two steps above on each packet
 *
 * No sockets, no allocation, no OS calls. A vc_parser_t belongs to one
 * session and must not be shared between threads without locking.
 *
 * The wire format carries no checksum of its own: UDP and TCP already
 * checksum every segment. Integrity here means strict structural validation
 * plus the sequence check, which rejects duplicated and reordered packets.
 */
#ifndef VC_PARSER_H
#define VC_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "controller.h"

typedef enum {
    VC_PARSE_OK = 0,
    /* Malformed: reported by vc_parser_decode(). */
    VC_PARSE_TOO_SHORT,     /* smaller than a packet header */
    VC_PARSE_BAD_MAGIC,
    VC_PARSE_BAD_VERSION,
    VC_PARSE_BAD_TYPE,      /* valid header, but not VC_MSG_INPUT */
    VC_PARSE_BAD_LENGTH,    /* length field, packet size and flags disagree */
    VC_PARSE_BAD_FIELD,     /* unknown flag or button bit, or reserved byte set */
    /* Well-formed but refused: reported by vc_parser_accept(). */
    VC_PARSE_BAD_TOKEN,     /* not this session's token */
    VC_PARSE_STALE          /* duplicate, or older than the last accepted packet */
} vc_parser_result_t;

const char *vc_parser_strerror(vc_parser_result_t result);

/* ------------------------------------------------------------------------ */
/* Stateless decoding                                                       */
/* ------------------------------------------------------------------------ */

/* One unpacked VC_MSG_INPUT packet, before any session checks. */
typedef struct {
    vc_input_state_t state;      /* exactly as sent; no profile applied */
    uint32_t         token;      /* session token the client claims */
    uint16_t         sequence;   /* header sequence number */
} vc_parser_packet_t;

/* Unpacks one complete packet (header + payload, nothing before or after).
 * On failure *out is left untouched. */
vc_parser_result_t vc_parser_decode(const uint8_t *data, size_t size,
                                    vc_parser_packet_t *out);

/* ------------------------------------------------------------------------ */
/* Controller profiles                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Buttons are positional on the wire, so a profile does not remap them. It
 * limits a state to what the emulated controller physically has:
 *
 *            buttons removed      motion   touch   triggers
 *   Xbox     Touchpad             no       no      analog
 *   PS5      -                    yes      yes     analog
 *   PS4      Misc (no mic mute)   yes      yes     analog
 *   Switch   Touchpad             yes      no      digital: 0 or 255
 *
 * Unsupported motion and touch data is zeroed and its flag cleared.
 */
void vc_parser_apply_profile(vc_controller_type_t type, vc_input_state_t *state);

/* Label printed on a button for the given controller, e.g. VC_BTN_SOUTH is
 * "A" on Xbox, "Cross" on PS5 and "B" on Switch. Takes a single VC_BTN_* bit.
 * Returns NULL if the controller has no such button or an argument is invalid. */
const char *vc_parser_button_name(vc_controller_type_t type, uint32_t button);

/* ------------------------------------------------------------------------ */
/* Per-session parser                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint64_t accepted;
    uint64_t rejected_malformed;
    uint64_t rejected_token;
    uint64_t rejected_stale;
    uint64_t frames_skipped;     /* stream only: valid packets of another type */
    uint64_t bytes_discarded;    /* stream only: skipped while resynchronising */
} vc_parser_stats_t;

/* Caller-allocated; treat the fields as private and use the functions below. */
typedef struct {
    vc_controller_type_t type;
    uint32_t             token;
    uint16_t             last_sequence;
    uint8_t              has_sequence;
    uint8_t              buffered;
    uint8_t              buffer[VC_INPUT_DATAGRAM_MAX];
    vc_parser_stats_t    stats;
} vc_parser_t;

/* Prepares a parser for one session. `type` is the active profile. `token`
 * is the session token every packet must carry; 0 disables the token check.
 * Returns 0, or -1 if `type` is not a real controller type. */
int vc_parser_init(vc_parser_t *parser, vc_controller_type_t type, uint32_t token);

/* Forgets the last sequence number and any partial stream data, keeping the
 * profile, token and counters. Call it when a client reconnects or after an
 * input timeout: sequence numbers are 16-bit, so after a gap of more than
 * 32767 packets new ones would otherwise look stale. */
void vc_parser_reset(vc_parser_t *parser);

/* Session checks on an already decoded packet: token, then sequence number
 * (16-bit serial arithmetic, so wrap-around from 65535 to 0 is accepted),
 * then the profile. Writes *out only when it returns VC_PARSE_OK. */
vc_parser_result_t vc_parser_accept(vc_parser_t *parser,
                                    const vc_parser_packet_t *packet,
                                    vc_input_state_t *out);

/* vc_parser_decode() followed by vc_parser_accept(), for one whole packet
 * such as a UDP datagram. */
vc_parser_result_t vc_parser_parse(vc_parser_t *parser, const uint8_t *data,
                                   size_t size, vc_input_state_t *out);

typedef void (*vc_parser_state_cb)(const vc_input_state_t *state, void *user);

/* Feeds a chunk of a byte stream. Chunks may split packets anywhere. Calls
 * `on_state` (may be NULL) once per accepted input packet, in order. Bytes
 * that do not form a valid packet are discarded one at a time until the
 * stream lines up again. Returns the number of states accepted from this
 * chunk, or -1 on a NULL argument. */
int vc_parser_feed(vc_parser_t *parser, const uint8_t *data, size_t size,
                   vc_parser_state_cb on_state, void *user);

#endif /* VC_PARSER_H */
