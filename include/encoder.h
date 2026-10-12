/*
 * encoder.h - turns a vc_input_state_t into the bytes a client sends.
 *
 * The inverse of vc_parser_decode() (parser.h): what this writes, the parser
 * reads back unchanged. Used by the test client (tools/vc_send.c) and the
 * tests; the daemon itself only decodes.
 *
 * No sockets, no allocation, no OS calls.
 */
#ifndef VC_ENCODER_H
#define VC_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "controller.h"

/* Writes one VC_MSG_INPUT datagram (header + payload) into buf. The motion
 * and touch blocks are included when state->flags asks for them. Fields are
 * written as given, without validation, so callers can also build packets
 * the parser must reject. Returns the datagram size, or 0 if an argument is
 * NULL or buf is smaller than the datagram. */
size_t vc_encode_input(const vc_input_state_t *state, uint32_t token,
                       uint16_t sequence, uint8_t *buf, size_t size);

#endif /* VC_ENCODER_H */
