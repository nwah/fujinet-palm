#!/bin/sh
# Run a command inside the Palm OS toolchain container.
# The repo's parent directory is mounted at its own host path and the
# command runs in the current directory, so sibling checkouts such as
# ../fujinet-lib-palmos resolve the same inside the container as outside.
# Usage: docker/palm-build.sh make -C palm/apps/hello
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PARENT=$(dirname "$ROOT")
IMAGE=${PALM_IMAGE:-fujinet-palm-toolchain}
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$IMAGE" "$ROOT/docker"
fi
case "$PWD" in
    "$PARENT"/*) ;;
    *) echo "run this from inside $PARENT (the repo or a sibling checkout)" >&2; exit 1 ;;
esac
exec docker run --rm --platform linux/amd64 -v "$PARENT":"$PARENT" -w "$PWD" "$IMAGE" "$@"
