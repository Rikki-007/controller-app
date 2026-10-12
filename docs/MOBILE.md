# Mobile client architecture

How the iOS and Android apps send controller input to `vcd`. It covers the project structure, the sender loop, and what each platform layer is responsible for. The wire format is in [PLAN.md §2](../PLAN.md#2-network-protocol); the host side is in [BUILD.md](BUILD.md).

## 1. Structure: shared C engine, thin native shells

The protocol is implemented once, in C, and compiled into both apps. Each app adds a native layer: Swift on iOS, Kotlin on Android. That layer owns the sender thread and its priority, the platform network settings, and the UI. No JavaScript or Dart runtime sits anywhere on the input path.

```
controller-app/
├── include/                      shared C core: public headers (layout.h: the on-screen gamepad)
│   ├── controller.h              wire format, button bits (source of truth)
│   ├── encoder.h, protocol.h     INPUT and control-message (de)serialisation
│   ├── vc_client.h               the phone-side engine
│   └── module.modulemap          what Swift sees: controller.h + vc_client.h
├── src/
│   ├── encoder.c, protocol.c     shared with vcd and its tests
│   ├── vc_client.c               handshake, keepalive, triple buffer, send loop
│   ├── layout.c                  on-screen gamepad: hit-testing, sticks, deadzones, labels
│   └── ...                       vcd only (network.c, session.c, hid_output.c, main.c)
├── Package.swift                 iOS: SwiftPM package (must sit at the root to reach src/)
├── mobile/
│   ├── ios/Sources/VirtualController/
│   │   ├── ControllerLink.swift  sender thread, input API, events on main
│   │   ├── GamepadView.swift     touch view, 120 Hz CADisplayLink, profile labels
│   │   └── Buttons.swift         Buttons, ControllerType, Capabilities
│   └── android/                  Gradle build (settings, plugins)
│       └── vccore/               Android library module
│           ├── src/main/cpp/     CMakeLists.txt (compiles ../../src/*.c), vccore_jni.c
│           └── src/main/kotlin/dev/vcd/controller/
│               ├── ControllerLink.kt   sender thread, Wi-Fi lock, input API
│               ├── GamepadView.kt      touch view, unbuffered dispatch, profile labels
│               ├── LatencyTuning.kt    Sustained Performance, refresh rate, screen on
│               ├── NativeCore.kt       JNI surface
│               └── Buttons.kt
└── tests/test_client.c           the engine against the real host stack (Linux, macOS CI)
```

The app projects (UI, Xcode app target, Android `app` module) are not created yet. They depend on these libraries: an Xcode local package reference to the repository root on iOS, and `implementation(project(":vccore"))` on Android.

### Why this split, and not the alternatives

| Option | Verdict |
|---|---|
| **Shared C engine + Swift/Kotlin shells** (chosen) | One implementation of the handshake, keepalive, sequencing and encoding: the same `encoder.c` and `protocol.c` `vcd` is built from, so the two sides cannot disagree. The engine is tested on CI against the real host (`test_client`). Swift calls C directly at zero cost; Kotlin crosses JNI only with primitives, well under 1 µs per call. |
| Kotlin Multiplatform | Would share Kotlin, not the existing C, so the wire format would be written twice (C on the host, Kotlin on the phones). Kotlin/Native on iOS adds a GC and its own threading model on the hottest path. No gain over C here. |
| Native reimplementation per platform (Network.framework, `DatagramChannel`) | Three implementations of one protocol (C, Swift, Kotlin) that must stay byte-identical, and two sender loops to tune. Network.framework's completion-handler sends also add a dispatch hop per packet. |
| React Native / Flutter | Excluded: a JS or Dart bridge on the input path. A shell in one of them could still call this engine through FFI if the UI ever needs it. |

## 2. The sender loop

```
 UI thread (touch, sensors)                  sender thread (high priority)
 ──────────────────────────                  ───────────────────────────────────────────
 touch event                                 vc_client_run()
   └─ ControllerLink.set…()                    connect TCP ─ HELLO ─ WELCOME (token, slot)
        └─ vc_client_set_state()               connect UDP socket
             write buffers[back]               loop:
             swap back <-> middle (atomic) ───►  take newest state (swap middle <-> front)
             poke wake pipe ──────────────────►  encode: 8-byte header + 24-byte input
                                                 [+ 12 motion] [+ 12 touch]
                                                 send()  (non-blocking UDP)
                                                 answer PING, send own PING (RTT)
                                                 sleep until: next change / 120 Hz re-send
```

**Handoff: a lock-free triple buffer** (`vc_client_set_state` / `take_state`). The UI thread writes its private buffer, then atomically swaps it into the middle slot and marks it dirty. The sender swaps the middle into its own slot when dirty. Neither side ever blocks or waits, nobody sees a half-written state, and the sender always gets the newest one; superseded states are simply skipped. There must be **one writer**: route sensor callbacks to the main thread (CoreMotion on `OperationQueue.main`, `SensorManager` with a main-looper `Handler`).

**When it sends.** The request was a 120 Hz loop. A fixed 120 Hz loop alone adds 0–8.3 ms (4.2 ms on average) of sampling delay to every press, so the engine sends on change and keeps 120 Hz as a floor:

- **Change-driven:** `set_state` pokes a pipe that the sender sleeps on, so a change is sent at once. Sends are kept at least `min_interval_us` apart (default 2 ms, so at most 500 Hz) to protect the Wi-Fi channel; a change inside that gap goes out when the gap ends.
- **Periodic:** the current state is re-sent `rate_hz` times a second (default 120) even with nothing changing. Every datagram is a full snapshot, so a lost one costs at most one period, and the host's 250 ms release timeout never fires while the phone is connected.
- `rate_only = 1` gives the plain fixed-rate behaviour (useful for measuring).

**Precise sleeping.** The sender polls the wake pipe and the TCP socket until about 1 ms before the deadline, then `nanosleep`s the rest, because `poll` only has millisecond resolution. CPU use is a few wake-ups per frame; nothing busy-waits.

**Sockets.** UDP is `connect()`ed, so the route is resolved once and each packet is a plain `send()`. Both sockets are non-blocking. If the OS buffer is full, the datagram is dropped and counted; for latest-wins input that is the right outcome. Both sockets also get latency settings:

- DSCP EF (`IP_TOS 0xB8`) on Android and Linux;
- `SO_NET_SERVICE_TYPE = NET_SERVICE_TYPE_VO` on Apple platforms.

Both settings put the packets in the Wi-Fi WMM voice queue, which has the shortest contention window. TCP gets `TCP_NODELAY`, and `SO_NOSIGPIPE` / `MSG_NOSIGNAL` so a dropped connection is an error rather than a crash.

**Session lifecycle** (mirrors the host, see `session.h`): HELLO → WELCOME or ERROR; host PINGs answered; own PING every second for the RTT statistic. The run ends on stop (sends BYE), host ERROR or BYE, a dropped connection, or a protocol error, and `on_event` reports the reason (`vc_client_end_t`). `vc_client_run` can be called again for a new session.

## 3. On-screen gamepad

`include/layout.h` / `src/layout.c`, shared by both apps and unit-tested on every CI platform (`tests/test_layout.c`). The platform views (`GamepadView.swift`, `GamepadView.kt`) only forward raw touches, publish, and draw what the layout reports. On Android a touch is one JNI call, and the resulting state goes from the layout to the sender inside native code.

**Coordinates.** Layout units, with 1.0 being the view's height, so circles stay round. x runs 0..aspect, y runs 0..1 downwards.

**Default layout.** Landscape, designed for a phone at aspect 2 and scaled down towards the bottom edge for narrower screens:

- top corners: triggers, with shoulders below them;
- left side: a floating left stick, with the D-pad below and inside it;
- right side: the face diamond, with a fixed right stick below and inside it;
- centre: system buttons, plus the touchpad surface and a touchpad-click button for PlayStation.

The test checks that every control stays on screen from tablets (aspect 1.33) to tall phones (2.4).

**Touch rules:**

| Control | Behaviour |
|---|---|
| Buttons | Not captured: a finger presses whatever is under it now, so it can slide or rock between face buttons, and a finger between two presses both. The hit area is 15% larger than the drawn shape. |
| Floating left stick | A finger landing in its zone (most of the left half, but not on another control) becomes the stick's centre. The base is kept whole on screen. Captured until lifted. |
| Fixed right stick, D-pad, triggers, touchpad | Captured by the control the finger lands on, until lifted, wherever it slides. A second finger cannot steal a held stick or D-pad. |
| Sticks | **Radial** deadzone (default 12% left, 10% right), then rescaled so output starts at 0 at the deadzone's edge and reaches full scale at the saturation radius (85% / 90%). Radial rather than per-axis, so diagonals and fine aim are not distorted. Configurable per side (`setStick`). |
| D-pad | 8 equal 45° sectors around the centre, 25% deadzone. |
| Triggers | Full (255) while held; the host snaps them anyway for Switch. |
| Touchpad | Up to two contacts as touchpad data (0..65535 from the top-left); touching is not clicking (there is a separate click button). |

**Labels from the profiles.** Which buttons exist, and their names, come from the protocol's controller profiles (`vc_parser_button_name` in `parser.c`, which the host uses too). So:

- there is no touchpad on Xbox or Switch, and no mic button on PS4;
- the bottom face button reads **A** on Xbox, **✕** on PlayStation and **B** on Switch;
- Switch shows **+ / −** and **ZL / ZR**.

Face buttons get Xbox and PlayStation accent colours, and the full names serve as accessibility labels. Changing `controllerType` rebuilds and relabels the pad. Because the wire format is positional, nothing changes on the wire. The host fixes the emulated type at HELLO, so a type change goes with a new `ControllerLink`.

## 4. Platform layers

| | iOS (`ControllerLink.swift`) | Android (`ControllerLink.kt`) |
|---|---|---|
| Sender thread | `Thread` with `qualityOfService = .userInteractive`; the engine also sets `QOS_CLASS_USER_INTERACTIVE` itself | `Thread` + `Process.setThreadPriority(URGENT_DISPLAY)`, falling back to `DISPLAY`; the engine sets 1 µs timer slack (`PR_SET_TIMERSLACK`; default 50 µs, much more for apps Android deems background) |
| Touch input | `GamepadView`: touch handlers publish at once; `CADisplayLink` at 120 Hz (`preferredFrameRateRange` 80–120) keeps ProMotion, and so touch delivery, at 120 Hz | `GamepadView`: `requestUnbufferedDispatch` on each gesture, so MotionEvents are not batched to vsync |
| Window | needs `CADisableMinimumFrameDurationOnPhone = YES` in Info.plist for >60 Hz on iPhone | `LatencyTuning.apply(activity)`: Sustained Performance mode, highest refresh rate mode, screen kept on |
| Input API | `update { $0.buttons = … }`, `set(buttons:leftStick:…)`, `setMotion`, `setTouch`; main thread | `setState`, `setMotion`, `setTouch`; main thread; JNI with primitives only |
| Events | `eventQueue` (main by default; `ControllerLink.feedbackQueue`, `.userInteractive`, for haptics) | `Listener` on the main thread |
| Radio | (iOS has no app-level power-save control) | `WifiLock` `WIFI_MODE_FULL_LOW_LATENCY` (API 29+) while connected |
| Permissions | `NSLocalNetworkUsageDescription` in Info.plist | `INTERNET`, `WAKE_LOCK`, `ACCESS_WIFI_STATE` (library manifest) |
| Background | stop on `didEnterBackground`; sockets are suspended | stop in `onStop`; background network access is restricted |
| Shrinking | n/a | `consumer-rules.pro` keeps the JNI-called names |

### Latency budget, touch to host (estimates, to be measured)

| Stage | Typical | What the app controls |
|---|---|---|
| Touch hardware to app | 4–16 ms | 120 Hz display (CADisplayLink / highest refresh mode); unbuffered dispatch on Android |
| App to wire | < 0.1 ms with change-driven sending (would be 0–8.3 ms at a fixed 120 Hz) | done in the engine |
| Wi-Fi to host | 1–5 ms; spikes of 20–100 ms with power save on | Wi-Fi lock (Android), WMM voice queue (both), 5 GHz band |
| `vcd` to virtual pad | < 1 ms | done on the host |

So the biggest remaining wins are on the input side (unbuffered touch dispatch) and the radio (power save), not in the send loop.

### Why the send loop is not on a dispatch queue

The loop blocks for the whole session (it sleeps on `poll` and `nanosleep`). Blocking work on GCD queues ties up worker threads, and GCD responds by spawning more of them. A dedicated thread at `.userInteractive` gets the same scheduling class a high-QoS queue would, with none of that. GCD is used where it fits: delivering events (`eventQueue`, with `ControllerLink.feedbackQueue` for rumble to Core Haptics).

### Possible later optimisations

- iOS: a real-time thread (`thread_policy_set` with `THREAD_TIME_CONSTRAINT_POLICY`, as audio apps use) instead of QoS, if measured jitter warrants it.
- Android: `@CriticalNative` for `layoutTouch` / `setState` (static, primitive-only, so they qualify; it needs `RegisterNatives`), and an ADPF `PerformanceHintManager` session for the sender thread (it needs the engine to report its work durations).
- Floating stick that follows the thumb when it overshoots the radius; a layout editor (the layout is plain data: `vc_layout_add`).
- Host discovery (UDP broadcast or Bonjour) instead of typing the IP: PLAN.md decision 3.
- Port `tools/vc_send.c` onto `vc_client` (it would need a Winsock port of the engine) so the e2e tests drive the phone code too.

## 5. Status

| Part | State |
|---|---|
| `vc_client.c`, `test_client.c` | Written, reviewed by hand, never compiled: this Windows machine cannot compile (Smart App Control, see BUILD.md). The `linux` and `apple` CI jobs build and run it. |
| Swift package | Written, never compiled; the `apple` CI job runs `swift build`. |
| Android library | Written, never compiled; the `android` CI job builds every ABI. Gradle wrapper not generated yet (`gradle wrapper` in `mobile/android`). |
| `layout.c`, `test_layout.c` | Written, traced by hand, never compiled; runs in every CI job's `make test`, Windows included. |
| `GamepadView` (both), `LatencyTuning.kt` | Written, never compiled; CI builds them (`xcodebuild` for iOS Simulator, Gradle for Android). Never run on a device. |
| App projects | Not started: an activity or view controller that puts a `GamepadView` on screen with a `ControllerLink`, plus a host-address screen. |
