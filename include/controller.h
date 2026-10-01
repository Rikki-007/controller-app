/*
 * controller.h - core data structures for the virtual controller backend.
 *
 * Defines the controller types, the unified input state the backend keeps
 * per controller, and the packets a mobile client sends (see PLAN.md, s2-s3).
 *
 * Types only: no functions, no OS headers. C99.
 */
#ifndef VC_CONTROLLER_H
#define VC_CONTROLLER_H

#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Controller types                                                         */
/* ------------------------------------------------------------------------ */

/* Layout the client presents. Sent once in HELLO; selects the virtual device
 * and the button mapping. Values are wire values and must not change. */
typedef enum {
    VC_CONTROLLER_NONE   = 0,
    VC_CONTROLLER_XBOX   = 1,
    VC_CONTROLLER_PS5    = 2,
    VC_CONTROLLER_SWITCH = 3,
    VC_CONTROLLER_PS4    = 4
} vc_controller_type_t;

#define VC_CONTROLLER_TYPE_COUNT 5

/* Features a client offers in HELLO and the host grants in WELCOME. */
#define VC_CAP_MOTION  0x01u
#define VC_CAP_TOUCH   0x02u
#define VC_CAP_RUMBLE  0x04u
#define VC_CAP_LED     0x08u

/* ------------------------------------------------------------------------ */
/* Digital buttons                                                          */
/* ------------------------------------------------------------------------ */

/* Positional, not label-based: SOUTH is the bottom face button whatever is
 * printed on it (Xbox A, PlayStation Cross, Switch B).
 *
 *                     Xbox     PS5 / PS4         Switch                     */
#define VC_BTN_SOUTH       (UINT32_C(1) << 0)   /* A        Cross         B       */
#define VC_BTN_EAST        (UINT32_C(1) << 1)   /* B        Circle        A       */
#define VC_BTN_WEST        (UINT32_C(1) << 2)   /* X        Square        Y       */
#define VC_BTN_NORTH       (UINT32_C(1) << 3)   /* Y        Triangle      X       */
#define VC_BTN_L1          (UINT32_C(1) << 4)   /* LB       L1            L       */
#define VC_BTN_R1          (UINT32_C(1) << 5)   /* RB       R1            R       */
#define VC_BTN_L3          (UINT32_C(1) << 6)   /* left stick click               */
#define VC_BTN_R3          (UINT32_C(1) << 7)   /* right stick click              */
#define VC_BTN_START       (UINT32_C(1) << 8)   /* Menu     Options       +       */
#define VC_BTN_SELECT      (UINT32_C(1) << 9)   /* View     Create/Share  -       */
#define VC_BTN_HOME        (UINT32_C(1) << 10)  /* Guide    PS            Home    */
#define VC_BTN_MISC        (UINT32_C(1) << 11)  /* Share    Mic mute      Capture */
#define VC_BTN_DPAD_UP     (UINT32_C(1) << 12)
#define VC_BTN_DPAD_DOWN   (UINT32_C(1) << 13)
#define VC_BTN_DPAD_LEFT   (UINT32_C(1) << 14)
#define VC_BTN_DPAD_RIGHT  (UINT32_C(1) << 15)
#define VC_BTN_TOUCHPAD    (UINT32_C(1) << 16)  /* --       Touchpad click  --    */

/* Bits 17-31 are reserved and must be zero on the wire. */
#define VC_BTN_VALID_MASK  UINT32_C(0x0001FFFF)

/* ------------------------------------------------------------------------ */
/* Unified input state                                                      */
/* ------------------------------------------------------------------------ */

#define VC_STICK_MIN    INT16_MIN
#define VC_STICK_MAX    INT16_MAX
#define VC_TRIGGER_MAX  255
#define VC_TOUCH_POINTS 2

/* vc_input_state_t.flags and vc_packet_input_t.flags */
#define VC_INPUT_HAS_MOTION  0x01u
#define VC_INPUT_HAS_TOUCH   0x02u
#define VC_INPUT_FLAGS_MASK  0x03u

/* Analog stick position. 0,0 is centred; +x is right, +y is up. */
typedef struct {
    int16_t x;
    int16_t y;
} vc_stick_t;

/* Gyro full scale is 2000 deg/s; accel is 1/4096 g per unit. Indices are
 * x, y, z with the controller flat: +x right, +y up, +z toward the player. */
typedef struct {
    int16_t gyro[3];
    int16_t accel[3];
} vc_motion_t;

/* One touchpad contact. x and y are normalised to 0..65535, origin top-left. */
typedef struct {
    uint8_t  active;   /* 0 or 1 */
    uint8_t  id;       /* 0..127, stable while the finger stays down */
    uint16_t x;
    uint16_t y;
} vc_touch_point_t;

/* Layout-neutral snapshot of one controller. This is what the HID drivers
 * consume; an all-zero value is the neutral state (nothing pressed, sticks
 * centred, triggers released). */
typedef struct {
    uint32_t         buttons;                 /* VC_BTN_* bitmask */
    vc_stick_t       left;
    vc_stick_t       right;
    uint8_t          trigger_left;            /* 0..VC_TRIGGER_MAX */
    uint8_t          trigger_right;
    uint8_t          flags;                   /* VC_INPUT_HAS_* */
    vc_motion_t      motion;                  /* valid if VC_INPUT_HAS_MOTION */
    vc_touch_point_t touch[VC_TOUCH_POINTS];  /* valid if VC_INPUT_HAS_TOUCH */
    uint32_t         timestamp_us;            /* client monotonic clock */
} vc_input_state_t;

/* ------------------------------------------------------------------------ */
/* Network packets (mobile client -> host)                                  */
/* ------------------------------------------------------------------------ */

