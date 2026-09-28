#!/bin/sh
# Install PRC/PDB files onto the Visor via USB HotSync (palm-sync).
# Run this, then press the HotSync button on the cradle.
# Usage: tools/install-prc.sh palm/apps/hello/hello.prc [...]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLI="$ROOT/tools/palm-sync/dist/bin/cli.js"
if [ ! -f "$CLI" ]; then
    git clone --depth 1 https://github.com/jichu4n/palm-sync "$ROOT/tools/palm-sync"
    (cd "$ROOT/tools/palm-sync" && npm install --no-audit --no-fund && npm run build)
fi
exec node "$CLI" push --usb "$@"
