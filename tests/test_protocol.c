/*
 * test_protocol.c - TCP control messages: encoding, decoding and stream
 * reassembly.
 *
 * Build:  cc -std=c99 -Iinclude -Itests src/protocol.c tests/test_protocol.c
 */
#include "protocol.h"

#define VC_TEST_NO_PACKETS
#include "test_util.h"

/* Encodes msg, reads it back in one chunk; returns the read result. */
static vc_proto_result_t round_trip(const vc_proto_msg_t *msg, vc_proto_msg_t *out)
{
    vc_proto_reader_t reader;
    uint8_t           buf[VC_PROTO_MAX_FRAME];
    const uint8_t    *data = buf;
    size_t            size = vc_proto_encode(msg, buf, sizeof buf);
    vc_proto_result_t result;

    CHECK(size > 0);
    vc_proto_reader_init(&reader);
    result = vc_proto_read(&reader, &data, &size, out);
    CHECK(size == 0);
    return result;
}

static void test_round_trips(void)
{
    vc_proto_msg_t in;
    vc_proto_msg_t out;

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_HELLO;
    in.sequence = 7;
    in.controller_type = VC_CONTROLLER_PS5;
    in.caps = VC_CAP_MOTION | VC_CAP_TOUCH;
    strcpy(in.name, "Pixel 9");
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_HELLO && out.sequence == 7);
    CHECK(out.controller_type == VC_CONTROLLER_PS5);
    CHECK(out.caps == (VC_CAP_MOTION | VC_CAP_TOUCH));
    CHECK(strcmp(out.name, "Pixel 9") == 0);

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_WELCOME;
    in.token = 0xCAFEF00Du;
    in.slot = 3;
    in.caps = VC_CAP_MOTION;
    in.timeout_ms = 250;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_WELCOME && out.token == 0xCAFEF00Du);
    CHECK(out.slot == 3 && out.caps == VC_CAP_MOTION && out.timeout_ms == 250);

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_PING;
    in.echo = 0xFFFFFFFFu;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_PING && out.echo == 0xFFFFFFFFu);
    in.type = VC_MSG_PONG;
    in.echo = 1;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_PONG && out.echo == 1);

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_FEEDBACK;
    in.feedback.rumble_low = 10;
    in.feedback.rumble_high = 20;
    in.feedback.led[0] = 1;
    in.feedback.led[1] = 2;
    in.feedback.led[2] = 3;
    in.feedback.player = 4;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(memcmp(&out.feedback, &in.feedback, sizeof in.feedback) == 0);

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_ERROR;
    in.error = VC_ERROR_NO_SLOT;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_ERROR && out.error == VC_ERROR_NO_SLOT);

    memset(&in, 0, sizeof in);
    in.type = VC_MSG_BYE;
    in.sequence = 65535;
    CHECK(round_trip(&in, &out) == VC_PROTO_OK);
    CHECK(out.type == VC_MSG_BYE && out.sequence == 65535);
}

