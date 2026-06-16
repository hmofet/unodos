# unodef/conformance/ — PORT-SPEC §6 made executable (Phase 2)

`conformance.py` turns the PORT-SPEC §6 "audit-tax" invariants into runnable test
vectors (CONTRACT-ARCH Phase 2 / §15.2). Host-only, stdlib-only:

```
python unodef/conformance/conformance.py    # exit 0 = all conformant
```

## Two layers

- **Structural** — static invariants checked against the Contract, the generated
  x86 surface, and `kernel.asm`:
  - rule 9 (stable/append-only API table): ordinals contiguous, `count == len`, and
    a frozen baseline of anchor ordinals never renumbers.
  - rule 10 (fixed-size tables with explicit "full" behavior): the event queue,
    window table, and handle table declare fixed counts; the queue's full/again
    state is the defined `CONSUMED` tombstone, not UB.
  - rule 8 (centralized disk geometry): one `fat12` block in the Contract, and the
    generated x86 geometry equals the known-good values (the Phase-1 trust anchor,
    re-asserted here).

- **Behavioral** — the §6 policy invariants as **executable reference models +
  golden vectors**: z-order raise/destroy (6), event focus-stamp / stale-discard
  (3), the fixed-size tombstone queue with lazy head advance (10), edge-only mouse
  posting (5), owner-based reaping on kill (7). These models are the host oracle a
  world's implementation is later run against (§15.1).

## Discrimination (the vectors have teeth)

Each behavioral invariant is paired with the **historical buggy behavior** PORT-SPEC
warns about (demote-all z-leak; deliver-events-always; per-sample mouse flood; leak
windows on kill). The runner asserts the reference passes every vector **and** the
buggy version fails at least one — so a passing suite proves the vectors actually
discriminate, not that they are vacuous.

## Coverage

Covered: rules 3, 5, 6, 7, 8, 9, 10. Not host-testable (hardware/timing): rule 1
(atomic cursor hide+lock via IF masking) and part of rule 2 (ISR-defer timing);
rule 4 (press-time latch + sequence counter) is modelable — vectors TODO. As the
host reference build lands (Phase 3 `unofs`) and real ports adopt the Contract,
each plugs its implementation into these same vectors.
