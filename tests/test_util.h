/*
 * test_util.h - check macro and packet builder shared by the tests.
 * Include from exactly one .c file per test program.
 */
#ifndef VC_TEST_UTIL_H
#define VC_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

#include "controller.h"

static int g_checks;
static int g_failures;

#define CHECK(cond) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* Prints the summary line and returns the process exit code. */
static int test_summary(void)
{
    printf("%s: %d checks, %d failed\n", g_failures ? "FAILED" : "ok",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}

/* Packet builder. Tests that never build packets define VC_TEST_NO_PACKETS
 * before including this file, so the helpers are not left unused. */
#ifndef VC_TEST_NO_PACKETS

#define TEST_TOKEN    0xDEADBEEFu
#define TEST_BUTTONS  (VC_BTN_SOUTH | VC_BTN_MISC | VC_BTN_TOUCHPAD)

static void put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (unsigned)(v & 0xFFFF));
    put_u16(p + 2, (unsigned)(v >> 16));
}

/* Writes a valid INPUT packet with recognisable values; returns its size
 * (32 bytes, plus 12 for motion and 12 for touch). */
static size_t build_input(uint8_t *buf, uint8_t flags, unsigned sequence)
{
    uint8_t *p = buf + VC_PACKET_HEADER_SIZE;
    size_t   length = VC_PACKET_INPUT_SIZE;
    int      i;

    put_u32(p + 0, TEST_TOKEN);
    put_u32(p + 4, 123456789u);     /* timestamp */
    put_u32(p + 8, TEST_BUTTONS);
    put_u16(p + 12, 0x8000);        /* left x  = -32768 */
    put_u16(p + 14, 0x7FFF);        /* left y  =  32767 */
    put_u16(p + 16, 0xFFFF);        /* right x = -1 */
    put_u16(p + 18, 0x0100);        /* right y =  256 */
    p[20] = 200;                    /* left trigger */
    p[21] = 7;                      /* right trigger */
    p[22] = flags;
    p[23] = 0;
    p += VC_PACKET_INPUT_SIZE;

    if (flags & VC_INPUT_HAS_MOTION) {
        for (i = 0; i < 6; i++) {
            put_u16(p + 2 * i, (unsigned)(0xFFFF - i));   /* -1, -2, ... -6 */
        }
        p += VC_PACKET_MOTION_SIZE;
        length += VC_PACKET_MOTION_SIZE;
    }
    if (flags & VC_INPUT_HAS_TOUCH) {
        p[0] = VC_TOUCH_STATE_ACTIVE | 5;
        p[1] = 0;
        put_u16(p + 2, 1000);
        put_u16(p + 4, 65535);
        memset(p + 6, 0, 6);                              /* second point idle */
        length += VC_PACKET_TOUCH_SIZE;
    }

    put_u16(buf + 0, VC_PROTOCOL_MAGIC);
    buf[2] = VC_PROTOCOL_VERSION;
    buf[3] = VC_MSG_INPUT;
    put_u16(buf + 4, sequence);
    put_u16(buf + 6, (unsigned)length);
    return VC_PACKET_HEADER_SIZE + length;
}

#endif /* VC_TEST_NO_PACKETS */

#endif /* VC_TEST_UTIL_H */
