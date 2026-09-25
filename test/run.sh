#!/bin/sh
# Builds test/test.c against the fake Win32 headers next to it and runs it
# with wintray in an exe, in a DLL, and with the module lookup failing.
# Needs gcc or clang (set CC to choose).
set -e
cd "$(dirname "$0")"
out="${TMPDIR:-/tmp}/wintray-logic-test"
${CC:-gcc} -std=gnu11 -fshort-wchar -g -O1 -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -Wall -Wextra -I . test.c -o "$out"
for mode in exe dll gmhfail; do
    "$out" "$mode"
done