/* The exact bytes, so the wire format cannot drift unnoticed. */
static void test_wire_bytes(void)
{
    static const uint8_t welcome[] = {
        'V', 'C', 1, VC_MSG_WELCOME, 0x02, 0x01, 8, 0,    /* seq 0x0102, len 8 */
        0x0D, 0xF0, 0xFE, 0xCA,                            /* token 0xCAFEF00D */
        2, VC_CAP_TOUCH, 0xF4, 0x01                        /* slot, caps, 500 ms */
    };
    static const uint8_t bye[] = { 'V', 'C', 1, VC_MSG_BYE, 0, 0, 0, 0 };
    vc_proto_msg_t msg;
    uint8_t        buf[VC_PROTO_MAX_FRAME];

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_WELCOME;
    msg.sequence = 0x0102;
    msg.token = 0xCAFEF00Du;
    msg.slot = 2;
    msg.caps = VC_CAP_TOUCH;
    msg.timeout_ms = 500;
    CHECK(vc_proto_encode(&msg, buf, sizeof buf) == sizeof welcome);
    CHECK(memcmp(buf, welcome, sizeof welcome) == 0);

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_BYE;
    CHECK(vc_proto_encode(&msg, buf, sizeof buf) == sizeof bye);
    CHECK(memcmp(buf, bye, sizeof bye) == 0);

    /* HELLO is header + 34 bytes; the name is NUL-padded. */
    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_HELLO;
    msg.controller_type = VC_CONTROLLER_SWITCH;
    strcpy(msg.name, "ab");
    CHECK(vc_proto_encode(&msg, buf, sizeof buf) == VC_PACKET_HEADER_SIZE + VC_PACKET_HELLO_SIZE);
    CHECK(buf[8] == VC_CONTROLLER_SWITCH && buf[10] == 'a' && buf[11] == 'b');
    CHECK(buf[12] == 0 && buf[VC_PROTO_MAX_FRAME - 1] == 0);
}

static void test_names(void)
{
    vc_proto_msg_t    in;
    vc_proto_msg_t    out;
    vc_proto_reader_t reader;
    uint8_t           buf[VC_PROTO_MAX_FRAME];
    const uint8_t    *data;
    size_t            size;
    size_t            i;

    /* A name filling all 32 bytes has no NUL on the wire; decoding still
     * terminates it. */
    memset(&in, 0, sizeof in);
    in.type = VC_MSG_HELLO;
    for (i = 0; i < VC_CLIENT_NAME_LEN; i++) {
        in.name[i] = (char)('a' + (int)(i % 26));
    }
    in.name[VC_CLIENT_NAME_LEN] = '\0';
    size = vc_proto_encode(&in, buf, sizeof buf);
    CHECK(buf[VC_PROTO_MAX_FRAME - 1] == (uint8_t)in.name[VC_CLIENT_NAME_LEN - 1]);
    data = buf;
    vc_proto_reader_init(&reader);
    CHECK(vc_proto_read(&reader, &data, &size, &out) == VC_PROTO_OK);
    CHECK(strlen(out.name) == VC_CLIENT_NAME_LEN);
    CHECK(strcmp(out.name, in.name) == 0);

    /* Bytes after an embedded NUL are ignored. */
    buf[VC_PACKET_HEADER_SIZE + 2 + 3] = 0;
    data = buf;
    size = VC_PROTO_MAX_FRAME;
    CHECK(vc_proto_read(&reader, &data, &size, &out) == VC_PROTO_OK);
    CHECK(strcmp(out.name, "abc") == 0);
}

static void test_stream(void)
{
    vc_proto_reader_t reader;
    vc_proto_msg_t    msg;
    vc_proto_msg_t    out[4];
    uint8_t           stream[3 * VC_PROTO_MAX_FRAME];
    size_t            len = 0;
    size_t            i;
    int               got = 0;

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_HELLO;
    msg.controller_type = VC_CONTROLLER_XBOX;
    strcpy(msg.name, "one");
    len += vc_proto_encode(&msg, stream + len, sizeof stream - len);
    msg.type = VC_MSG_PING;
    msg.echo = 42;
    len += vc_proto_encode(&msg, stream + len, sizeof stream - len);
    msg.type = VC_MSG_BYE;
    len += vc_proto_encode(&msg, stream + len, sizeof stream - len);
    CHECK(len == (8 + 34) + (8 + 4) + 8);

    /* One byte at a time: every frame comes out once, in order. */
    vc_proto_reader_init(&reader);
    for (i = 0; i < len; i++) {
        const uint8_t    *data = stream + i;
        size_t            size = 1;
        vc_proto_result_t result;

        while ((result = vc_proto_read(&reader, &data, &size, &out[got])) == VC_PROTO_OK) {
            got++;
        }
        CHECK(result == VC_PROTO_NEED_MORE);
    }
    CHECK(got == 3);
    CHECK(out[0].type == VC_MSG_HELLO && strcmp(out[0].name, "one") == 0);
    CHECK(out[1].type == VC_MSG_PING && out[1].echo == 42);
    CHECK(out[2].type == VC_MSG_BYE);

    /* All at once: same result, three OKs then NEED_MORE. */
    {
        const uint8_t *data = stream;
        size_t         size = len;

        vc_proto_reader_init(&reader);
        got = 0;
        while (vc_proto_read(&reader, &data, &size, &out[got]) == VC_PROTO_OK) {
            got++;
        }
        CHECK(got == 3 && size == 0);
    }

    /* Nothing in, nothing out. */
    {
        const uint8_t *data = NULL;
        size_t         size = 0;
        CHECK(vc_proto_read(&reader, &data, &size, &out[0]) == VC_PROTO_NEED_MORE);
    }
}

