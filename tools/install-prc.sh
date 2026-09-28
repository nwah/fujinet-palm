#!/bin/sh
# Install PRC/PDB files onto the Visor via USB HotSync (palm-sync).
# Run this, then press the HotSync button on the cradle.
# Usage: tools/install-prc.sh palm/apps/hello/hello.prc [...]
#
# tools/visorbridge.js also watches for the Visor and would take the HotSync
# session itself, so it is paused (SIGSTOP) for the duration and resumed on
# exit. Exit any FujiNet app first so the bridge isn't holding the device.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLI="$ROOT/tools/palm-sync/dist/bin/cli.js"
if [ ! -f "$CLI" ]; then
    git clone --depth 1 https://github.com/jichu4n/palm-sync "$ROOT/tools/palm-sync"
    (cd "$ROOT/tools/palm-sync" && npm install --no-audit --no-fund && npm run build)
fi
BRIDGE=$(pgrep -f 'node .*visorbridge\.js' || true)
if [ -n "$BRIDGE" ]; then
    echo "Pausing visorbridge ($BRIDGE) during install"
    kill -STOP $BRIDGE
    trap 'kill -CONT $BRIDGE 2>/dev/null; echo "Resumed visorbridge"' EXIT INT TERM
fi
node "$CLI" push --usb "$@"
