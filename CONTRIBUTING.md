# Contributing to UnoDOS

Thanks for your interest in UnoDOS! This document explains how to get started.

## Getting Started

### Prerequisites

- Linux (Ubuntu/Debian recommended)
- NASM assembler
- QEMU (for testing)
- Python 3
- Make

```bash
sudo apt install nasm qemu-system-x86 make python3
```

### Building

```bash
# Build everything
make clean && make floppy144 && make apps && make build/launcher-floppy.img

# Run in QEMU
make run144

# Build HD image too
make hd-image
make run-hd
```

### Project Layout

- `kernel/kernel.asm` — The entire kernel (one large assembly file)
- `apps/*.asm` — Each application is a standalone NASM source file
- `boot/*.asm` — Boot chain (MBR, VBR, stage2 loaders)
- `tools/*.py` — Build tools (image creation, filesystem)
- `docs/` — Technical documentation

### How It Works

The kernel communicates with apps through `INT 0x80` system calls (similar in concept to Linux, but with a completely different API). Apps are flat `.BIN` binaries that each run in their own 64KB segment. The [App Development Guide](docs/APP_DEVELOPMENT.md) covers everything you need to write an app.

## What to Contribute

### Good First Contributions

- **New applications** — Write a new app using the existing API. See [docs/APP_DEVELOPMENT.md](docs/APP_DEVELOPMENT.md) for the full guide and [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for available system calls.
- **Icon artwork** — Apps use 16x16 2bpp CGA icons. The existing ones are functional but could be improved.
- **Documentation** — Corrections, clarifications, or tutorials.
- **Screenshots** — The README needs screenshots from real hardware or QEMU.

### Larger Contributions

- Bug fixes in the kernel or existing apps
- New kernel APIs (coordinate with maintainer first)
- Hardware testing on machines not yet listed

## Code Style

- x86 assembly uses Intel syntax (NASM)
- Comments explain *why*, not *what* — `; Skip header` not `; Jump past 80 bytes`
- Labels use `snake_case`
- Constants use `UPPER_CASE`
- Local labels start with `.` (e.g., `.loop:`, `.exit:`)
- Keep lines under 100 characters where practical

## Testing

Always test changes in QEMU before submitting:

```bash
make run144          # Test floppy boot
make run-hd          # Test HD boot
```

If you have real vintage hardware, testing there is even better. Report the hardware model and results.

## Submitting Changes

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test in QEMU (both floppy and HD boot if applicable)
5. Submit a pull request with a clear description

## Build Numbers

The `BUILD_NUMBER` file tracks sequential builds. Don't increment it in your PR — the maintainer handles build numbering.

## License

By contributing, you agree that your contributions will be licensed under the project's [CC BY-NC 4.0](LICENSE) license.
