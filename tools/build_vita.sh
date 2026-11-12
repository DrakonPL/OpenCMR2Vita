#!/bin/sh
# Builds the vita preset in the vitasdk Docker image. Extra arguments go to
# "cmake --build", for example: --target cmr2_game
set -e
cd "$(dirname "$0")/.."

step() {
    name=$1
    shift
    start=$(date +%s)
    echo "==> $name"
    "$@"
    echo "==> $name: done in $(($(date +%s) - start)) s"
}

if [ ! -f build/vita/build.ninja ]; then
    step configure cmake --preset vita
fi
step build cmake --build --preset vita "$@"
