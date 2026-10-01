/*
 * test_parser.c - unit tests for the packet parser. No sockets involved.
 *
 * Build:  cc -std=c99 -Iinclude src/parser.c tests/test_parser.c
 */
#include "parser.h"
#include "test_util.h"

#define FLAGS_ALL (VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH)

/* ------------------------------------------------------------------------ */
/* Stateless decoding                                                       */
/* ------------------------------------------------------------------------ */

static void test_decode_valid(void)
{
    uint8_t            buf[VC_INPUT_DATAGRAM_MAX];
    vc_parser_packet_t in;
    size_t             size;

    size = build_input(buf, 0, 0xFFFE);
    CHECK(size == 32);
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_OK);
    CHECK(in.sequence == 0xFFFE);
    CHECK(in.token == TEST_TOKEN);
    CHECK(in.state.timestamp_us == 123456789u);
    CHECK(in.state.buttons == TEST_BUTTONS);
    CHECK(in.state.left.x == -32768);
    CHECK(in.state.left.y == 32767);
    CHECK(in.state.right.x == -1);
    CHECK(in.state.right.y == 256);
    CHECK(in.state.trigger_left == 200);
    CHECK(in.state.trigger_right == 7);
    CHECK(in.state.flags == 0);
    CHECK(in.state.touch[0].active == 0);

    size = build_input(buf, FLAGS_ALL, 1);
    CHECK(size == VC_INPUT_DATAGRAM_MAX);
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_OK);
    CHECK(in.state.motion.gyro[0] == -1);
    CHECK(in.state.motion.gyro[2] == -3);
    CHECK(in.state.motion.accel[0] == -4);
    CHECK(in.state.motion.accel[2] == -6);
    CHECK(in.state.touch[0].active == 1);
    CHECK(in.state.touch[0].id == 5);
    CHECK(in.state.touch[0].x == 1000);
    CHECK(in.state.touch[0].y == 65535);
    CHECK(in.state.touch[1].active == 0);

    /* Touch without motion: the touch block directly follows the fixed part. */
    size = build_input(buf, VC_INPUT_HAS_TOUCH, 2);
    CHECK(size == 44);
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_OK);
    CHECK(in.state.touch[0].x == 1000);
}

static void test_decode_rejects(void)
{
    uint8_t            buf[VC_INPUT_DATAGRAM_MAX + 8];
    vc_parser_packet_t in;
    size_t             size;

    memset(&in, 0x5A, sizeof in);

    size = build_input(buf, 0, 1);
    CHECK(vc_parser_decode(buf, 7, &in) == VC_PARSE_TOO_SHORT);
    CHECK(vc_parser_decode(buf, 0, &in) == VC_PARSE_TOO_SHORT);
    CHECK(vc_parser_decode(NULL, size, &in) == VC_PARSE_TOO_SHORT);

    buf[0] ^= 0xFF;
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_MAGIC);

    build_input(buf, 0, 1);
    buf[2] = VC_PROTOCOL_VERSION + 1;
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_VERSION);

    build_input(buf, 0, 1);
    buf[3] = VC_MSG_HELLO;
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_TYPE);

    /* Truncated packet, trailing junk, and a header-only packet. */
    build_input(buf, 0, 1);
    CHECK(vc_parser_decode(buf, size - 1, &in) == VC_PARSE_BAD_LENGTH);
    CHECK(vc_parser_decode(buf, size + 1, &in) == VC_PARSE_BAD_LENGTH);
    put_u16(buf + 6, 0);
    CHECK(vc_parser_decode(buf, VC_PACKET_HEADER_SIZE, &in) == VC_PARSE_BAD_LENGTH);

    /* Flag claims a motion block that is not there, and the reverse. */
    build_input(buf, 0, 1);
    buf[VC_PACKET_HEADER_SIZE + 22] = VC_INPUT_HAS_MOTION;
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_LENGTH);
    size = build_input(buf, VC_INPUT_HAS_MOTION, 1);
    buf[VC_PACKET_HEADER_SIZE + 22] = 0;
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_LENGTH);

    size = build_input(buf, 0, 1);
    buf[VC_PACKET_HEADER_SIZE + 22] = 0x80;   /* unknown flag */
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_FIELD);

    build_input(buf, 0, 1);
    buf[VC_PACKET_HEADER_SIZE + 23] = 1;      /* reserved byte */
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_FIELD);

    build_input(buf, 0, 1);
    buf[VC_PACKET_HEADER_SIZE + 11] = 0x80;   /* button bit 31 */
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_FIELD);

    size = build_input(buf, VC_INPUT_HAS_TOUCH, 1);
    buf[VC_PACKET_HEADER_SIZE + VC_PACKET_INPUT_SIZE + 1] = 1;   /* touch reserved */
    CHECK(vc_parser_decode(buf, size, &in) == VC_PARSE_BAD_FIELD);

    /* None of the failures above may have written to the output. */
    CHECK(in.token == 0x5A5A5A5Au);
    CHECK(in.sequence == 0x5A5A);
}

