#!/bin/sh
set -e
CC="${CC:-gcc}"; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"; mkdir -p build
"$CC" -std=c11 -Wall -Wextra -O1 -I unobus -I unonet \
      unobus/unobus.c unonet/uno_nic.c unonet/unonet_test.c -o build/unonet_test
./build/unonet_test
