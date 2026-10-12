# Virtual Controller Backend — Plan

Status: **draft, awaiting approval**. No implementation exists yet.

## 0. Scope and assumptions

- This repo is the **host-side backend** (`vcd`, a C99 daemon). It receives controller input over the network from the app (client) and exposes it to the OS as a virtual gamepad. The client app is out of scope here; it only needs to speak the protocol in §2.
- Up to 4 simultaneous controllers (slots). LAN use; no encryption in v1.
- Toolchain found on this machine: MSYS2 UCRT64 `gcc 16.1` + `mingw32-make`. No CMake, no ViGEm installed. The Makefile targets GNU make on Windows (MinGW) and Linux.
- Symbol prefix `vc_`. No dynamic allocation on the input hot path.

## 1. Directory structure

```
controller-app/
├── Makefile                 # all / test / clean; OS auto-detect; BACKEND=stub|vigem|uinput (written)
├── PLAN.md
├── include/
│   ├── controller.h         # controller types, button bits, vc_input_state_t, wire packet structs (written)
│   ├── network.h            # UDP input listener (written)
│   ├── parser.h             # INPUT decode, token/sequence checks, controller profiles, stream reassembly (written)
│   ├── encoder.h            # INPUT encode, the inverse of the parser; used by vc_send and the tests (written)
│   ├── protocol.h           # TCP control messages: encode/decode, stream reassembly (written)
│   ├── hid_output.h         # report translation + virtual gamepad wrapper (written; replaces the §4 vtable)
│   ├── session.h            # handshake, issued tokens, slots, keepalive and timeouts (written)
│   └── vc_log.h
├── src/
│   ├── main.c               # options, signal handling, poll loop, implicit slots keyed by token, timeouts (written)
│   ├── session.c            # pure (no sockets, no clock): HELLO/WELCOME/PING/BYE, slots, timeouts (written)
│   ├── network.c            # UDP socket (Winsock/POSIX), hands decoded packets to a callback (written)
│   ├── parser.c             # pure: decode + validate, token/sequence checks, controller profiles (written)
│   ├── encoder.c            # pure: vc_input_state_t -> INPUT datagram (written)
│   ├── protocol.c           # pure: TCP control messages, byte-wise, validated as soon as the header is in (written)
│   ├── log.c
│   ├── hid_output.c         # translation to Xbox 360 / DS4 / evdev; stub, ViGEm and uinput backends by compile flag (written)
│   └── platform/            # clock_win32.c, clock_posix.c
├── tests/                   # test_parser, test_network, test_hid_output, test_encoder, test_loopback, test_protocol, test_session, e2e.sh, test_util.h (written)
├── tools/vc_send.c          # CLI test client: scripted input, many clients, bad packets (written)
├── include/vc_client.h, src/vc_client.c  # phone-side engine shared by iOS and Android (written; docs/MOBILE.md)
├── Package.swift, mobile/   # iOS Swift package and Android library around vc_client (written; docs/MOBILE.md)
├── scripts/test.ps1         # Windows test runner; falls back to WSL when Smart App Control blocks (written)
├── .github/workflows/ci.yml # Linux + Windows CI: unit tests, e2e, backend compile checks (written)
├── docs/BUILD.md            # compile flags and driver setup per backend (written)
└── third_party/ViGEmClient/ # cloned at build time, Windows only (not vendored yet)
```

Dependency rule: `parser`, `protocol`, `state` and the translation half of `hid_output` are pure (no sockets, no OS calls) so they are fully unit-testable. OS-specific code lives only in `network.c` (Winsock/POSIX behind `#ifdef`) and `hid_output.c` (backend chosen by `-DVC_HID_BACKEND_*`).

## 2. Network protocol

Two channels per client, same 8-byte header on both. All integers **little-endian**, encoded byte-by-byte (never by casting structs).

| Channel | Carries | Why |
|---|---|---|
| TCP (default port 28960) | handshake, keepalive, feedback (rumble/LED), teardown | must be reliable and ordered |
| UDP (same port) | input state snapshots | latest-wins; a retransmitted stale input is worse than a lost one |

### 2.1 Header (8 bytes)

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | magic | bytes `'V','C'` |
| 2 | 1 | version | `1` |
| 3 | 1 | type | see below |
| 4 | 2 | seq | per-sender counter; UDP compares with serial-number arithmetic and drops stale/duplicate |
| 6 | 2 | len | payload bytes following the header (also the TCP frame length) |

### 2.2 TCP messages

| Type | Dir | Payload |
|---|---|---|
| `HELLO` 0x01 | C→H | controller type u8, caps u8 (motion, touch, rumble, LED), name[32] |
| `WELCOME` 0x02 | H→C | token u32, slot u8, granted caps u8, input timeout ms u16 |
| `PING`/`PONG` 0x03/0x04 | both | echo u32 (keepalive + RTT), every 1 s |
| `FEEDBACK` 0x05 | H→C | rumble_low u8, rumble_high u8, led r,g,b u8, player u8 |
| `ERROR` 0x06 | H→C | code u8: 1 bad version, 2 no free slot, 3 type unsupported, 4 protocol error, 5 timeout, 6 driver error |
| `BYE` 0x07 | both | — |