/*
 * Every packet is a vc_packet_header_t followed by header.length payload
 * bytes. All multi-byte fields are little-endian on the wire.
 *
 * The structs below have the exact wire layout: fields are ordered so no
 * compiler inserts padding, and the size checks at the end of this file fail
 * the build if that ever stops being true. They do not fix byte order, so
 * portable code should read fields through the protocol decoder rather than
 * casting a receive buffer.
 */

#define VC_PROTOCOL_MAGIC    0x4356u   /* bytes 'V','C' on the wire */
#define VC_PROTOCOL_VERSION  1
#define VC_DEFAULT_PORT      28960
#define VC_CLIENT_NAME_LEN   32

typedef enum {
    /* TCP control channel */
    VC_MSG_HELLO    = 0x01,   /* client -> host */
    VC_MSG_WELCOME  = 0x02,   /* host -> client */
    VC_MSG_PING     = 0x03,   /* either direction */
    VC_MSG_PONG     = 0x04,   /* either direction */
    VC_MSG_FEEDBACK = 0x05,   /* host -> client */
    VC_MSG_ERROR    = 0x06,   /* host -> client */
    VC_MSG_BYE      = 0x07,   /* either direction, no payload */
    /* UDP input channel */
    VC_MSG_INPUT    = 0x10    /* client -> host */
} vc_message_type_t;

/* Common header, TCP and UDP. */
typedef struct {
    uint16_t magic;      /* VC_PROTOCOL_MAGIC */
    uint8_t  version;    /* VC_PROTOCOL_VERSION */
    uint8_t  type;       /* vc_message_type_t */
    uint16_t sequence;   /* per-sender counter, wraps */
    uint16_t length;     /* payload bytes after this header */
} vc_packet_header_t;

/* VC_MSG_HELLO: first message on the TCP connection. */
typedef struct {
    uint8_t controller_type;            /* vc_controller_type_t */
    uint8_t caps;                       /* VC_CAP_* the client can provide */
    char    name[VC_CLIENT_NAME_LEN];   /* UTF-8, NUL-padded, need not be terminated */
} vc_packet_hello_t;

/* VC_MSG_PING and VC_MSG_PONG: the receiver echoes the value back. */
typedef struct {
    uint32_t echo;
} vc_packet_ping_t;

/* VC_MSG_INPUT: fixed part of every input datagram. A full snapshot, never a
 * delta. Followed by vc_packet_motion_t if VC_INPUT_HAS_MOTION is set, then
 * vc_packet_touch_t if VC_INPUT_HAS_TOUCH is set. */
typedef struct {
    uint32_t token;          /* session token from WELCOME */
    uint32_t timestamp_us;   /* client monotonic clock */
    uint32_t buttons;        /* VC_BTN_* bitmask */
    int16_t  left_x;
    int16_t  left_y;
    int16_t  right_x;
    int16_t  right_y;
    uint8_t  trigger_left;
    uint8_t  trigger_right;
    uint8_t  flags;          /* VC_INPUT_HAS_* */
    uint8_t  reserved;       /* must be 0 */
} vc_packet_input_t;

/* Optional motion block of VC_MSG_INPUT. Same units as vc_motion_t. */
typedef struct {
    int16_t gyro[3];
    int16_t accel[3];
} vc_packet_motion_t;

#define VC_TOUCH_STATE_ACTIVE   0x80u   /* finger is down */
#define VC_TOUCH_STATE_ID_MASK  0x7Fu

typedef struct {
    uint8_t  state;      /* VC_TOUCH_STATE_ACTIVE | contact id */
    uint8_t  reserved;   /* must be 0 */
    uint16_t x;
    uint16_t y;
} vc_packet_touch_point_t;

/* Optional touch block of VC_MSG_INPUT. */
typedef struct {
    vc_packet_touch_point_t points[VC_TOUCH_POINTS];
} vc_packet_touch_t;

/* Wire sizes in bytes. */
#define VC_PACKET_HEADER_SIZE  8
#define VC_PACKET_HELLO_SIZE   34
#define VC_PACKET_PING_SIZE    4
#define VC_PACKET_INPUT_SIZE   24
#define VC_PACKET_MOTION_SIZE  12
#define VC_PACKET_TOUCH_SIZE   12

/* Largest input datagram: header + fixed part + motion + touch. */
#define VC_INPUT_DATAGRAM_MAX \
    (VC_PACKET_HEADER_SIZE + VC_PACKET_INPUT_SIZE + \
     VC_PACKET_MOTION_SIZE + VC_PACKET_TOUCH_SIZE)

/* C99 has no static assert; a negative array size is a compile error. */
#define VC_STATIC_ASSERT(cond, name) \
    typedef char vc_static_assert_##name[(cond) ? 1 : -1]

VC_STATIC_ASSERT(sizeof(vc_packet_header_t) == VC_PACKET_HEADER_SIZE, header_size);
VC_STATIC_ASSERT(sizeof(vc_packet_hello_t)  == VC_PACKET_HELLO_SIZE,  hello_size);
VC_STATIC_ASSERT(sizeof(vc_packet_ping_t)   == VC_PACKET_PING_SIZE,   ping_size);
VC_STATIC_ASSERT(sizeof(vc_packet_input_t)  == VC_PACKET_INPUT_SIZE,  input_size);
VC_STATIC_ASSERT(sizeof(vc_packet_motion_t) == VC_PACKET_MOTION_SIZE, motion_size);
VC_STATIC_ASSERT(sizeof(vc_packet_touch_t)  == VC_PACKET_TOUCH_SIZE,  touch_size);

#endif /* VC_CONTROLLER_H */
