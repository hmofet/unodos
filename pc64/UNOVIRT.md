# unovirt, the subsystem contract

The appliance machinery: can this machine host a guest, and later, the guest
itself. The programme and its phases are `docs/UNOVIRT-PLAN.md`; this file is
the API surface, its changelog, and the things a consumer has to know.

**Status: A0 through A5 [implemented on VMX]. A1 [unproved on SVM].** The
capability gate runs on every boot. The foothold is real on Intel: on devbuntu
(bare metal, nested KVM) UnoDOS enters VMX operation, runs a guest, takes it
through a CPUID intercept, reads its marker back out of guest memory, and then
runs a guest that destroys itself and carries on booting. The AMD backend is
written and compiles but its first VMRUN has never returned, on a box that is
two levels of virtualization down (see "The A1 wedge"). The selftest stays
**opt-in** either way. The carve is taken at detach and the guest now runs
behind EPT, in an address space of its own.

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
| `uno_vmm_reserve()` | A2: take the carve before ExitBootServices, beside the module arena |
| `uno_vmm_carve_base/size()` | where it is and how big, 0 for none |
| `uno_vmm_gpa(gpa, len)` | **the security boundary**: a guest address the host may touch, or NULL |
| `uno_vmm_tick()` | A3: one budgeted slice of any running guest, once per shell frame |
| `uno_vmm_slice_str()` | what the slice test measured |

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

## A2: the guest gets an address space

    [hv] vmentry rip=0000000000010000   exit reason=a   (CPUID)
    [hv] vmentry rip=0000000000010002   exit reason=c   (HLT)
    ept OK gpa 0x100000 -> pa 1bf00000 wrote 534f444f4e56
        (memtype 6, 1536 MB at 1be00000)

**The pair of addresses is the point.** The guest stored at guest-physical
`0x100000` and the value turned up at host-physical `0x1bf00000`. Translation
observed, not assumed. And the guest's RIP is now `0x10000` rather than a host
address in the hundreds of megabytes, because it is running in a small tidy
machine with RAM at the bottom and nothing else in it.

Two details that make the evidence hold:

- **The value is the marker PLUS ONE**, so a buffer that happened to contain
  the right bytes cannot pass for a guest that ran.
- **Had the second stage been quietly off, the store would have gone to host
  physical `0x100000`** - the kernel's own low memory - and nothing would have
  appeared where we looked. The address was chosen for that.

The carve is `EfiLoaderData` taken beside the module arena in `try_detach`,
because `AllocatePages` does not exist afterwards. It halves down to a 128 MiB
floor rather than failing: firmware that will not part with 1.5 GiB will often
part with 512 MiB, and a smaller appliance beats none. The BIOS path gets no
carve at all, and says so - `uno_bios_find_ram()` keeps no bookkeeping, so a
second consumer would be handed memory the module arena is already using.

**`uno_vmm_gpa()` is the only place a guest address becomes an address this
machine will touch.** Stage two bounds the guest's OWN accesses and does
nothing whatever for the ones the host makes on its behalf - and every address
in a virtqueue will have come from the guest, including the ones inside
descriptors. It checks address and length together, because checking them
separately is how an overflow gets through (S-HV-15).

### Two findings

**A 2 MiB second-stage leaf must be 2 MiB aligned**, and `AllocatePages` only
promises 4 KiB. An unaligned frame address is a reserved-bit violation, and
the machine reports it as **EPT misconfiguration (exit 49)** - a number that
says the tables are malformed and nothing about which field. The reservation
over-allocates by 2 MiB and rounds up.

**Only whole frames inside the carve are mapped.** Rounding the mapping up to
the next 2 MiB would hand the guest whatever follows the carve in host memory,
and it would work perfectly in every test.

## A3: how a guest is scheduled on an OS with no scheduler

    vm slice: 120 slices x 4000 us budget: max 4811 us, mean 4025 us,
              0 exits that were not the clock

The guest is two bytes, `jmp $`. It makes no hypercall, takes no fault and
touches no device, so **nothing it does can end its turn** - which is the
property being tested. It does not get scheduled; it gets a budget out of a
frame the shell was going to spend anyway, and the machine takes the core back
whether or not it cooperates.

- **mean 4025 us against a 4000 us budget** is the preemption timer being
  accurate to half a percent, which means the budget is a real control and not
  an aspiration.
- **max 4811 us** is the worst slice over 120, comfortably under the 100 ms
  that `pc64/DEBUG.md` counts as a hitch.
- **0 exits that were not the clock** is the load-bearing number. Every single
  slice ended because the machine ended it.

`shots/hv_slice.png` is the other half, and it is the half a log cannot show:
the desktop mid-run, Control Panel open, HUD reading 0.5 ms render and 93%
idle, zero crash reports, while the guest spins in the gaps between frames.

**Why the budget is a correctness constraint rather than a tuning knob.** The
watchdog fires on a heartbeat 20 s stale and anything over 100 ms is a logged
hitch, so a slice has to fit inside a frame with room left for the desktop to
draw. 4 ms of a 16 ms frame is about a quarter of a core, and that is the
honest number for a single-core appliance until A9 gives a guest a core of its
own.

