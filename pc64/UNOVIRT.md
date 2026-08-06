# unovirt, the subsystem contract

The appliance machinery: can this machine host a guest, and later, the guest
itself. The programme and its phases are `docs/UNOVIRT-PLAN.md`; this file is
the API surface, its changelog, and the things a consumer has to know.

**Status: A0 [implemented].** Capability gate only. Nothing enters VMX or SVM
operation, nothing is carved, no guest exists.

## API (`unovirt.h`, `UNO_VIRT_API 1`)

| Call | Answers |
|---|---|
| `uno_vmm_probe()` | everything CPUID and the capability MSRs say, latched |
| `uno_vmm_eligible(&mask)` | could an appliance run RIGHT NOW; `UNO_VMB_*` either way |
| `uno_vmm_blocker_str(mask)` | the one sentence worth telling the operator |
| `uno_vmm_carve_mb()` | the guest carve this machine would get, 0 for none |
| `uno_vmm_status_str(buf, cap)` | the two-line `HV:` block for the env block and the System window |

`probe` is latched; `eligible` is live, because detaching from the firmware
changes the answer and nothing else does.

## Three things a consumer must not assume

**Eligible does not mean it works.** The gate reads what the machine says about
itself, and no architectural read distinguishes "advertises VMX" from "VMXON
succeeds". A CPU model under QEMU/TCG advertises SVM and NPT and can host
nothing. Discovering the difference is A1's job, by trying it once and latching
the result; until then `eligible` means "worth trying".

**The RAM figure is free conventional memory, not installed RAM.** It is always
lower, sometimes by hundreds of MiB, and it is latched during init because the
UEFI memory map does not survive ExitBootServices. The carve thresholds
(1800 / 3500 / 7000 MiB) are set against that measurement, not against the
round numbers in the plan's table.

**`preempt_timer` is Intel-only and its absence is not a defect.** SVM has no
VMX-preemption-timer equivalent; the slice clock on AMD will be an intercepted
local-APIC one-shot (A3). The field stays 0 rather than being fudged to 1,
because A3 has to branch on it.

## Why the probe is inert

It executes no `wrmsr`, no control-register write, no `vmxon` and no `vmrun`,
and it reads a capability MSR only under the CPUID bit that makes that MSR
architectural (S-HV-02, S-HV-03). Two reasons, and the second is the real one:

- A machine that cannot host an appliance is the NORMAL case. The commonest
  cause by far is a firmware that turned virtualization off in setup and set
  the lock bit, which no software can undo. That machine must boot exactly as
  it did before, with one honest line in its log.
- CPUID.1:ECX[5] still reads 1 on that machine. The feature is in the silicon;
  it is `IA32_FEATURE_CONTROL` that says it may not be used. A gate that
  stopped at CPUID would report "supported" and then #GP on VMXON, which is
  the single most common way this goes wrong.

## Where it is called from, and what it costs

One call during init in `uefi_main.c`, before the pre-detach telemetry, because
the RAM figure needs boot services alive. One line in the env block
(`uno_debug.c`), so every boot log on every machine carries it. Nothing else in
the OS depends on it yet. Cost is a few dozen CPUID and RDMSR instructions,
once.

## Testing

`python3 tools/hv_test.py` boots the debug build under five QEMU CPU models and
checks that each arm of the gate is reached, that no arm faults, and that no
refusal is unattributed. `--kvm` boots one machine with `-enable-kvm -cpu host`
so the capability MSRs are the host's own.

What the harness cannot prove, and does not claim to: that VMXON works, that
second-stage tables translate, or that a guest runs. TCG silently drops `+vmx`
(a stderr warning nobody reads), so **the Intel arm is untested under TCG** and
gates on `--kvm` on an Intel box or on metal.

### Results so far

| Machine | Reads | Verdict |
|---|---|---|
| QEMU TCG `qemu64` | svm rev 1, no NPT | no: no EPT/NPT |
| QEMU TCG `EPYC` / `max` | svm rev 1, npt wb 2m 1g | no: still attached |
| QEMU TCG `Nehalem[,+vmx]` | neither (TCG drops vmx) | no: no VMX or SVM |
| QEMU KVM `-cpu host` (Ryzen 5 5600G, WSL2 nested) | svm rev 1, npt wb 2m 1g, phys 48, 3889 MB | **yes**, carve 1536 MB (UNO_DETACH=1 build) |
| ZimaBlade, X1 Carbon (Intel, the VMX arm) | pending | pending |

The Intel arm has never run. Both metal boxes are Intel, so the first hardware
boot of this branch is also the first execution of half the file, and the
thing to watch for is the `HV:` line naming `vmx` with a plausible EPT
capability set rather than a locked `FEATURE_CONTROL`.

## Changelog

- **2026-08-06, API 1.** A0: the capability gate. `uno_vmm_probe`,
  `uno_vmm_eligible`, `uno_vmm_blocker_str`, `uno_vmm_carve_mb`,
  `uno_vmm_status_str`. Contracts S-HV-01..11.
