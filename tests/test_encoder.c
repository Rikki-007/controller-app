/*
 * test_encoder.c - the client-side encoder, checked against the hand-built
 * packets in test_util.h and round-tripped through the parser.
 *
 * Build:  cc -std=c99 -Iinclude -Itests src/encoder.c src/parser.c tests/test_encoder.c
 */
#include "encoder.h"
#include "parser.h"
#include "test_util.h"

/* The same values build_input() in test_util.h writes. */
static vc_input_state_t reference_state(uint8_t flags)
{
    vc_input_state_t s;
    int              i;

    memset(&s, 0, sizeof s);
    s.timestamp_us = 123456789u;
    s.buttons = TEST_BUTTONS;
    s.left.x = -32768;
    s.left.y = 32767;
    s.right.x = -1;
    s.right.y = 256;
    s.trigger_left = 200;
    s.trigger_right = 7;
    s.flags = flags;
    if (flags & VC_INPUT_HAS_MOTION) {
        for (i = 0; i < 3; i++) {
            s.motion.gyro[i] = (int16_t)(-1 - i);
            s.motion.accel[i] = (int16_t)(-4 - i);
        }
    }
    if (flags & VC_INPUT_HAS_TOUCH) {
        s.touch[0].active = 1;
        s.touch[0].id = 5;
        s.touch[0].x = 1000;
        s.touch[0].y = 65535;
    }
    return s;
}

/* Byte for byte the same as the independent builder, for every block mix. */
static void test_matches_reference(void)
{
    static const uint8_t all_flags[] = {
        0, VC_INPUT_HAS_MOTION, VC_INPUT_HAS_TOUCH, VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH
    };
    uint8_t want[VC_INPUT_DATAGRAM_MAX];
    uint8_t got[VC_INPUT_DATAGRAM_MAX];
    size_t  i;

    for (i = 0; i < sizeof all_flags; i++) {
        vc_input_state_t s = reference_state(all_flags[i]);
        size_t           want_size = build_input(want, all_flags[i], 0xBEEF);
        size_t           got_size;

        memset(got, 0xCC, sizeof got);
        got_size = vc_encode_input(&s, TEST_TOKEN, 0xBEEF, got, sizeof got);
        CHECK(got_size == want_size);
        CHECK(memcmp(got, want, want_size) == 0);
    }
}

static void test_round_trip(void)
{
    vc_input_state_t   s;
    vc_parser_packet_t p;
    uint8_t            buf[VC_INPUT_DATAGRAM_MAX];
    size_t             size;

    memset(&s, 0, sizeof s);
    s.timestamp_us = 0xFFFFFFFFu;
    s.buttons = VC_BTN_VALID_MASK;
    s.left.x = 32767;
    s.left.y = -32768;
    s.right.x = 1;
    s.right.y = -1;
    s.trigger_left = 255;
    s.trigger_right = 0;
    s.flags = VC_INPUT_HAS_MOTION | VC_INPUT_HAS_TOUCH;
    s.motion.gyro[0] = -32768;
    s.motion.gyro[2] = 32767;
    s.motion.accel[1] = 4096;
    s.touch[1].active = 1;
    s.touch[1].id = 127;
    s.touch[1].x = 65535;
    s.touch[1].y = 1;

    size = vc_encode_input(&s, 0x01020304u, 65535, buf, sizeof buf);
    CHECK(size == VC_INPUT_DATAGRAM_MAX);
    CHECK(vc_parser_decode(buf, size, &p) == VC_PARSE_OK);
    CHECK(p.token == 0x01020304u);
    CHECK(p.sequence == 65535);
    CHECK(p.state.timestamp_us == 0xFFFFFFFFu);
    CHECK(p.state.buttons == VC_BTN_VALID_MASK);
    CHECK(p.state.left.x == 32767 && p.state.left.y == -32768);
    CHECK(p.state.right.x == 1 && p.state.right.y == -1);
    CHECK(p.state.trigger_left == 255 && p.state.trigger_right == 0);
    CHECK(p.state.flags == s.flags);
    CHECK(memcmp(&p.state.motion, &s.motion, sizeof s.motion) == 0);
    CHECK(p.state.touch[0].active == 0 && p.state.touch[0].id == 0);
    CHECK(p.state.touch[1].active == 1 && p.state.touch[1].id == 127);
    CHECK(p.state.touch[1].x == 65535 && p.state.touch[1].y == 1);

    /* The neutral state is the smallest packet. */
    memset(&s, 0, sizeof s);
    size = vc_encode_input(&s, 1, 0, buf, sizeof buf);
    CHECK(size == VC_PACKET_HEADER_SIZE + VC_PACKET_INPUT_SIZE);
    CHECK(vc_parser_decode(buf, size, &p) == VC_PARSE_OK);
    CHECK(p.state.buttons == 0 && p.state.flags == 0);
}

static void test_edges(void)
{
    vc_input_state_t   s = reference_state(VC_INPUT_HAS_TOUCH);
    vc_parser_packet_t p;
    uint8_t            buf[VC_INPUT_DATAGRAM_MAX];
    size_t             exact = VC_PACKET_HEADER_SIZE + VC_PACKET_INPUT_SIZE + VC_PACKET_TOUCH_SIZE;

    /* Too small a buffer or a NULL argument writes nothing and returns 0. */
    CHECK(vc_encode_input(&s, 1, 1, buf, exact - 1) == 0);
    CHECK(vc_encode_input(&s, 1, 1, buf, exact) == exact);
    CHECK(vc_encode_input(NULL, 1, 1, buf, sizeof buf) == 0);
    CHECK(vc_encode_input(&s, 1, 1, NULL, sizeof buf) == 0);

    /* Touch ids keep their low 7 bits; any non-zero `active` means down. */
    s.touch[0].id = 200;
    s.touch[0].active = 9;
    CHECK(vc_parser_decode(buf, vc_encode_input(&s, 1, 1, buf, sizeof buf), &p) == VC_PARSE_OK);
    CHECK(p.state.touch[0].id == (200 & VC_TOUCH_STATE_ID_MASK));
    CHECK(p.state.touch[0].active == 1);

    /* No validation: the encoder can produce packets the parser rejects. */
    s = reference_state(0);
    s.buttons = UINT32_C(1) << 31;
    CHECK(vc_parser_decode(buf, vc_encode_input(&s, 1, 1, buf, sizeof buf), &p) == VC_PARSE_BAD_FIELD);
    s = reference_state(0);
    s.flags = 0x04;
    CHECK(vc_parser_decode(buf, vc_encode_input(&s, 1, 1, buf, sizeof buf), &p) == VC_PARSE_BAD_FIELD);
}

int main(void)
{
    test_matches_reference();
    test_round_trip();
    test_edges();
    return test_summary();
}