/* Feeds `bytes` to a fresh reader; returns the first non-OK result. */
static vc_proto_result_t feed(const uint8_t *bytes, size_t size)
{
    vc_proto_reader_t reader;
    vc_proto_msg_t    msg;
    vc_proto_result_t result;

    vc_proto_reader_init(&reader);
    while ((result = vc_proto_read(&reader, &bytes, &size, &msg)) == VC_PROTO_OK) {
    }
    return result;
}

static void test_errors(void)
{
    uint8_t        buf[64];
    vc_proto_msg_t msg;
    size_t         size;

    memset(&msg, 0, sizeof msg);
    msg.type = VC_MSG_PING;
    size = vc_proto_encode(&msg, buf, sizeof buf);

    /* Detected as soon as the header is in, before any payload. */
    buf[0] = 'X';
    CHECK(feed(buf, VC_PACKET_HEADER_SIZE) == VC_PROTO_BAD_MAGIC);
    buf[0] = 'V';
    buf[2] = 2;
    CHECK(feed(buf, VC_PACKET_HEADER_SIZE) == VC_PROTO_BAD_VERSION);
    buf[2] = VC_PROTOCOL_VERSION;
    buf[3] = VC_MSG_INPUT;     /* belongs on UDP */
    CHECK(feed(buf, VC_PACKET_HEADER_SIZE) == VC_PROTO_BAD_TYPE);
    buf[3] = 0x42;
    CHECK(feed(buf, VC_PACKET_HEADER_SIZE) == VC_PROTO_BAD_TYPE);
    buf[3] = VC_MSG_PING;
    buf[6] = 5;                /* PING is 4 bytes */
    CHECK(feed(buf, VC_PACKET_HEADER_SIZE) == VC_PROTO_BAD_LENGTH);
    buf[6] = 4;
    CHECK(feed(buf, size) == VC_PROTO_NEED_MORE);
    CHECK(feed(buf, 3) == VC_PROTO_NEED_MORE);

    /* Encoding refuses what it cannot represent. */
    msg.type = VC_MSG_INPUT;
    CHECK(vc_proto_encode(&msg, buf, sizeof buf) == 0);
    msg.type = VC_MSG_HELLO;
    CHECK(vc_proto_encode(&msg, buf, VC_PROTO_MAX_FRAME - 1) == 0);
    CHECK(vc_proto_encode(NULL, buf, sizeof buf) == 0);
    CHECK(vc_proto_encode(&msg, NULL, sizeof buf) == 0);

    CHECK(strcmp(vc_proto_type_name(VC_MSG_WELCOME), "WELCOME") == 0);
    CHECK(strcmp(vc_proto_type_name(0x99), "?") == 0);
    CHECK(vc_proto_strerror(VC_PROTO_BAD_MAGIC) != NULL);
}

int main(void)
{
    test_round_trips();
    test_wire_bytes();
    test_names();
    test_stream();
    test_errors();
    return test_summary();
}
