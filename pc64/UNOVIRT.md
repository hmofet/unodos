# unovirt, the subsystem contract

The appliance machinery: can this machine host a guest, and later, the guest
itself. The programme and its phases are `docs/UNOVIRT-PLAN.md`; this file is
the API surface, its changelog, and the things a consumer has to know.

**Status: A0 [implemented]. A1 [written, not proved].** The capability gate is
in and runs on every boot. The SVM backend, the vCPU context and the marker
guest are written and compile, but the first VMRUN has never returned on the
only machine available to run it (see "The A1 wedge" below), so the selftest is
**opt-in** and off by default. Nothing is carved and there is no NPT yet.

## API (`unovirt.h`, `UNO_VIRT_API 1`)

| Call | Answers |
|---|---|
| `uno_vmm_probe()` | everything CPUID and the capability MSRs say, latched |
| `uno_vmm_eligible(&mask)` | could an appliance run RIGHT NOW; `UNO_VMB_*` either way |
| `uno_vmm_blocker_str(mask)` | the one sentence worth telling the operator |
| `uno_vmm_carve_mb()` | the guest carve this machine would get, 0 for none |
| `uno_vmm_status_str(buf, cap)` | the two/three-line `HV:` block for the env block and the System window |
| `uno_vmm_selftest()` | A1: enter host mode, run the marker guest, then a guest that destroys itself. **Opt-in**: DEBUG.CFG `vm-selftest` |
| `uno_vmm_selftest_str()` | what it found, for the env block |

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

## A1: the foothold, and why it is opt-in

`uno_vmm_selftest()` enters host SVM operation, runs a guest that takes a
64-bit value through a CPUID intercept, writes it to its own memory and halts,
then runs a second guest that destroys itself on purpose. The evidence is
deliberately the value in GUEST memory rather than the exit codes: a guest that
never ran leaves it zero, and a hypervisor that failed to advance RIP past the
intercepted instruction never reaches the store at all.

It runs **only** when DEBUG.CFG carries `vm-selftest`, following the `mtrr-wc`
precedent in `pc64_mtrr.c`: a change that can take the machine out runs with an
operator present who asked for it. The risk is specific. A VMRUN that does not
return leaves the core with **GIF clear**, and with GIF clear nothing at all is
delivered to that core, including the LAPIC watchdog that exists to catch a
hang. There is no fault, no report, no reset and no screen. The machine stops
being a machine.

### The A1 wedge, stated as what is and is not known

**Known:** on amanuensis (Ryzen 5 5600G) under QEMU/KVM, `EFER.SVME` sets, the
host save area is accepted, and the first `vmrun` never returns. Reproducible,
every run. No fault is taken (our IDT is installed by then and no crash report
is written), QEMU stays alive, and the boot simply ends there.

**Not known:** whether that is a defect in this code or a limit of the test
environment. The nesting on this box is three deep - Hyper-V hosts WSL2, KVM
runs inside WSL2, UnoDOS runs under KVM, and UnoDOS's guest would be a fourth
level. Nested SVM under an already-nested KVM is not a configuration anyone
supports, and a wedged vCPU is a plausible way for it to decline.

**Two experiments settle it, in order of cost:**

1. Write the VMX backend and run it on **devbuntu**, which is bare metal
   (`systemd-detect-virt` says none), Intel, with `kvm_intel nested=Y`. That is
   KVM at L0, one level of nesting, the configuration nested virtualization is
   actually tested in. It also exercises the arm both metal boxes need.
2. Boot the branch on the **ZimaBlade** with `vm-selftest` set. That is the real
   answer, and it is the one that has to be true anyway.

Until one of those runs, A1 is written and unproved, and this file says so.

### Two findings from the bring-up, kept because both cost time

**The VMCB intercept words are at 0x00C and 0x010, not 0x010 and 0x014.**
0x008 is the exception bitmap. Writing the instruction words four bytes early
installs no CPUID, HLT or SHUTDOWN intercept while *accidentally* setting the
VMRUN intercept the consistency check demands, so VMRUN succeeds, the guest
runs, and it halts with nothing to catch it and GIF clear. The symptom is
indistinguishable from the wedge above, which is why finding it did not end the
investigation.

**DEBUG.CFG is read into a 512-byte buffer** (`pc64_stress.c dbg_cfg_read`) and
the shipped file is already 536 bytes of comments. A key appended to the end of
it is **silently ignored** - it never reaches the parser. `vm-selftest` has to
go near the top. That is the debug harness's lane, reported in
`pc64/UNOAUTOMATE-REQUESTS.md` rather than fixed here.

## Changelog

- **2026-08-06, API 1.** A0: the capability gate. `uno_vmm_probe`,
  `uno_vmm_eligible`, `uno_vmm_blocker_str`, `uno_vmm_carve_mb`,
  `uno_vmm_status_str`. Contracts S-HV-01..11.
- **2026-08-06, API 1.** A1 written: the `uno_hv_t` backend seam, the SVM
  backend (`hv_svm.c`), the vCPU register context, the marker guest and the
  crasher. `uno_vmm_selftest` / `uno_vmm_selftest_str`, opt-in behind DEBUG.CFG
  `vm-selftest`. Unproved: see the wedge above.
