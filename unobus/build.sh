#!/bin/sh
set -e
CC="${CC:-gcc}"; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"; mkdir -p build
"$CC" -std=c11 -Wall -Wextra -O1 -I unodef/gen/c -I unofs -I unobus \
      unobus/unobus.c unobus/unobus_test.c -o build/unobus_test
./build/unobus_test