Lifecycle: `HELLO` → backend creates the virtual device → `WELCOME` (token random, never 0). The client must then send UDP input from the same IP address as its TCP connection. Device destroyed and slot freed on: `BYE`, TCP close, 3 unanswered host pings, 10 s without input, or a protocol error; all but the first two are announced with `ERROR` first. 250 ms without input only releases the controls. A connection without `HELLO` after 5 s gets `ERROR` timeout. As built in `session.h`; `--type` on the daemon is gone.

### 2.3 UDP `INPUT` (type 0x10)

**One wire format for all three layouts.** Layout is fixed at `HELLO` and only selects which virtual device and mapping the host uses. Every packet is a full snapshot (no deltas), so loss needs no recovery.

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | token | from `WELCOME`; binds datagram to a session, rejects strays |
| 4 | 4 | timestamp_us | client monotonic clock; used for motion timing |
| 8 | 4 | buttons | canonical bitmask (§2.4) |
| 12 | 8 | lx, ly, rx, ry | i16 each; +x right, **+y up** |
| 20 | 2 | lt, rt | u8 each, 0–255 |
| 22 | 1 | flags | bit0 `HAS_MOTION`, bit1 `HAS_TOUCH` |
| 23 | 1 | reserved | must be 0 |
| 24 | 12 | *motion* (if flag) | gyro x,y,z i16 (2000 dps full scale); accel x,y,z i16 (1/4096 g) |
| +0 | 12 | *touch* (if flag) | 2 × { state u8 (bit7 active, bits0–6 id), reserved u8, x u16, y u16 normalised } |

Size: 32–56 bytes including header. Every field sits on its natural alignment, so the structs in `controller.h` match the wire layout with no padding (checked at compile time). Client sends at a fixed rate (target 120 Hz) while connected. If no valid `INPUT` arrives within the timeout (default 250 ms), the host **neutralises** the slot (all buttons released, sticks centred) so a dropped connection never leaves a button held.

### 2.4 Canonical button bits

Positional, not label-based, so Nintendo's swapped A/B and X/Y need no special casing on the wire.

| Bit | Canonical | Xbox | PS5 / PS4 | Switch |
|---|---|---|---|---|
| 0 | SOUTH | A | Cross | B |
| 1 | EAST | B | Circle | A |
| 2 | WEST | X | Square | Y |
| 3 | NORTH | Y | Triangle | X |
| 4 / 5 | L1 / R1 | LB / RB | L1 / R1 | L / R |
| 6 / 7 | L3 / R3 | stick clicks | L3 / R3 | stick clicks |
| 8 | START | Menu | Options | + |
| 9 | SELECT | View | Create | − |
| 10 | HOME | Guide | PS | Home |
| 11 | MISC | Share | Mic mute | Capture |
| 12–15 | DPAD U/D/L/R | ← | ← | ← |
| 16 | TOUCHPAD | — | Touchpad click | — |
| 17–31 | reserved | | | |

Triggers are always analog on the wire; Switch ZL/ZR send 0 or 255. Motion is ignored for Xbox targets.

## 3. Internal state representation

`vc_controller_type_t` (Xbox 1, PS5 2, Switch 3, PS4 4), `vc_stick_t`, `vc_motion_t`, `vc_touch_point_t` and the layout-neutral snapshot `vc_input_state_t` are defined in `include/controller.h`, which is the source of truth. An all-zero `vc_input_state_t` is the neutral state. **As built**, `include/session.h` holds the per-connection and per-slot state (`vc_session_conn_t`, `vc_session_slot_t`: token, type, granted caps, parser, device, learned UDP peer, ping state) and `vc_packet_feedback_t` lives in `controller.h`. The original sketch:

```c
typedef struct { uint8_t rumble_low, rumble_high, led[3], player; } vc_feedback_t;

/* One per connected controller; fixed array of VC_MAX_SLOTS (4). */
typedef struct {
    bool                 in_use;
    vc_controller_type_t type;
    uint8_t              caps;
    uint32_t             token;
    uint16_t             last_seq;
    uint64_t             last_input_ms;   /* drives the neutralise timeout */
    bool                 neutral;
    vc_input_state_t     state;           /* last accepted snapshot */
    vc_hid_device_t     *dev;             /* opaque, owned by the driver */
    vc_net_conn_t        tcp;
    vc_net_addr_t        udp_peer;
} vc_slot_t;
```

Data flow per datagram: `network.c` receive → `parser.c` decode + validate → `session.c` finds the slot by token, checks the source IP, runs the slot's parser (sequence, profile) → `vc_hid_submit()`. Control bytes: `network.c` → `session.c` → `protocol.c` reader → handler; replies go back through `vc_net_send()`. Single thread, so no locking on state. Driver feedback callbacks that fire on another thread (ViGEm) write into a small per-slot mailbox that the loop drains.