/* ------------------------------------------------------------------------ */
/* Sequence numbers and token                                               */
/* ------------------------------------------------------------------------ */

static vc_parser_result_t parse_seq(vc_parser_t *parser, unsigned sequence)
{
    uint8_t          buf[VC_INPUT_DATAGRAM_MAX];
    vc_input_state_t state;
    size_t           size = build_input(buf, 0, sequence);

    return vc_parser_parse(parser, buf, size, &state);
}

static void test_sequence(void)
{
    vc_parser_t parser;

    CHECK(vc_parser_init(&parser, VC_CONTROLLER_PS5, TEST_TOKEN) == 0);

    /* The first packet is accepted whatever its number. */
    CHECK(parse_seq(&parser, 500) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 500) == VC_PARSE_STALE);   /* duplicate */
    CHECK(parse_seq(&parser, 499) == VC_PARSE_STALE);   /* reordered */
    CHECK(parse_seq(&parser, 501) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 510) == VC_PARSE_OK);      /* lost packets are fine */
    CHECK(parse_seq(&parser, 505) == VC_PARSE_STALE);   /* late arrival */

    /* More than half the number space ahead reads as old, not new. */
    CHECK(parse_seq(&parser, 0xFFFE) == VC_PARSE_STALE);

    /* Wrap-around from 65535 to 0 counts as moving forward. */
    vc_parser_reset(&parser);
    CHECK(parse_seq(&parser, 0xFFFE) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 0xFFFF) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 0) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 0xFFFF) == VC_PARSE_STALE);
    CHECK(parse_seq(&parser, 3) == VC_PARSE_OK);

    /* Exactly half the number space ahead is ambiguous and treated as old. */
    CHECK(parse_seq(&parser, 3 + 0x7FFF) == VC_PARSE_OK);
    CHECK(parse_seq(&parser, 3 + 0x7FFF + 0x8000) == VC_PARSE_STALE);

    /* After a reset any number is accepted again. */
    vc_parser_reset(&parser);
    CHECK(parse_seq(&parser, 1) == VC_PARSE_OK);

    CHECK(parser.stats.accepted == 9);
    CHECK(parser.stats.rejected_stale == 6);
    CHECK(parser.stats.rejected_malformed == 0);
}

