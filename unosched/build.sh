#!/bin/sh
# Concurrency stress pass (CONTRACT-ARCH Phase 7 / §10 / §15.2). Same workload as
# COOP, SMP+TSan (guarded), SMP+TSan+race. Checks: COOP==SMP==expected; guarded SMP
# is TSan-clean; the unguarded build is caught by TSan. (TSan needs ASLR off here:
# run under `setarch -R`.)
set -e
CC="${CC:-gcc}"; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"; mkdir -p build
F="-std=c11 -Wall -Wextra -I unosched"
R="setarch -R"; command -v setarch >/dev/null 2>&1 || R=""
fail=0

$CC $F unosched/uno_sync.c unosched/unosched_test.c -o build/sched_coop
COOP_OUT=$(./build/sched_coop); echo "$COOP_OUT"
echo "$COOP_OUT" | grep -q "\[OK\]" || { echo "[FAIL] COOP result"; fail=1; }

$CC $F -DUNO_SMP -fsanitize=thread -O1 unosched/uno_sync.c unosched/unosched_test.c -lpthread -o build/sched_smp
SMP_OUT=$($R ./build/sched_smp 2>build/sched_smp.tsan); echo "$SMP_OUT"
echo "$SMP_OUT" | grep -q "\[OK\]" || { echo "[FAIL] SMP result"; fail=1; }
if grep -q "ThreadSanitizer" build/sched_smp.tsan; then echo "[FAIL] guarded SMP tripped TSan"; fail=1; else echo "[PASS] guarded SMP is TSan-clean"; fi

$CC $F -DUNO_SMP -DUNO_RACE -fsanitize=thread -O1 unosched/uno_sync.c unosched/unosched_test.c -lpthread -o build/sched_race
$R ./build/sched_race >/dev/null 2>build/sched_race.tsan || true
if grep -q "data race" build/sched_race.tsan; then echo "[PASS] DISCRIMINATION: TSan caught the unguarded race"; else echo "[FAIL] TSan did not catch the race"; fail=1; fi

echo ""; [ $fail -eq 0 ] && echo "ALL PASS — COOP result == SMP result == expected; guarded clean; race caught" || echo "FAILURES"
exit $fail