**Two mechanisms bound a slice, and the second is not redundancy.** The
preemption timer ends it on time; external-interrupt exiting means a host
interrupt arriving mid-slice ends it too, rather than being delivered through
the guest's IDT - which this guest does not have, and which would turn every
timer tick into a triple fault.

### The defect this phase found in the last one

A VM exit loads host RFLAGS with `0x2`. Every flag clear, **IF included**. The
A1 and A2 stubs did not restore them, so from the first guest onwards the host
ran with interrupts disabled - and nothing in this OS is interrupt-driven
except the LAPIC watchdog, so the machine looked perfectly healthy while the
one mechanism that exists to catch a hang was dead. `pushfq`/`popfq` now
bracket the entry (S-HV-23).

It is worth noticing what made this findable: A3 is the first phase whose
subject is the machine still working afterwards, rather than a value arriving.

## A4: the three things that make a guest an operating system

    clock ticks (+59145778 over 140 exits), irq taken once (1, not
    redelivered), msr answered

A guest needs a clock it can read, an interrupt it can take, and an MSR space
somebody answers, and Linux asks for all three in its first milliseconds. One
guest of eighteen instructions exercises all three, and the SHAPE of each proof
matters more than the number.

- **The clock is sampled across two slices, not within one.** `rdtsc` is not
  intercepted, so the guest reads the real counter directly; the claim being
  tested is that it keeps making progress, which sampling inside a single slice
  would not show. 59 million cycles is about 17 ms of guest time.
- **The interrupt is counted by the guest**, in its own memory, by a handler
  reached through its own IDT. Nothing about the delivery is taken on trust,
  and the count is checked AGAIN after forty more entries: an interrupt
  delivered forever and one never delivered look identical for the first
  millisecond. The CPU clears the entry-interruption valid bit itself, and this
  is what proves it.
- **The MSR read is answered by us.** The guest reads an index nothing real
  uses, we put a marker in its EDX, and it stores what it got.

**One thing here is right for this guest and wrong for Linux.** With no MSR
bitmap configured, every MSR access exits. That is fine for eighteen
instructions and unworkable for a kernel that reads MSRs constantly, so the
bitmap belongs with the virtio work (S-HV-26).

## A5: a device, and the thing x86 does not give you

    virtio OK magic 74726976, used.idx 1, 29 bytes in 1 notify,
    cycle refused, said "a message through a virtqueue"

Four claims: the guest read the transport's identity register, its doorbell
write reached the device, the device walked a descriptor chain in guest memory
and consumed exactly 29 bytes, and **the guest read `used.idx` back out of its
own memory** - so the device's write landed where a driver would look for it.

**The structural difference from Glide, and it is the big one.** On ARM a
stage-2 abort hands the hypervisor a syndrome naming the access size, the
direction and which register the guest used; trap-and-emulate is a switch on
those fields. An EPT violation gives the faulting address, a direction bit,
and nothing else. The register and the operand size are only knowable by
**decoding the instruction**, which is why KVM carries an x86 emulator.

What keeps that from becoming an emulator here is scope: a driver's MMIO
accesses are `mov` between a register and memory, because that is what `readl`
and `writel` compile to. So `decode_mov` handles exactly `88`/`89`/`8A`/`8B`
with their prefixes and refuses everything else **loudly** - an unrecognised
opcode ends the guest with a report rather than a guessed answer. The
instruction length does not have to be decoded at all: VMX supplies it.

`guest_walk` turns a guest linear address into a guest-physical one through
the guest's own page tables, which is what makes the decoder work for a guest
that is not identity-mapped. A1..A5's guests are; Linux is not.

### The bug, and why it looked like something else

The guest's first device access triple-faulted at an address unrelated to the
device. The device is at 3.25 GiB and **the guest's own page tables mapped
only the first gibibyte** - so the access faulted in stage ONE, into an IDT
the guest had not built, before stage two ever saw it. A device access is
meant to fault in stage two; when it faults in stage one instead, nothing in
the report mentions the device.

### The security posture is the point of `unovdev.c`

Every address in a virtqueue came from the guest, including the ones inside
descriptors, and stage two does nothing about them: it bounds the guest's own
accesses, not the ones we make on its behalf. So every guest address goes
through `uno_vmm_gpa()`, and every chain walk is bounded by the queue size.
`cycle refused` is that second rule under test: a descriptor whose `next`
points at itself is one store by the guest, and a device that trusts a chain
to terminate hangs the machine on request.

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
- **2026-08-06, API 1.** A5: `unovdev.c`/`unovdev.h` (virtio-mmio transport,
  console device, bounded ring walk), plus MMIO decode and a guest page-table
  walk in the VMX backend. Contracts S-HV-27..30.
- **2026-08-06, API 1.** A4: the `clockirq` backend entry - guest TSC across
  slices, interrupt injection through the guest's own IDT, and MSR exits
  answered. Contracts S-HV-24..26.
- **2026-08-06, API 1.** A3: `uno_vmm_tick` / `uno_vmm_slice_str`, the
  preemption-timer slice, and the RFLAGS fix in the entry stub. Contracts
  S-HV-20..23.
- **2026-08-06, API 1.** A2: `uno_vmm_reserve`, `uno_vmm_carve_base/size`,
  `uno_vmm_gpa`, and EPT in the VMX backend. The guest runs at low
  guest-physical addresses in a 1536 MB write-back carve. Contracts
  S-HV-15..19.
