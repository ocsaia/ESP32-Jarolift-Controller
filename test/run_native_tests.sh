#!/bin/sh
# Build and run every native test suite.
#
# Run it directly if a host compiler is on PATH:
#
#     sh test/run_native_tests.sh
#
# or in a container, which needs nothing installed and is what CI would use:
#
#     docker run --rm -v "$PWD:/w" -w /w gcc:13 sh test/run_native_tests.sh
#
# Unity comes from the PlatformIO library cache, so `pio pkg install` (or any
# device build) has to have run at least once.
set -e

CXX="${CXX:-g++}"
UNITY="${UNITY:-.pio/libdeps/esp32/Unity/src}"
OUT="${OUT:-/tmp}"

if [ ! -f "$UNITY/unity.c" ]; then
  echo "Unity not found at $UNITY - run 'pio pkg install -e esp32' first" >&2
  exit 1
fi

INCLUDES="-I test/shim -I include -I lib/Dusk2Dawn -I lib/JaroliftController -I $UNITY"
FLAGS="-std=gnu++17 -Wall"

fail=0
for suite in test/test_*/test_*.cpp; do
  name=$(basename "$suite" .cpp)
  printf '\n=== %s ===\n' "$name"
  # shellcheck disable=SC2086
  "$CXX" $FLAGS $INCLUDES "$suite" "$UNITY/unity.c" -o "$OUT/$name"
  "$OUT/$name" || fail=1
done

if [ "$fail" -ne 0 ]; then
  echo "\nsome suites failed" >&2
  exit 1
fi
echo "\nall suites passed"
