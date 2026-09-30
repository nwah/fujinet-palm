#!/bin/sh
# Run fujinet-pc (RS232 target) with bus-over-IP on localhost:1985.
# The binary comes from FUJINET_BIN, else a fujinet-firmware checkout next to
# this repo, built with `./build.sh -p RS232` (it lands in build/dist/).
cd "$(dirname "$0")" || exit 1
BIN=${FUJINET_BIN:-../../fujinet-firmware/build/dist/fujinet}
if [ ! -x "$BIN" ]; then
    echo "fujinet-pc not found at $BIN" >&2
    echo "Build it: (cd ../../fujinet-firmware && ./build.sh -p RS232), or set FUJINET_BIN" >&2
    exit 1
fi
"$BIN" -c fnconfig.ini -s SD "$@"
rc=$?
while [ $rc -eq 75 ]; do
    "$BIN" -c fnconfig.ini -s SD "$@"
    rc=$?
done
exit $rc
