#!/bin/sh
set -e
cd "$(dirname "$0")/.."
game=${1:-template}
kind=${BUILD:-plain}
for tool in gcc cmake; do
    command -v $tool >/dev/null || { echo "no $tool (Debian, Ubuntu: apt install gcc-multilib cmake ninja-build)"; exit 1; }
done
if command -v ninja >/dev/null; then gen=Ninja; bin=../build/cmake/linux-$game-$kind
else gen="Unix Makefiles"; bin=../build/cmake/linux-$game-$kind-make; fi
icon=OFF
[ -n "$NOICON" ] && icon=ON
cmake -S . -B "$bin" -G "$gen" -DGMV_GAME="$game" -DGMV_BUILD="$kind" -DGMV_NOICON=$icon -DGMV_OUT_DIR= -DGMV_STAGE1=OFF
cmake --build "$bin"
