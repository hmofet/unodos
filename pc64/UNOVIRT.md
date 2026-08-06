# unovirt, the subsystem contract

The appliance machinery: can this machine host a guest, and later, the guest
itself. The programme and its phases are `docs/UNOVIRT-PLAN.md`; this file is
the API surface, its changelog, and the things a consumer has to know.

**Status: A0 [implemented]. A1 [implemented on VMX, unproved on SVM].** The
capability gate runs on every boot. The foothold is real on Intel: on devbuntu
(bare metal, nested KVM) UnoDOS enters VMX operation, runs a guest, takes it
through a CPUID intercept, reads its marker back out of guest memory, and then
runs a guest that destroys itself and carries on booting. The AMD backend is
written and compiles but its first VMRUN has never returned, on a box that is
two levels of virtualization down (see "The A1 wedge"). The selftest stays
**opt-in** either way. Nothing is carved and there is no EPT/NPT yet.

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
| QEMU KVM `-cpu host` on **devbuntu** (Haswell, bare metal) | vmx rev 0x11e57ed0, ept wb 2m 1g unrestricted vpid preempt, phys 39, 3887 MB | **yes**, carve 1536 MB, **and A1 passes** |
| ZimaBlade, X1 Carbon | pending | pending |

The Intel arm now has real numbers, and the revision id is the tell that they
are KVM's rather than Haswell's: `0x11e57ed0` is literally KVM's
`VMCS12_REVISION`. Metal is still outstanding, and it is the run that decides
whether `FEATURE_CONTROL` is locked on either box.

## A1: the foothold, and why it is opt-in

`uno_vmm_selftest()` enters host virtualization mode (VMX or SVM, whichever
this machine has), runs a guest that takes a
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

### A1 on Intel: what the run proves

    [hv] vmxon ok, rev=0000000011e57ed0
    [hv] vmentry rip=0000000142d31000   exit reason=a   (CPUID)
    [hv] vmentry rip=0000000142d31002   exit reason=c   (HLT)
    [hv] vmentry rip=0000000142d31000   exit reason=2   (triple fault: the crasher)
    selftest: vmx: entered, guest round trip -> 534f444f4e55 OK, crasher contained

Three separate claims, each with its own evidence rather than one summary:

- **The guest ran and we answered it.** Exit reason 10 is CPUID; the value we
  handed back arrived at `0x534f444f4e55` in GUEST memory, written by the
  guest's own store through its own page tables.
- **It resumed correctly.** The second entry is at RIP + 2, and reason 12 is
  HLT - which the guest only reaches by executing the two stores after the
  intercepted instruction. Getting the RIP advance wrong does not produce a
  wrong answer, it produces a guest that executes CPUID forever.
- **A guest that destroys itself takes only itself.** Reason 2 is a triple
  fault, and the boot log continues past it into the network self-test, which
  runs from the shell's frame loop. The desktop was alive on the far side.

Run it again with `python3 tools/hv_remote.py devbuntu.local`.

**One bug worth keeping.** The first attempt entered with guest RIP 0, because
the SVM backend runs its guest from offset 0 with a real-mode CS base pointing
at the code. A long-mode guest has no segment base to fold in: CS.base must be
0, so RIP *is* the linear address. Entering at 0 mapped nothing, the fetch
faulted into an IDT that cannot deliver, and it presented as exit reason 2 at
RIP 0 - indistinguishable from a guest that never started.

### The A1 wedge on AMD, stated as what is and is not known

**Known:** on amanuensis (Ryzen 5 5600G) under QEMU/KVM, `EFER.SVME` sets, the
host save area is accepted, and the first `vmrun` never returns. Reproducible,
every run. No fault is taken (our IDT is installed by then and no crash report
is written), QEMU stays alive, and the boot simply ends there.

**Not known, but now less so:** whether that is a defect in this code or a
limit of the test environment. The VMX backend, built on the same seam, the
same vCPU stub pattern and the same selftest, works one level down - so the
shape of all of that is right, and the environment is the stronger suspect.
It is not proof: the two backends share no vendor-specific code. The nesting on this box is three deep - Hyper-V hosts WSL2, KVM
runs inside WSL2, UnoDOS runs under KVM, and UnoDOS's guest would be a fourth
level. Nested SVM under an already-nested KVM is not a configuration anyone
supports, and a wedged vCPU is a plausible way for it to decline.

**What settles it:** an AMD machine where KVM is at L0, or AMD metal. Neither
is on the LAN today (devbuntu is Intel), so the SVM row stays unproved and this
file says so rather than borrowing the VMX result.

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
  `vm-selftest`. Unproved on AMD: see the wedge above.
- **2026-08-06, API 1.** A1 PASSES on VMX (`hv_vmx.c`), on devbuntu under KVM
  at L0: entered, round trip, crasher contained, boot continued. `tools/
  hv_remote.py` runs it.
