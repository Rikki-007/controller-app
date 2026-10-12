#!/bin/sh
# e2e.sh - end-to-end test of the standalone daemon, driven by vc_send.
#
#   sh tests/e2e.sh build/stub/vcd build/tools/vc_send
#   make e2e                      (same thing, for the current BACKEND)
#
# Starts vcd and plays phones at it, then checks from vcd's own output that
# every part of the session lifecycle works with real processes and sockets:
#
#   phase 1  five clients handshake for four slots: four WELCOMEs, one
#            "no free slot"; bad datagrams dropped, duplicate and older
#            packets counted stale; input reaches the pads; all say BYE
#   phase 2  in parallel, one client per way a session can end:
#            stall  input stops, TCP stays up: released after 250 ms,
#                   closed after 10 s with ERROR TIMEOUT
#            drop   TCP closed without BYE: "connection lost"
#            mute   PINGs unanswered: "keepalive timeout"
#            stray  UDP with a token nobody was given: dropped
#            odd    unknown controller type: refused
#
# Works with sh on Linux, macOS, WSL, Git Bash and MSYS2. Takes ~15 s, most
# of it the 10 s idle timeout. Set VCD_E2E_PORT to use another port
# (default 28977, TCP and UDP).

set -u

VCD=${1:?usage: e2e.sh path/to/vcd path/to/vc_send}
VC_SEND=${2:?usage: e2e.sh path/to/vcd path/to/vc_send}
PORT=${VCD_E2E_PORT:-28977}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/vcd-e2e.XXXXXX")
LOG=$TMP/vcd.log
VCD_PID=

failures=0
pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; failures=$((failures + 1)); }

cleanup() {
    if [ -n "$VCD_PID" ]; then
        kill "$VCD_PID" 2>/dev/null
        wait "$VCD_PID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

# Waits up to $2 tenths of a second for $3 lines of the log to match $1.
wait_for() {
    tries=0
    while [ "$tries" -lt "$2" ]; do
        if [ "$(grep -c -- "$1" "$LOG")" -ge "$3" ]; then
            return 0
        fi
        sleep 0.1
        tries=$((tries + 1))
    done
    return 1
}

# expect_count NAME PATTERN MIN [MAX]   (PATTERN is a basic regular expression)
expect_count() {
    n=$(grep -c -- "$2" "$LOG")
    if [ "$n" -ge "$3" ] && [ "$n" -le "${4:-$n}" ]; then
        pass "$1 ($n)"
    else
        fail "$1: expected ${3}${4:+..$4} lines matching '$2', found $n"
    fi
}

# send NAME ARGS...: runs vc_send with output in $TMP/NAME.out
send() {
    name=$1
    shift
    "$VC_SEND" --port "$PORT" --name "$name" "$@" >"$TMP/$name.out" 2>&1
}

for f in "$VCD" "$VC_SEND"; do
    if [ ! -f "$f" ] && [ ! -f "$f.exe" ]; then
        echo "e2e: $f not found; build it first (make all)" >&2
        exit 2
    fi
done

# --- start -------------------------------------------------------------------

"$VCD" --port "$PORT" --bind 127.0.0.1 --print >"$LOG" 2>&1 &
VCD_PID=$!

if ! wait_for 'vcd running' 50 1; then
    echo "e2e: vcd did not start; its output was:" >&2
    cat "$LOG" >&2
    exit 1
fi
backend=$(sed -n 's/^vcd running: \([a-z]*\) backend.*/\1/p' "$LOG")
echo "vcd started on 127.0.0.1:$PORT with the '$backend' backend"

# --- phase 1: handshakes, slot limit, input, BYE -----------------------------

if send full --type ps5 --clients 5 --script buttons --duration 1500 \
        --motion --touch --bad; then
    pass "phase 1: vc_send exited cleanly"
else
    fail "phase 1: vc_send exited with status $?"
fi
wait_for 'client said bye' 30 4

expect_count "four slots opened" 'slot [0-3] opened for "full-[0-4]"' 4 4
expect_count "PS5 granted motion and touch" '"full-0" .*PS5 as .*caps motion+touch' 1 1
expect_count "fifth client refused" 'refused "full-4" .*no free slot' 1 1
expect_count "malformed datagrams dropped" 'dropped a [0-9]*-byte datagram' 1
expect_count "oversized datagram dropped" 'oversized datagram' 1
expect_count "input reached the pads" 'slot [0-3]: buttons=' 20
expect_count "every session ended with BYE" 'closed ("full-[0-3]"): client said bye' 4 4
expect_count "duplicate and older packet counted as stale" \
    '"full-0"): client said bye; [1-9][0-9]* inputs accepted, 2 stale' 1 1
if grep -q 'refused: ERROR 2 (no free slot)' "$TMP/full.out"; then
    pass "vc_send saw ERROR no free slot"
else
    fail "vc_send did not report ERROR no free slot"
fi

# --- phase 2: every other way a session ends, in parallel --------------------

send stall --type xbox --script hold --duration 500 --linger 12000 & stall=$!
send drop --duration 500 --end close & drop=$!
send mute --script idle --duration 6000 --no-pong & mute=$!
send stray --no-handshake --token deadbeef --duration 300 & stray=$!
send odd --type 9 & odd=$!
for pid in $stall $drop $mute $stray $odd; do
    wait "$pid" || fail "phase 2: a vc_send exited with status $? (pid $pid)"
done
# vc_send can exit on the ERROR before vcd has logged the close.
wait_for 'slot [0-3] closed (' 30 7

expect_count "phase 2 sessions opened" 'slot [0-3] opened for "[dms][a-z]*-0"' 3 3
expect_count "stall: released after the input timeout" \
    '"stall-0"): no input for 250 ms, releasing all controls' 1
expect_count "stall: closed after the idle timeout" \
    'closed ("stall-0"): no input for 10000 ms' 1 1
expect_count "drop: closed when TCP dropped" 'closed ("drop-0"): connection lost' 1 1
expect_count "mute: closed for unanswered PINGs" 'closed ("mute-0"): keepalive timeout' 1 1
expect_count "stray: unknown token dropped" 'dropped input with unknown token deadbeef' 1
expect_count "odd: unknown controller type refused" 'refused "odd-0" .*not supported' 1 1
if grep -q 'host sent ERROR 5 (timeout)' "$TMP/stall.out" &&
   grep -q 'host sent ERROR 5 (timeout)' "$TMP/mute.out"; then
    pass "vc_send saw ERROR timeout for stall and mute"
else
    fail "vc_send did not report ERROR timeout for stall and mute"
fi

# --- overall -----------------------------------------------------------------

expect_count "no driver errors" '\[hid\] error' 0 0
expect_count "every session closed" 'slot [0-3] closed (' 7 7

if [ "$failures" -ne 0 ]; then
    echo "--- vcd output ---"
    cat "$LOG"
    for out in "$TMP"/*.out; do
        echo "--- $(basename "$out" .out) ---"
        cat "$out"
    done
    echo "e2e: FAILED ($failures)"
    exit 1
fi
echo "e2e: ok"
