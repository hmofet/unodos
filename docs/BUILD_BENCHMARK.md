# UnoDOS Build Performance Benchmark

**Date:** 2026-01-28
**Build:** 072

## Machine Specifications

| Machine | Hostname | CPU | Cores | Clock Speed | Location |
|---------|----------|-----|-------|-------------|----------|
| **Current** | macpro | Intel Xeon E5-1650 v2 | 12 | 3.50GHz | Local development |
| **Previous** | control | Intel Xeon E5-2695 v4 | 8 | 2.10GHz | 192.168.2.108 |

## Build Performance Results

| Target | macpro (current) | control (previous) | Speedup |
|--------|------------------|--------------------|---------|
| Boot sector | 5.6ms | 13.6ms | **2.4x** |
| Stage2 | 5.5ms | 12.7ms | **2.3x** |
| MBR | 5.8ms | 12.9ms | **2.2x** |
| VBR | 6.7ms | 12.8ms | **1.9x** |
| Stage2 HD | 5.7ms | 13.1ms | **2.3x** |
| Kernel | 5.1ms | 13.1ms | **2.6x** |
| Floppy 1.44MB | 14.0ms | 200.3ms | **14.3x** |
| HD Image | 13.4ms | 550.8ms | **41.1x** |
| Apps | 7.1ms | 102.0ms | **14.4x** |
| **Full build** | **33.7ms** | **690.8ms** | **20.5x** |

### Detailed Results

#### macpro (Current Development Machine)
```
=== UnoDOS Build Benchmark ===
Host: macpro
CPU: Intel(R) Xeon(R) CPU E5-1650 v2 @ 3.50GHz
Cores: 12
Date: Wed Jan 28 05:49:33 PM EST 2026

Apps                      .007085574s
Boot sector               .005556160s
Floppy 1.44MB             .013991885s
Full build                .033671244s
HD Image                  .013380030s
Kernel                    .005101393s
MBR                       .005841748s
Stage2                    .005531825s
Stage2 HD                 .005669511s
VBR                       .006679544s

Total time: .033671244s
```

#### control (Previous Development Machine - 192.168.2.108)
```
=== UnoDOS Build Benchmark ===
Host: control
CPU: Intel(R) Xeon(R) CPU E5-2695 v4 @ 2.10GHz
Cores: 8
Date: Wed Jan 28 10:50:55 PM UTC 2026

Apps                      .101992681s
Boot sector               .013572165s
Floppy 1.44MB             .200322553s
Full build                .690838808s
HD Image                  .550769761s
Kernel                    .013053644s
MBR                       .012887550s
Stage2                    .012715953s
Stage2 HD                 .013136715s
VBR                       .012766053s

Total time: .690838808s
```

## Analysis

### Performance Categories

1. **Assembly Compilation** (Boot sector, Stage2, MBR, VBR, Kernel)
   - Average speedup: **2.3x**
   - Bottleneck: CPU clock speed (3.50GHz vs 2.10GHz = 1.67x difference)
   - Single-threaded NASM assembly compilation

2. **Disk Image Creation** (Floppy, HD Image)
   - Floppy speedup: **14.3x**
   - HD Image speedup: **41.1x**
   - Bottleneck: Disk I/O + Python script execution
   - Likely benefits from faster storage and higher clock speed

3. **Multi-File Compilation** (Apps)
   - Speedup: **14.4x**
   - Benefits from parallel make execution and higher core count

### Key Findings

1. **Overall Development Speed**: macpro is **20.5x faster** for complete builds
2. **Clock Speed Impact**: Higher frequency (3.50GHz) provides 40-60% boost for single-threaded tasks
3. **Disk I/O Impact**: Image creation shows dramatic differences (41x for HD image)
4. **Core Count**: 12 cores vs 8 cores helps with parallel compilation

### Recommendations

- **Primary Development**: Continue using macpro (current machine)
- **CI/CD**: macpro provides much faster iteration cycles
- **Bottleneck**: Disk image creation dominates build time on slower machines

### Build Time Impact

For a typical development session with 10 build-test cycles:
- **macpro**: 10 × 33.7ms = 337ms (~0.3 seconds)
- **control**: 10 × 690.8ms = 6.9 seconds

The macpro machine saves **6.5 seconds per 10 builds**, making rapid iteration much more efficient.

---

**Benchmark Script:** `benchmark_build.sh`
**Test Method:** Each target built from clean state, averaged single run
**Build System:** GNU Make + NASM 2.15+ + Python 3
