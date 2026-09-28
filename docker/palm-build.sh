#!/bin/sh
# Run a command inside the Palm OS toolchain container.
# The repo is at /src (the working dir when run from the repo root), and the
# repo's parent dir is also mounted at its own host path, so sibling checkouts
# such as ../fujinet-lib-palmos build in place: run this from inside them.
# Usage: docker/palm-build.sh make -C palm/apps/hello
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=$(dirname "$ROOT")
IMAGE=${PALM_IMAGE:-palmos-rs232-toolchain}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/docker"
fi
case "$PWD" in
    "$ROOT") WD=/src ;;
    "$PARENT"/*) WD=$PWD ;;
    *) WD=/src ;;
esac
exec docker run --rm --platform linux/amd64 -v "$ROOT":/src -v "$PARENT":"$PARENT" -w "$WD" "$IMAGE" "$@"