## 4. Virtual HID driver abstraction

**As built:** `include/hid_output.h` replaces the vtable below with plain functions (`vc_hid_init`, `vc_hid_create`, `vc_hid_submit`, `vc_hid_destroy`) and one backend selected at compile time; the stub backend is what the tests use. The vtable is kept here as the original design; rumble/LED feedback (`poll_feedback`) is not implemented yet.

```c
typedef struct vc_hid_device vc_hid_device_t;   /* opaque */

typedef struct {
    const char *name;
    int      (*init)(void);
    void     (*shutdown)(void);
    uint8_t  (*caps)(vc_controller_type_t type);   /* 0 = type not supported */
    int      (*create)(vc_controller_type_t type, uint8_t slot, vc_hid_device_t **out);
    int      (*submit)(vc_hid_device_t *dev, const vc_input_state_t *state);
    bool     (*poll_feedback)(vc_hid_device_t *dev, vc_feedback_t *out);
    void     (*destroy)(vc_hid_device_t *dev);
} vc_hid_driver_t;

const vc_hid_driver_t *vc_hid_default_driver(void);   /* hid_registry.c */
```

The shared, pure conversions (Y-axis flip, d-pad bits → hat, trigger buttons, motion rescale, touch scaling) are the `vc_hid_to_xusb` / `vc_hid_to_ds4` / `vc_hid_to_evdev` functions in `hid_output.c`, so no separate `mapping.c` is needed.

| Layout | Windows — `hid_vigem` | Linux — `hid_uinput` | Any — `hid_null` |
|---|---|---|---|
| Xbox | Xbox 360 (XInput) | evdev pad, Xbox 360 VID/PID | logs / records |
| PS5 | **DualShock 4** (gyro + touchpad kept) | evdev pad + separate motion node, DualSense VID/PID | logs / records |
| PS4 | DualShock 4 (native) | evdev pad + separate motion node, DualShock 4 VID/PID | logs / records |
| Switch | **DualShock 4** (keeps gyro) or Xbox 360 | evdev pad + motion node, Pro Controller VID/PID | logs / records |

Known limits, stated up front:

- **Windows cannot present a real DualSense or Switch Pro Controller.** ViGEmBus only emulates Xbox 360 and DualShock 4, so adaptive triggers and DualSense haptics are not reachable. Games will show DS4 or Xbox glyphs.
- **ViGEmBus is retired upstream** (no new releases) but remains the only signed virtual-gamepad bus driver that works without writing our own kernel driver. Users must install it separately.
- **ViGEmClient is C++.** Building it with MinGW `g++` and linking it into the C backend works (verified, see `docs/BUILD.md`). Not yet run against an installed ViGEmBus.
- **Linux v1 uses uinput**, which gives correct buttons/axes/motion but not byte-exact HID reports. A `hid_uhid` backend can be added later behind the same vtable if exact device identity matters.
- **macOS** has no virtual HID path without a DriverKit entitlement; it builds with `hid_null` only.

## 5. Build and test

- `make` → `build/vcd`; `make test` → builds and runs every `tests/test_*.c`; `make DRIVER=null` forces the null backend. Flags: `-std=c99 -pedantic -Wall -Wextra -Werror -O2`.
- Tests use a tiny in-repo assert header (no external framework).
  - `test_protocol`: round-trip, truncated/oversized/bad-magic/bad-version packets, flag/length mismatches.
  - `test_state`: seq wraparound, stale drop, timeout neutralise.
  - `test_hid_output`: button tables, axis flips, hat conversion, motion/touch scaling, stub device lifecycle.
  - `test_loopback`: real sockets on localhost + `hid_null`, driven by the `vc_send` code path.

## 6. Milestones

1. Done: Makefile, `controller.h`, input packet parser, unit tests.
2. Written, not yet compiled (Smart App Control blocks the local compiler; CI is the first run): TCP control channel in `network.c`, `protocol.c`, `session.c` (handshake, per-client controller type, issued tokens, keepalive, timeouts), Windows `SIO_UDP_CONNRESET` off, `vc_send` speaking the handshake, unit tests and `tests/e2e.sh` covering every way a session ends.
3. ViGEm backend: basic Xbox 360 and DS4 reports are written in `hid_output.c`; still to do: run against ViGEmBus, DS4 motion/touch via `DS4_REPORT_EX`.
4. uinput backend: written in `hid_output.c`, never compiled; still to do: build and run on Linux, motion/touchpad devices.
5. Feedback path (rumble/LED) end to end.

## 7. Decisions needed from you

1. **Switch on Windows**: map to DualShock 4 (keeps gyro, PlayStation glyphs) or Xbox 360 (no gyro, widest compatibility)? Plan assumes DS4.
2. **Pairing/security**: v1 as planned accepts any LAN client. Add a pairing PIN to `HELLO` now, or defer?
3. **Discovery**: client enters host IP manually in v1; add a UDP broadcast beacon now or later?
4. **Linux in v1**, or Windows-only first with Linux as a follow-up?
