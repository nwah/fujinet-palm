#!/bin/sh
# Drive CloudpilotEmu (native build) headlessly-ish for testing Palm apps
# against fujinet-pc. See docs/emulator-testing.md.
#
#   tools/emu.sh start [rom-or-image] [cloudpilot-emu args...]
#   tools/emu.sh cmd "<cli command>" ["<cli command>" ...]
#   tools/emu.sh shot [out.png]
#   tools/emu.sh log [lines]
#   tools/emu.sh stop
#
# Paths default to checkouts next to this repo (../cloudpilot-emu,
# ../palm-emu), wherever it lives.
#
# Environment:
#   CLOUDPILOT     emulator binary (default ../cloudpilot-emu/src/cloudpilot/cloudpilot-emu)
#   EMU_IMAGE      session image for a bare `start` (default ../palm-emu/palmv-base.img)
#   EMU_DIR        state dir: command file, log, pid, screenshots (default $TMPDIR/palm-emu)
#   EMU_CMD_DELAY  seconds to wait after each command (default 1.5)
set -e

TOOLS_DIR=$(cd "$(dirname "$0")" && pwd)
SIBLINGS=$(cd "$TOOLS_DIR/../.." && pwd)
CLOUDPILOT=${CLOUDPILOT:-$SIBLINGS/cloudpilot-emu/src/cloudpilot/cloudpilot-emu}
EMU_IMAGE=${EMU_IMAGE:-$SIBLINGS/palm-emu/palmv-base.img}
EMU_DIR=${EMU_DIR:-${TMPDIR:-/tmp}/palm-emu}
EMU_CMD_DELAY=${EMU_CMD_DELAY:-1.5}

mkdir -p "$EMU_DIR"

emu_pid() {
    [ -f "$EMU_DIR/emu.pid" ] && cat "$EMU_DIR/emu.pid"
}

emu_running() {
    pid=$(emu_pid) && [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

# Compile the window-ID helper once. It prints the CGWindowID of the
# on-screen "CloudpilotEmu" window owned by the given PID.
winid_helper() {
    bin="$EMU_DIR/winid"
    if [ ! -x "$bin" ]; then
        cat > "$EMU_DIR/winid.swift" <<'EOF'
import CoreGraphics
let pid = Int(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as! [[String: Any]]
for w in list where (w["kCGWindowOwnerPID"] as? Int) == pid
    && (w["kCGWindowName"] as? String) == "CloudpilotEmu" {
    print(w["kCGWindowNumber"]!)
    break
}
EOF
        swiftc -O "$EMU_DIR/winid.swift" -o "$bin"
    fi
    echo "$bin"
}

case "$1" in
start)
    shift
    # No image given (nothing, or only --options): use the base image
    case "$1" in
    ""|-*) set -- "$EMU_IMAGE" "$@" ;;
    esac
    [ -x "$CLOUDPILOT" ] || { echo "no emulator at $CLOUDPILOT (set CLOUDPILOT; see docs/emulator-testing.md)" >&2; exit 1; }
    [ -f "$1" ] || { echo "no ROM or image at $1 (set EMU_IMAGE or pass one)" >&2; exit 1; }
    if emu_running; then echo "emulator already running (pid $(emu_pid))" >&2; exit 1; fi
    : > "$EMU_DIR/cmds"
    : > "$EMU_DIR/emu.log"
    # tail -f keeps stdin open so the CLI never sees EOF; commands are
    # appended to the file. (A FIFO deadlocks on open -- don't.)
    tail -f "$EMU_DIR/cmds" | "$CLOUDPILOT" "$@" >> "$EMU_DIR/emu.log" 2>&1 &
    sleep 1
    pgrep -n -f "^$CLOUDPILOT " > "$EMU_DIR/emu.pid" || { cat "$EMU_DIR/emu.log"; exit 1; }
    # Wait for the CLI prompt.
    i=0
    while ! grep -q '^> ' "$EMU_DIR/emu.log" && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    echo "started pid $(emu_pid); log $EMU_DIR/emu.log"
    ;;
cmd)
    shift
    emu_running || { echo "emulator not running" >&2; exit 1; }
    for c in "$@"; do
        echo "$c" >> "$EMU_DIR/cmds"
        sleep "$EMU_CMD_DELAY"
    done
    ;;
shot)
    emu_running || { echo "emulator not running" >&2; exit 1; }
    out=${2:-$EMU_DIR/shot-$(date +%H%M%S).png}
    wid=$("$(winid_helper)" "$(emu_pid)")
    [ -n "$wid" ] || { echo "emulator window not found (minimised or off-screen?)" >&2; exit 1; }
    # -l <id>: capture ONLY that window, never the whole desktop.
    screencapture -x -o -l "$wid" "$out"
    sips -Z 400 "$out" > /dev/null
    echo "$out"
    ;;
log)
    tail -n "${2:-30}" "$EMU_DIR/emu.log"
    ;;
stop)
    pkill -f "tail -f $EMU_DIR/cmds" 2>/dev/null || true
    # The emulator ignores SIGTERM while in its CLI loop.
    if emu_running; then kill -9 "$(emu_pid)"; fi
    rm -f "$EMU_DIR/emu.pid"
    echo stopped
    ;;
*)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
