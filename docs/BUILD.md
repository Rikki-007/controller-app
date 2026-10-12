# Building and running

C99, GNU make. On Windows use `mingw32-make` (MSYS2 UCRT64 gcc) wherever this says `make`.

```sh
make                  # build/stub/vcd      - no driver needed, creates no real gamepad
                      # + build/tools/vc_send, the test client
make BACKEND=vigem    # build/vigem/vcd.exe - Windows, real gamepad through ViGEmBus
make BACKEND=uinput   # build/uinput/vcd    - Linux, real gamepad through uinput
make test             # build and run the unit tests (always the stub backend)
make test-build       # build the unit tests without running them
make e2e              # run vcd and drive it with vc_send (needs sh)
make clean
```

Everything is compiled with `-std=c99 -pedantic -Wall -Wextra -Werror`. Override the optimisation level with `make CFLAGS=-g`, the compiler with `make CC=clang`, and the output root with `make BUILD=dir` (default `build`; use a separate one per OS when Windows and WSL share a checkout).

On Windows with Smart App Control on, use `scripts/test.ps1` instead of `make test`; see [the section below](#windows-smart-app-control).

## Running

```sh
build/stub/vcd --print
```

| Option | Meaning | Default |
|---|---|---|
| `--port PORT` | TCP (control) and UDP (input) port | 28960 |
| `--bind ADDR` | IPv4 address to listen on | all interfaces |
| `--timeout MS` | release all controls after this much input silence | 250 |
| `--print` | print each change of state | off |

`--type` is gone: each client chooses its controller type in its HELLO.

Stop it with Ctrl+C. It sends BYE to every client, releases and unplugs every virtual pad, disconnects from the driver and closes the sockets before exiting.

### Sessions

A client connects over TCP and sends HELLO with its controller type, capabilities and name. If one of the 4 slots is free and the backend can present that type, `vcd` plugs in a virtual pad and answers WELCOME with a random session token, the slot and the granted capabilities; otherwise it answers ERROR (no free slot, unsupported type) and closes. The client then streams UDP input carrying the token, from the same IP address as its TCP connection. Anything else is dropped. `vcd` PINGs every second and expects PONGs.

| Event | Result |
|---|---|
| no input for `--timeout` (250 ms) | all controls released; the session stays |
| client sends BYE | session closed |
| TCP connection drops | session closed |
| 3 PINGs unanswered | ERROR timeout, session closed |
| no input for 10 s | ERROR timeout, session closed (catches UDP blocked by a firewall) |
| no HELLO within 5 s of connecting | ERROR timeout, connection closed |
| malformed or unexpected control message | ERROR protocol (or bad version), session closed |

Closing a session always releases and unplugs its pad. The logic is in `src/session.c` and the cases above are unit-tested with a fake clock in `tests/test_session.c`.

## Test client: vc_send

`tools/vc_send.c` plays the part of the phone: it does the TCP handshake, streams scripted UDP input with the token it was given, answers PINGs and says BYE, so the whole daemon can be exercised without the app.

```sh
build/stub/vcd --print                                       # terminal 1
build/tools/vc_send --type ps5 --script sweep --motion       # terminal 2
```

| Option | Meaning | Default |
|---|---|---|
| `--host ADDR` / `--port PORT` | where `vcd` listens | 127.0.0.1, 28960 |
| `--type TYPE` | `xbox`, `ps4`, `ps5`, `switch`, or a raw number (to test refusals) | `xbox` |
| `--name NAME` | name sent in HELLO; controller *i* is `NAME-i` | `vc_send` |
| `--clients N` | controllers to simulate (1–16), each with its own connection and sockets | 1 |
| `--script NAME` | `buttons` (one button at a time, 4 per second), `sweep` (sticks and triggers move), `hold` (South held, stick right), `idle` | `buttons` |
| `--rate HZ` | packets per second per controller | 120 |
| `--duration MS` | how long to send input; 0 runs until Ctrl+C | 3000 |
| `--linger MS` | keep the connection open, answering PINGs, after input stops | 0 |
| `--end bye\|close` | end with BYE, or drop the connection without a word | `bye` |
| `--motion`, `--touch` | offer and send motion and touch data | off |
| `--bad` | first send malformed, oversized, duplicate and stale packets from the first controller | off |
| `--no-pong` | ignore the host's PINGs | off |
| `--no-handshake` | skip TCP and send input with `--token HEX` (which `vcd` must drop) | off |

`tests/e2e.sh` (`make e2e`) starts `vcd` and runs `vc_send` through every path above: five clients for four slots (four WELCOMEs, one "no free slot"), bad datagrams, stale packets and input reaching the pads, all ending with BYE; then, in parallel, a client that stalls (released after 250 ms, closed after 10 s), one that drops TCP, one that ignores PINGs, one with an unknown token and one with an unknown controller type. It takes about 15 seconds.

## HID backends

`src/hid_output.c` contains three backends; `BACKEND=` selects one by passing a compile flag.

| `BACKEND` | Flag | Platform | Needs |
|---|---|---|---|
| `stub` | none | any | nothing |
| `vigem` | `-DVC_HID_BACKEND_VIGEM` | Windows | ViGEmBus driver, ViGEmClient source |
| `uinput` | `-DVC_HID_BACKEND_UINPUT` | Linux | `uinput` kernel module, write access to `/dev/uinput` |

### Windows: ViGEm

1. **Driver (runtime).** Install ViGEmBus from <https://github.com/nefarius/ViGEmBus/releases>. Without it `vcd` exits with "could not connect to ViGEmBus".
2. **Client library (build time).** ViGEmClient is C++ and is compiled from source by the Makefile:

   ```sh
   git clone https://github.com/nefarius/ViGEmClient.git third_party/ViGEmClient
   mingw32-make BACKEND=vigem
   ```

   Use `VIGEM_DIR=path` if it lives elsewhere.

What the Makefile does, if you need to reproduce it by hand:

- compiles `ViGEmClient.cpp` with `g++`;
- compiles our sources with `-DVC_HID_BACKEND_VIGEM -isystem $(VIGEM_DIR)/include` (`-isystem` because the ViGEm headers trigger `-Wextra` warnings of their own);
- links with `-lsetupapi -lstdc++ -lws2_32`.

### Linux: uinput

No library is needed; `linux/uinput.h` comes with the toolchain. Requires kernel 4.5 or newer.

At runtime the process must be able to write to `/dev/uinput`:

```sh
sudo modprobe uinput
# one-off test:
sudo chmod 0660 /dev/uinput && sudo chgrp input /dev/uinput
# permanent, as /etc/udev/rules.d/60-vcd-uinput.rules:
#   KERNEL=="uinput", GROUP="input", MODE="0660", OPTIONS+="static_node=uinput"
```

and the user must be in the `input` group.

## Windows: Smart App Control

Symptoms: `An Application Control policy has blocked this file` when running a test or `vcd.exe`, or `gcc: fatal error: cannot execute '...cc1.exe'` when compiling. Event Viewer shows it under Applications and Services Logs > Microsoft > Windows > CodeIntegrity > Operational (events 3033/3077).

Smart App Control (SAC) blocks unsigned executables that Microsoft's cloud service does not know, and its verdict can change over time. On the development machine it blocks every binary this repo builds, and since October 2026 also MSYS2's own compiler, `cc1.exe`, so nothing can be built or run natively there. It is a Windows security setting, not a build problem. Things that do **not** help:

- `Set-ExecutionPolicy`: it governs PowerShell scripts, not executables.
- Self-signed certificates: SAC only trusts signatures that chain to Microsoft's trusted roots.
- Building with MSVC: its compiler is signed, but what it produces is not.

What works:

| Route | How |
|---|---|
| `scripts/test.ps1` | `powershell -ExecutionPolicy Bypass -File scripts\test.ps1 [-E2E]`. Builds and runs natively; if SAC blocks the compiler or a test, reruns everything inside WSL with output in `build/wsl`. Exit code 3 means nothing could be verified. `-Wsl` skips the native attempt. |
| WSL | Linux binaries are not subject to SAC, and the POSIX code paths get built as well. Once, as administrator: `wsl --install -d Ubuntu`, then in Ubuntu `sudo apt install -y build-essential`. Then `make BUILD=build/wsl test e2e`, or just the script above. |
| CI | `.github/workflows/ci.yml` runs the unit tests and `make e2e` on GitHub's Linux and Windows (MSYS2) runners, which do not use SAC, and compile-checks the uinput and ViGEm backends. |
| Turn SAC off | Windows Security > App & browser control > Smart App Control settings. Affects the whole machine; on some Windows builds it cannot be turned back on without reinstalling, so read the warning there first. |
| Trusted signing | A certificate from a CA in Microsoft's program (or Azure Artifact Signing). Worth it for release builds of `vcd`, too heavy for a test loop. |

## What has been verified

| Configuration | Status |
|---|---|
| `make` and `make test`, stub backend, Windows (MinGW gcc 16.1) | built; all tests pass |
| Daemon loop: receive, parse, submit, timeout, slot limit, shutdown on SIGINT | run in-process with the stub backend. The standalone `vcd.exe` was blocked by Application Control, so a real Ctrl+C in a console is untested. |
| `make BACKEND=vigem`, Windows | builds and links; init fails cleanly without the driver. Not run with ViGEmBus installed. |
| `make BACKEND=uinput`, Linux | never compiled locally; CI compiles it |
| POSIX branches of `network.c` and `main.c` | never compiled locally; CI compiles and tests them |
| `vc_send`, `test_encoder`, `test_loopback`, `tests/e2e.sh` | written after SAC began blocking `cc1.exe`, so not yet compiled or run anywhere; CI is the first place they run |
| TCP control channel and sessions (`protocol.c`, `session.c`, TCP in `network.c`, `test_protocol`, `test_session`, new `test_network` cases) | same: written and reviewed by hand, never compiled |
| Phone engine `vc_client.c` and `test_client` (POSIX only, not built on Windows), Swift package, Android library | same; see [MOBILE.md](MOBILE.md). CI's `linux`, `apple` and `android` jobs are their first build |
