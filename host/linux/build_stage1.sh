#!/bin/sh
set -e
cd "$(dirname "$0")/.."
game=${1:-gangstar}
for tool in gcc cmake; do
    command -v $tool >/dev/null || { echo "no $tool (Debian, Ubuntu: apt install gcc-multilib cmake ninja-build)"; exit 1; }
done
if command -v ninja >/dev/null; then gen=Ninja; bin=../build/cmake/linux-$game-stage1
else gen="Unix Makefiles"; bin=../build/cmake/linux-$game-stage1-make; fi
cmake -S . -B "$bin" -G "$gen" -DGMV_GAME="$game" -DGMV_STAGE1=ON -DGMV_OUT_DIR=
cmake --build "$bin"