static void test_token(void)
{
    vc_parser_t      parser;
    vc_input_state_t state;
    uint8_t          buf[VC_INPUT_DATAGRAM_MAX];
    size_t           size;

    CHECK(vc_parser_init(&parser, VC_CONTROLLER_XBOX, 0x11111111u) == 0);
    CHECK(parse_seq(&parser, 10) == VC_PARSE_BAD_TOKEN);
    CHECK(parser.stats.rejected_token == 1);

    /* A wrong-token packet must not move the sequence window. */
    size = build_input(buf, 0, 9);
    put_u32(buf + VC_PACKET_HEADER_SIZE, 0x11111111u);
    memset(&state, 0x5A, sizeof state);
    CHECK(vc_parser_parse(&parser, buf, size, &state) == VC_PARSE_OK);
    CHECK(state.buttons == VC_BTN_SOUTH + VC_BTN_MISC);

    /* A rejected packet leaves the caller's state untouched. */
    memset(&state, 0x5A, sizeof state);
    CHECK(vc_parser_parse(&parser, buf, size, &state) == VC_PARSE_STALE);
    CHECK(vc_parser_parse(&parser, buf, size - 1, &state) == VC_PARSE_BAD_LENGTH);
    CHECK(state.buttons == 0x5A5A5A5Au);
    CHECK(parser.stats.rejected_malformed == 1);

    /* Token 0 turns the check off. */
    CHECK(vc_parser_init(&parser, VC_CONTROLLER_XBOX, 0) == 0);
    CHECK(parse_seq(&parser, 1) == VC_PARSE_OK);

    CHECK(vc_parser_init(&parser, VC_CONTROLLER_NONE, 0) == -1);
    CHECK(vc_parser_init(&parser, (vc_controller_type_t)99, 0) == -1);
    CHECK(vc_parser_init(NULL, VC_CONTROLLER_PS5, 0) == -1);
}

/* ------------------------------------------------------------------------ */
/* Controller profiles                                                      */
/* ------------------------------------------------------------------------ */

/* Parses the same fully-populated packet under the given profile. */
static vc_input_state_t parse_as(vc_controller_type_t type)
{
    vc_parser_t      parser;
    vc_input_state_t state;
    uint8_t          buf[VC_INPUT_DATAGRAM_MAX];
    size_t           size = build_input(buf, FLAGS_ALL, 1);

    memset(&state, 0, sizeof state);
    CHECK(vc_parser_init(&parser, type, TEST_TOKEN) == 0);
    CHECK(vc_parser_parse(&parser, buf, size, &state) == VC_PARSE_OK);
    return state;
}

static void test_profiles(void)
{
    vc_input_state_t s;

    /* PS5 has everything the wire format can carry. */
    s = parse_as(VC_CONTROLLER_PS5);
    CHECK(s.buttons == TEST_BUTTONS);
    CHECK(s.flags == FLAGS_ALL);
    CHECK(s.trigger_left == 200 && s.trigger_right == 7);
    CHECK(s.motion.gyro[0] == -1);
    CHECK(s.touch[0].active == 1 && s.touch[0].x == 1000);
    CHECK(s.left.x == -32768 && s.right.y == 256);

    /* PS4: as PS5 but without the mic-mute button. */
    s = parse_as(VC_CONTROLLER_PS4);
    CHECK(s.buttons == (VC_BTN_SOUTH | VC_BTN_TOUCHPAD));
    CHECK(s.flags == FLAGS_ALL);
    CHECK(s.touch[0].active == 1);

    /* Xbox: no touchpad, no motion, analog triggers. */
    s = parse_as(VC_CONTROLLER_XBOX);
    CHECK(s.buttons == (VC_BTN_SOUTH | VC_BTN_MISC));
    CHECK(s.flags == 0);
    CHECK(s.trigger_left == 200 && s.trigger_right == 7);
    CHECK(s.motion.gyro[0] == 0 && s.motion.accel[2] == 0);
    CHECK(s.touch[0].active == 0 && s.touch[0].x == 0);
    CHECK(s.left.x == -32768 && s.right.y == 256);

    /* Switch: motion but no touchpad, and triggers are on or off. */
    s = parse_as(VC_CONTROLLER_SWITCH);
    CHECK(s.buttons == (VC_BTN_SOUTH | VC_BTN_MISC));
    CHECK(s.flags == VC_INPUT_HAS_MOTION);
    CHECK(s.trigger_left == 255 && s.trigger_right == 0);
    CHECK(s.motion.accel[2] == -6);
    CHECK(s.touch[0].active == 0);

    /* Trigger snapping threshold. */
    memset(&s, 0, sizeof s);
    s.trigger_left = 127;
    s.trigger_right = 128;
    vc_parser_apply_profile(VC_CONTROLLER_SWITCH, &s);
    CHECK(s.trigger_left == 0 && s.trigger_right == 255);

    /* An invalid type changes nothing. */
    s.buttons = VC_BTN_TOUCHPAD;
    vc_parser_apply_profile(VC_CONTROLLER_NONE, &s);
    CHECK(s.buttons == VC_BTN_TOUCHPAD);
}

