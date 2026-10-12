#!/usr/bin/env bash
# Builds and runs the Desk mode host tests: no device, no network, no test framework needed.
#
#   test/desk_mode/run.sh                 run every test
#   test/desk_mode/run.sh <test-name>     run one test (names are listed in DeskModeTest.cpp)
#
# The tests compile the production headers in src/activities/desk/ and the real SecureHttpClient from the SDK
# against a scripted socket and a fake clock (stubs/). The binary is built with AddressSanitizer and
# UndefinedBehaviorSanitizer and no error recovery, so integer overflow and bad memory use fail the run.
# The build directory is ignored by git. Set CXX to use a compiler other than c++, and CXXFLAGS=-Werror to
# turn warnings into failures.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$here/build"
sdk_header="$repo/freeink-sdk/libs/network/SecureNet/include/SecureHttpClient.h"

if [ ! -f "$sdk_header" ]; then
  echo "freeink-sdk is not checked out: run 'git submodule update --init freeink-sdk'" >&2
  exit 2
fi

mkdir -p "$build/sdk"
# Copied (not included in place) so the SDK's own SecureClient.h does not shadow the host stub.
cp "$sdk_header" "$build/sdk/SecureHttpClient.h"

"${CXX:-c++}" -std=c++20 -g -O1 -Wall -Wextra ${CXXFLAGS:-} \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$build/sdk" \
  -I"$here/stubs" \
  -I"$repo/lib/GfxRenderer" \
  -I"$repo/src/activities/desk" \
  "$here/DeskModeTest.cpp" \
  -o "$build/DeskModeTest"

"$build/DeskModeTest" "$@"
