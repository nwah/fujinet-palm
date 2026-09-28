#!/bin/sh
# Run a command inside the Palm OS toolchain container with the repo at /src.
# Usage: docker/palm-build.sh make -C palm/apps/hello
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${PALM_IMAGE:-palmos-rs232-toolchain}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/docker"
fi
exec docker run --rm --platform linux/amd64 -v "$ROOT":/src -w /src "$IMAGE" "$@"