static int name_is(vc_controller_type_t type, uint32_t button, const char *want)
{
    const char *name = vc_parser_button_name(type, button);
    if (want == NULL) {
        return name == NULL;
    }
    return name != NULL && strcmp(name, want) == 0;
}

static void test_button_names(void)
{
    /* The same physical position carries a different label per controller. */
    CHECK(name_is(VC_CONTROLLER_XBOX, VC_BTN_SOUTH, "A"));
    CHECK(name_is(VC_CONTROLLER_PS5, VC_BTN_SOUTH, "Cross"));
    CHECK(name_is(VC_CONTROLLER_PS4, VC_BTN_SOUTH, "Cross"));
    CHECK(name_is(VC_CONTROLLER_SWITCH, VC_BTN_SOUTH, "B"));
    CHECK(name_is(VC_CONTROLLER_SWITCH, VC_BTN_EAST, "A"));
    CHECK(name_is(VC_CONTROLLER_XBOX, VC_BTN_NORTH, "Y"));
    CHECK(name_is(VC_CONTROLLER_SWITCH, VC_BTN_NORTH, "X"));
    CHECK(name_is(VC_CONTROLLER_PS5, VC_BTN_SELECT, "Create"));
    CHECK(name_is(VC_CONTROLLER_PS4, VC_BTN_SELECT, "Share"));
    CHECK(name_is(VC_CONTROLLER_SWITCH, VC_BTN_START, "Plus"));
    CHECK(name_is(VC_CONTROLLER_PS5, VC_BTN_TOUCHPAD, "Touchpad"));
    CHECK(name_is(VC_CONTROLLER_XBOX, VC_BTN_DPAD_RIGHT, "D-pad Right"));

    /* Buttons a controller does not have. */
    CHECK(name_is(VC_CONTROLLER_XBOX, VC_BTN_TOUCHPAD, NULL));
    CHECK(name_is(VC_CONTROLLER_SWITCH, VC_BTN_TOUCHPAD, NULL));
    CHECK(name_is(VC_CONTROLLER_PS4, VC_BTN_MISC, NULL));

    /* Invalid arguments. */
    CHECK(name_is(VC_CONTROLLER_NONE, VC_BTN_SOUTH, NULL));
    CHECK(name_is(VC_CONTROLLER_PS5, 0, NULL));
    CHECK(name_is(VC_CONTROLLER_PS5, VC_BTN_SOUTH | VC_BTN_EAST, NULL));
    CHECK(name_is(VC_CONTROLLER_PS5, UINT32_C(1) << 17, NULL));
}

