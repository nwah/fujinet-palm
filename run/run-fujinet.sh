#!/bin/sh
# Run fujinet-pc (RS232 target) with bus-over-IP on localhost:1985.
# FUJINET_BIN overrides the binary (default: ~/Atari/fn-build-rs232/fujinet).
cd "$(dirname "$0")" || exit 1
BIN=${FUJINET_BIN:-$HOME/Atari/fn-build-rs232/fujinet}
"$BIN" -c fnconfig.ini -s SD "$@"
rc=$?
while [ $rc -eq 75 ]; do
    "$BIN" -c fnconfig.ini -s SD "$@"
    rc=$?
done
exit $rc
