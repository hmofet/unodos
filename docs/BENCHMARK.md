=== UnoDOS Build Benchmark ===
Machine: control
Date: 2026-01-28T22:32:13+00:00
CPU: Intel(R) Xeon(R) CPU E5-2695 v4 @ 2.10GHz
Cores: 8

### Build Times

| Target | Time |
|--------|------|
| floppy144 (clean) | .199012712s |
| hd-image (clean) | .492853339s |
| apps (incremental) | .038789838s |
| all targets (clean) | .535890773s |

### Notes
- floppy144: Boot sector + stage2 + kernel assembly
- hd-image: MBR + VBR + stage2_hd + kernel + Python FAT16 image creation
- apps: 5 application binaries (hello, clock, launcher, browser, mouse_test)