/* ------------------------------------------------------------------------ */
/* Byte streams                                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    int              calls;
    vc_input_state_t last;
} capture_t;

static void on_state(const vc_input_state_t *state, void *user)
{
    capture_t *cap = (capture_t *)user;
    cap->calls++;
    cap->last = *state;
}

static void test_stream(void)
{
    vc_parser_t parser;
    capture_t   cap;
    uint8_t     stream[512];
    size_t      len = 0;
    size_t      bad_at;
    size_t      i;

    /* Layout of the stream:
     *   5 bytes of junk | input #1 | HELLO frame | input #2 with a corrupt body
     *   | 3 bytes of junk that start like a header | input #3 (full size)
     *   | input #3 again (duplicate) | first half of input #4 */
    memset(stream + len, 0x00, 5);
    len += 5;
    len += build_input(stream + len, 0, 1);

    put_u16(stream + len, VC_PROTOCOL_MAGIC);
    stream[len + 2] = VC_PROTOCOL_VERSION;
    stream[len + 3] = VC_MSG_HELLO;
    put_u16(stream + len + 4, 0);
    put_u16(stream + len + 6, VC_PACKET_HELLO_SIZE);
    memset(stream + len + VC_PACKET_HEADER_SIZE, 'x', VC_PACKET_HELLO_SIZE);
    len += VC_PACKET_HEADER_SIZE + VC_PACKET_HELLO_SIZE;

    bad_at = len;
    len += build_input(stream + len, 0, 2);
    stream[bad_at + VC_PACKET_HEADER_SIZE + 23] = 0xFF;   /* reserved byte */

    stream[len++] = 'V';
    stream[len++] = 'C';
    stream[len++] = 0x7F;                                 /* not a version */

    len += build_input(stream + len, FLAGS_ALL, 3);
    len += build_input(stream + len, FLAGS_ALL, 3);
    build_input(stream + len, 0, 4);
    len += 16;

    /* Whole stream in one call. */
    memset(&cap, 0, sizeof cap);
    CHECK(vc_parser_init(&parser, VC_CONTROLLER_PS5, TEST_TOKEN) == 0);
    CHECK(vc_parser_feed(&parser, stream, len, on_state, &cap) == 2);
    CHECK(cap.calls == 2);
    CHECK(cap.last.flags == FLAGS_ALL);
    CHECK(cap.last.touch[0].id == 5);
    CHECK(parser.stats.accepted == 2);
    CHECK(parser.stats.frames_skipped == 1);
    CHECK(parser.stats.rejected_stale == 1);
    CHECK(parser.stats.rejected_malformed == 1);
    CHECK(parser.stats.bytes_discarded == 5 + 32 + 3);
    CHECK(parser.buffered == 16);

    /* The rest of input #4 completes it. */
    CHECK(vc_parser_feed(&parser, stream + len, 16, on_state, &cap) == 1);
    CHECK(cap.calls == 3);
    CHECK(cap.last.flags == 0);
    CHECK(parser.buffered == 0);

    /* Same stream one byte at a time gives the same outcome. */
    memset(&cap, 0, sizeof cap);
    CHECK(vc_parser_init(&parser, VC_CONTROLLER_PS5, TEST_TOKEN) == 0);
    for (i = 0; i < len + 16; i++) {
        CHECK(vc_parser_feed(&parser, stream + i, 1, on_state, &cap) >= 0);
    }
    CHECK(cap.calls == 3);
    CHECK(parser.stats.accepted == 3);
    CHECK(parser.stats.frames_skipped == 1);
    CHECK(parser.stats.rejected_stale == 1);
    CHECK(parser.stats.bytes_discarded == 5 + 32 + 3);
    CHECK(parser.buffered == 0);

    /* And in awkward 7-byte chunks, with no callback. */
    CHECK(vc_parser_init(&parser, VC_CONTROLLER_PS5, TEST_TOKEN) == 0);
    for (i = 0; i < len + 16; i += 7) {
        size_t chunk = len + 16 - i < 7 ? len + 16 - i : 7;
        CHECK(vc_parser_feed(&parser, stream + i, chunk, NULL, NULL) >= 0);
    }
    CHECK(parser.stats.accepted == 3);

    /* Pure garbage never accumulates and never delivers. */
    memset(stream, 0xAB, sizeof stream);
    CHECK(vc_parser_init(&parser, VC_CONTROLLER_PS5, TEST_TOKEN) == 0);
    CHECK(vc_parser_feed(&parser, stream, sizeof stream, on_state, &cap) == 0);
    CHECK(parser.buffered == 0);
    CHECK(parser.stats.bytes_discarded == sizeof stream);

    /* A run of 'V' bytes (first magic byte) must not stall the parser. */
    memset(stream, 'V', sizeof stream);
    CHECK(vc_parser_feed(&parser, stream, sizeof stream, on_state, &cap) == 0);
    CHECK(parser.buffered == 1);

    /* Argument checks. */
    CHECK(vc_parser_feed(NULL, stream, 1, NULL, NULL) == -1);
    CHECK(vc_parser_feed(&parser, NULL, 1, NULL, NULL) == -1);
    CHECK(vc_parser_feed(&parser, NULL, 0, NULL, NULL) == 0);
}

int main(void)
{
    test_decode_valid();
    test_decode_rejects();
    test_sequence();
    test_token();
    test_profiles();
    test_button_names();
    test_stream();
    return test_summary();
}
