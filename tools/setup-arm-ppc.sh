#!/bin/bash
# One-time: install bare-metal cross binutils (aarch64 + powerpc) in WSL for the
# UnoDOS ARM64 (rpi/pinephone) and PowerPC (ppcmac) ports.
set -e
echo "=== distro ==="
. /etc/os-release; echo "$PRETTY_NAME"

need=""
command -v aarch64-linux-gnu-as   >/dev/null 2>&1 || need="$need binutils-aarch64-linux-gnu"
command -v powerpc-linux-gnu-as   >/dev/null 2>&1 || need="$need binutils-powerpc-linux-gnu"

if [ -n "$need" ]; then
  echo "=== installing:$need ==="
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $need
fi

echo "=== versions ==="
aarch64-linux-gnu-as --version | head -1
powerpc-linux-gnu-as --version | head -1
aarch64-linux-gnu-ld --version | head -1
powerpc-linux-gnu-ld --version | head -1

echo "=== smoke test: assemble a trivial object each ==="
tmp=$(mktemp -d)
printf '.text\n.globl _start\n_start:\n  mov x0, #1\n  ret\n' > "$tmp/a64.s"
aarch64-linux-gnu-as "$tmp/a64.s" -o "$tmp/a64.o" && echo "aarch64 OK"
printf '.text\n.globl _start\n_start:\n  li 3,1\n  blr\n' > "$tmp/ppc.s"
powerpc-linux-gnu-as -mregnames "$tmp/ppc.s" -o "$tmp/ppc.o" && echo "powerpc OK"
rm -rf "$tmp"
echo "=== DONE ==="
