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

# Sanitizers are on by default.
#
# They cost nothing here - the suites run in milliseconds - and they catch the
# defects these modules are most exposed to: index arithmetic over fixed-size
# arrays, and signed overflow in the millisecond deltas that have to survive the
# 32 bit wrap. Neither one fails an assertion, so a green run without them
# proves less than it appears to. The firmware itself cannot be checked this
# way; these modules can, which is the whole reason they were made testable.
#
# -fno-sanitize-recover=all is the part that makes it a check rather than a
# log: by default UBSan prints what it found and carries on, the suite still
# exits 0, and CI stays green while reporting undefined behaviour in its own
# output.
#
# Set SAN=0 to build without them.
SAN_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
export UBSAN_OPTIONS="print_stacktrace=1"

if [ "${SAN:-1}" = "0" ]; then
  echo "sanitizers disabled (SAN=0)"
  SAN_FLAGS=""
else
  # A compiler without the sanitizer runtimes should not turn into a confusing
  # link error. Say so loudly instead - a silent fallback would let CI report
  # a pass that never ran the check.
  printf 'int main(void) { return 0; }\n' > "$OUT/.santest.cpp"
  # shellcheck disable=SC2086
  if "$CXX" $SAN_FLAGS "$OUT/.santest.cpp" -o "$OUT/.santest" 2>/dev/null && "$OUT/.santest"; then
    echo "sanitizers: address, undefined"
  else
    echo "WARNING: $CXX cannot build with ASan/UBSan - running WITHOUT sanitizers" >&2
    SAN_FLAGS=""
  fi
  rm -f "$OUT/.santest.cpp" "$OUT/.santest"
fi

fail=0
for suite in test/test_*/test_*.cpp; do
  name=$(basename "$suite" .cpp)
  printf '\n=== %s ===\n' "$name"
  # shellcheck disable=SC2086
  "$CXX" $FLAGS $SAN_FLAGS $INCLUDES "$suite" "$UNITY/unity.c" -o "$OUT/$name"
  "$OUT/$name" || fail=1
done

if [ "$fail" -ne 0 ]; then
  echo "\nsome suites failed" >&2
  exit 1
fi
echo "\nall suites passed"
