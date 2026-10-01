# Building and running

C99, GNU make. On Windows use `mingw32-make` (MSYS2 UCRT64 gcc) wherever this says `make`.

```sh
make                  # build/stub/vcd      - no driver needed, creates no real gamepad
make BACKEND=vigem    # build/vigem/vcd.exe - Windows, real gamepad through ViGEmBus
make BACKEND=uinput   # build/uinput/vcd    - Linux, real gamepad through uinput
make test             # build and run the unit tests (always the stub backend)
make clean
```

Everything is compiled with `-std=c99 -pedantic -Wall -Wextra -Werror`. Override the optimisation level with `make CFLAGS=-g`.

## Running

```sh
build/stub/vcd --type ps5 --print
```

| Option | Meaning | Default |
|---|---|---|
| `--type xbox\|ps4\|ps5\|switch` | controller to emulate | `xbox` |
| `--port PORT` | UDP port | 28960 |
| `--bind ADDR` | IPv4 address to listen on | all interfaces |
| `--timeout MS` | release all controls after this much silence | 250 |
| `--print` | print each change of state | off |

Stop it with Ctrl+C. It releases every control, unplugs the virtual pads, disconnects from the driver and closes the socket before exiting.

There is no handshake yet: the first packet with a new session token opens a slot (4 at most), and every slot emulates the `--type` given on the command line. A slot that stays silent for 10 seconds is closed.

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

## Windows: "An Application Control policy has blocked this file"

Smart App Control can refuse to run a freshly built, unsigned executable. It happened to `vcd.exe` and to one test binary on the development machine; the verdict is per file, so some builds run and others do not. This is a Windows security setting, not a build problem. Options are to sign the binary, or to change the Smart App Control setting under Windows Security > App & browser control.

## What has been verified

| Configuration | Status |
|---|---|
| `make` and `make test`, stub backend, Windows (MinGW gcc 16.1) | built; all tests pass |
| Daemon loop: receive, parse, submit, timeout, slot limit, shutdown on SIGINT | run in-process with the stub backend. The standalone `vcd.exe` was blocked by Application Control, so a real Ctrl+C in a console is untested. |
| `make BACKEND=vigem`, Windows | builds and links; init fails cleanly without the driver. Not run with ViGEmBus installed. |
| `make BACKEND=uinput`, Linux | never compiled |
| POSIX branches of `network.c` and `main.c` | never compiled |
