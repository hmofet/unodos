#!/bin/bash
# UnoDOS Build Benchmark Script
# Measures build time for all targets

echo "=== UnoDOS Build Benchmark ==="
echo "Host: $(hostname)"
echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "Cores: $(nproc)"
echo "Date: $(date)"
echo ""

# Array to store results
declare -A times

# Function to benchmark a target
benchmark_target() {
    local target=$1
    local label=$2

    echo -n "Benchmarking $label... "
    make clean &>/dev/null

    local start=$(date +%s.%N)
    make $target &>/dev/null
    local end=$(date +%s.%N)

    local elapsed=$(echo "$end - $start" | bc)
    times[$label]=$elapsed
    echo "${elapsed}s"
}

# Run benchmarks
benchmark_target "boot" "Boot sector"
benchmark_target "stage2" "Stage2"
benchmark_target "mbr" "MBR"
benchmark_target "vbr" "VBR"
benchmark_target "stage2_hd" "Stage2 HD"
benchmark_target "kernel" "Kernel"
benchmark_target "floppy144" "Floppy 1.44MB"
benchmark_target "hd-image" "HD Image"
benchmark_target "apps" "Apps"

# Full build benchmark
echo -n "Benchmarking Full clean build... "
make clean &>/dev/null
start=$(date +%s.%N)
make floppy144 &>/dev/null
make apps &>/dev/null
make build/launcher-floppy.img &>/dev/null
make hd-image &>/dev/null
end=$(date +%s.%N)
total_elapsed=$(echo "$end - $start" | bc)
times["Full build"]=$total_elapsed
echo "${total_elapsed}s"

echo ""
echo "=== Results Summary ==="
printf "%-25s %10s\n" "Target" "Time (s)"
printf "%-25s %10s\n" "-------------------------" "----------"
for key in "${!times[@]}"; do
    printf "%-25s %10s\n" "$key" "${times[$key]}"
done | sort

echo ""
echo "Total time: ${total_elapsed}s"
