#!/bin/bash
# Usage: ./build.sh [release|debug|asan|tsan|ubsan] [--clean] [--run]

set -euo pipefail

PROFILE="${1:-release}"
CLEAN=0
RUN=0
for arg in "${@:2}"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --run)   RUN=1 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

case "$PROFILE" in
    release) BUILD_TYPE=Release; SANITIZER="" ;;
    debug)   BUILD_TYPE=Debug;   SANITIZER="" ;;
    asan)    BUILD_TYPE=Debug;   SANITIZER="address" ;;
    tsan)    BUILD_TYPE=Debug;   SANITIZER="thread" ;;
    ubsan)   BUILD_TYPE=Debug;   SANITIZER="undefined" ;;
    *) echo "unknown profile: $PROFILE (use release|debug|asan|tsan|ubsan)" >&2; exit 1 ;;
esac

BUILD_DIR="build/$PROFILE"
[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD_DIR"

cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLLT_SANITIZER="$SANITIZER"

cmake --build "$BUILD_DIR" -j"$(nproc)"

ctest --test-dir "$BUILD_DIR" --output-on-failure

echo ""
echo "built: $BUILD_DIR/llt_bot"

# The bot places real orders once the execution plane lands, so running it is opt-in.
if [[ $RUN -eq 1 ]]; then
    echo ""
    "./$BUILD_DIR/llt_bot"
fi
