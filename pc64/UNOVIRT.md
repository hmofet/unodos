# unovirt, the subsystem contract

The appliance machinery: can this machine host a guest, and later, the guest
itself. The programme and its phases are `docs/UNOVIRT-PLAN.md`; this file is
the API surface, its changelog, and the things a consumer has to know.

**Status: A0 through A6 [implemented on VMX]. A7a [virtio-blk: the guest
mounts a disk]. A1 [unproved on SVM].** A real Ubuntu kernel boots under
UnoDOS, reaches userspace, **its shell reads a command and answers**, and it
**mounts an ext4 filesystem served from a file on a UnoDOS volume**. The
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

## The backend seam, and the mistake it made first

`uno_hv_t` (`unovirt_hv.h`) is the vendor split, and it is now the small generic
interface `docs/UNOVIRT-PLAN.md` §3.1 asked for: `enable`, `vcpu_create`,
`vcpu_run`, `map`, `inject`, and a `get`/`set` window on the vCPU state an exit
handler has to touch. Exactly two files implement it, `hv_vmx.c` and `hv_svm.c`.

**It used to carry one pointer per PHASE** - `marker`, `crasher`, `ept`,
`spin_start`, `slice`, `clockirq`, `virtio`, `linux_boot`, `linux_slice` - and
every one of those was a *test*. A test behind a vendor seam is a test written
twice, so the SVM backend could not reach A2 without reimplementing nine guests
that have nothing vendor-specific about them: the guests are x86 machine code,
which is the same machine code on both, and servicing their exits is x86
register work, which is the same work. That is why `hv_svm.c` sat at A1.

The phases are CALLERS of the seam now, in `hv_phases.c`, and they compile once.
So does the MMIO instruction decoder, the guest page-table walk, the Linux boot
protocol and every hand-assembled guest. A backend owes seven operations and
gets A1..A6; what `hv_svm.c` still owes is exactly one of them, `map`, and its
being NULL is the question "has this backend a second stage yet" asked in the
only place that can answer it.

Two things are worth knowing before writing a third backend:

- **`vcpu_create` refuses a configuration it cannot host**, and A1 uses that:
  it asks for a flat 64-bit guest first and falls back to a real-mode one.
  Intel needs the "unrestricted guest" control to run a guest with paging off
  and that control needs EPT, which would drag A2 into A1; AMD needs nothing.
  So the same phase runs both arrangements without either backend pretending.
- **`vcpu_run` returns 1 for "the entry was attempted"**, not for "it went
  well". A machine that refuses the guest state is `UNO_VX_INVALID` in the
  exit, which is a result the caller reports rather than a failure it retries.

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

**DEBUG.CFG was read into a 512-byte buffer** (`pc64_stress.c dbg_cfg_read`)
while the shipped file was already 536 bytes of comments, so a key appended to
the end was **silently ignored** - it never reached the parser. `vm-selftest`
had to go near the top. Reported to the debug-harness lane rather than fixed
here, and **fixed there on 2026-08-02**: the reader takes the whole file and
says so when it does not fit. Keys still go at the top out of habit.

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

## A6: Linux boots, and says so

The criterion named a shell on virtio-console; what it has is a shell on the
8250, which is the same claim through the device the kernel could actually
talk to from its first millisecond. The sections below are the order it was
reached in, because each fault was further in than the last.

    linux SPOKE: 16870 KB, 1542 chars 24 lines over 3666 exits, 3100 pio

    KASLR disabled: 'nokaslr' on cmdline.
    [    0.000000] Linux version 7.0.0-28-generic (buildd@lcy02-amd64-047) ...
    [    0.000000] Command line: earlyprintk=serial,ttyS0,115200 console=ttyS0
                   nolapic no_timer_check panic=-1 nokaslr
    [    0.000000] BIOS-provided physical RAM map:
    [    0.000000] BIOS-e820: [mem 0x0000000000000000-0x000000000009fbff] System RAM
    [    0.000000] BIOS-e820: [mem 0x0000000000600000-0x0000000000ffffff] device reserved
    [    0.000000] BIOS-e820: [mem 0x0000000001000000-0x000000005fffffff] System RAM
    [    0.000000] printk: legacy bootconsole [earlyser0] enabled
    [    0.000000] NX (Execute Disable) protection: active
    [    0.000000] DMI not present or invalid.

Every line of that is a claim about this hypervisor rather than about Linux.
It read the command line we placed, so `boot_params` was accepted. It echoed
back the **e820 map we invented**, our reserved run for the loader's page
tables included. It found the 8250 and enabled a console on it. And it says
DMI is absent, which is true: we provide no SMBIOS. It was still running when
the three-second bound stopped it.

**The x86 equivalent of Glide's device tree is `boot_params`**, and the
comparison holds all the way down: an arm64 kernel is entered with x0 holding
a flattened device tree, an x86-64 kernel with RSI holding a zero page, and
both learn their machine from nothing else.

### Four faults, each one further in

The sequence is the value here, so it is kept:

1. **Triple fault at entry+0x7e.** No vector, no address - the kernel has no
   IDT yet, so every early fault is a triple fault at whatever RIP it reached.
   Trapping #UD/#GP/#PF is what made the next one legible.
2. **`#GP(0)` at the same place, once it could be seen.** VMX requires the
   guest's CR4 to keep VMXE set and Linux does not know it is a guest: it
   writes CR4 with VMXE cleared a hundred bytes into its entry point. **A
   kernel cannot boot without the CR shadow registers.**
3. **`#PF(0)` at guest `0x40e`** - and this one was ours. The decompressor
   **takes page faults on purpose**: its identity map is built on demand and
   its own handler adds the mapping and returns. The bitmap that made fault 2
   visible was stealing an exception the guest handles correctly, and
   reporting the kernel's normal operation as a failure. It is empty now; it
   earned its keep and then got out of the way.
4. **Silence, over 1454 exits, ending in `hlt`.** The tell was in the numbers:
   **zero of those exits were port I/O**, and a booting kernel touches the
   PIT, the CMOS and the PCI config ports constantly. Port I/O does not exit
   unless asked. Without the unconditional-I/O-exiting control the guest's
   `in` and `out` had been executing **natively, against this machine's real
   ports** - so the kernel was not failing to reach its serial port, it was
   reaching the host's.

### A6b: somewhere to run, a clock, and an instruction that was not there

    vm linux: 128 lines 8433 chars, 58689 exits, 57131 pio,
              last "active return thunk: its_return_thunk"

The kernel is out of the selftest and running from `uno_vmm_tick` - a 4 ms
slice per frame, the same budget the spinner gets, for as long as it takes.
Three findings, and the last two are the same lesson twice:

**One VMCS means one guest.** Arming A3's spinner after placing the kernel
vmcleared and reconfigured the block the kernel was using. That does not fail;
it silently replaces a booting Linux with two bytes of `jmp $`, and the only
symptom is a kernel that stops saying anything.

**A kernel calibrates its clock against an 8254.** After "DMI not present" it
sat on port `0x42` forever: `quick_pit_calibrate` watches channel 2 count down,
and a counter that never moves is a loop that never ends. The counter is driven
by the real TSC rather than a tick of our own, which is what makes the
calibration come out right even though the guest only runs in slices - the
kernel reads the same TSC we do, so both sides of its ratio stop and start
together. That took it from 24 lines to 54.

**An instruction the CPU has does not necessarily work in a guest.** The next
stop was `PANIC: early exception 0x06 ... native_flush_tlb_global+0x3c`, and
the faulting bytes were `66 0f 38 82` - INVPCID. It raises #UD in VMX non-root
operation unless a secondary control says otherwise, while CPUID cheerfully
tells the guest it is available. One bit, and 54 lines became 128. It is
exactly the shape of the port-I/O finding in A6a: the default is not "works",
it is "ask first".

### The last four, and the shape they share

    333 lines, ending:
    Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)

The kernel now runs its whole boot and panics for the one honest reason: there
is no root filesystem, because nobody has given it one. Getting from 128 lines
to there took four fixes and three of them are the same fix:

- **XSETBV (exit 55) always exits**, because XCR0 is machine state VMX carries
  in neither direction - a guest that enabled AVX would enable it for the host
  and leave it that way.
- **XSAVE then had to go entirely.** The kernel died in `fpstate_reset` with a
  null pointer, which is what a zero xstate size looks like from the far end:
  advertising XSAVE drags in XCR0, `IA32_XSS` and CPUID leaf 0xD, whose
  answers must agree with each other and with what the hypervisor really
  preserves. Masked out of CPUID, the guest uses FXSAVE, which every x86-64
  CPU has and which needs nothing from us. It costs the guest AVX.
- **The CMOS, and the general lesson.** It then sat on port `0x71` forever.
  The default answer for an absent port was all-ones, and in a STATUS register
  all-ones means every flag is set - including update-in-progress, which the
  kernel spins on until it clears. **0xFF is a dangerous default: absent
  hardware should read as quiet, not as busy.** That is the same failure the
  PIT had, twice in one phase.

The shared shape, across INVPCID, port I/O, XSETBV and the status registers:
**the default is never "works as on real hardware"**. It is either "traps", or
"reads as busy forever", and CPUID will happily tell the guest otherwise.

### Userspace

    Freeing unused kernel image (initmem) memory: 5292K
    Run /init as init process
    Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000

**The kernel reaches userspace.** A 1.1 MB gzipped initramfs (static busybox)
is read into the carve at guest-physical `0x20000000` and handed over through
the two fields that are the whole interface, `ramdisk_image` and
`ramdisk_size`. Linux unpacks it and execs `/init`.

Two fixes were needed to get there, and the first is the more interesting:

**The SYSCALL MSRs have to reach the real machine.** `LSTAR`, `STAR`, `SFMASK`
and `KERNEL_GS_BASE` were being held in a C variable, which is fine right up
until userspace executes its first `syscall` - because SYSCALL reads LSTAR out
of the CPU, not out of us, and a value we kept to ourselves sent the guest
into the host's syscall handler or to address zero. `SWAPGS` is the same
argument for `KERNEL_GS_BASE`: it exchanges with the register, and an exchange
with a value never written swaps in nothing. Writing through is safe here for
a reason specific to this OS - UnoDOS is a ring-0 monolith that executes
neither instruction, so it has no values of its own to lose. An OS that did
would have to save and restore them around every entry.

**An initramfs with an empty `/dev` has no console.** The kernel opens
`/dev/console` for init; without the node, init gets no stdin, and a shell
that reads EOF exits immediately - which the kernel reports as "Attempted to
kill init".

**Where it stands, and the cause is now known.**

    printk: legacy console [ttyS0] enabled
    Run /init as init process
    Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000

The console node is in the cpio now, and it had to be: the kernel opens
`/dev/console` to give init its stdin **before** init runs, so an init that
creates the node itself is already too late - and cannot say so, because the
echo reporting the failure has nowhere to go either. That is why the first
attempt exited in silence. With the node present, `ttyS0` registers as a real
console rather than just an earlyprintk.

**The shell then exits with status 0, and that number is the diagnosis.** A
shell whose stdin is at end-of-file exits cleanly, and `exitcode=0x00000000`
is the kernel reporting exactly that. It is not a crash and not a missing
binary: `rdinit=/bin/sh` execs busybox directly, bypassing the script and
binfmt_script entirely, and the result is identical.

**The missing piece is the UART's receive path.** `uno_vdev_pio` answers LSR
with `0x60` - transmitter empty, and **data-ready always clear** - so nothing
ever arrives, and a console that never delivers a byte is indistinguishable
from a closed one. Output was enough to watch a kernel boot and is not enough
to hold a shell open.

**The receive path is now in** - an RX FIFO, LSR bit 0 following it, a queued
seed handed over on the first poll that finds it empty, and the MCR loopback
the driver's autoconfig actually tests (a port that answers a constant fails
that test and is registered as hardware the driver will not really drive).

**And the shell still exits with status 0.** So the missing thing is not the
bytes, it is the WAKE-UP: nothing ever interrupts the guest. The 8250 driver
is interrupt-driven - it enables the receive interrupt and waits for IRQ4 -
and this guest has no interrupt controller at all, so no byte we queue is ever
collected, and a blocking read has nothing to wake it.

That is the next piece, and it is bigger than the receive path was:

- an 8259 model (ports 0x20/0x21, 0xA0/0xA1) so the guest can unmask and
  acknowledge, since `nolapic` means Linux uses the legacy PIC;
- IRQ4 injected when the RX FIFO gains a byte, and IRQ0 from the PIT so the
  guest has a periodic tick at all;
- both delivered through `VM_ENTRY_INTR_INFO`, which **A4 already proved**
  with vector 0x20 and which has been idle ever since.

Everything under it is in place: the queue, the status bit, the injection
mechanism and the frame loop that would drive them.

### A6e: the shell answers

    ~ # echo UNODOS-GUEST-SHELL-OK
    UNODOS-GUEST-SHELL-OK
    ~ # uname -a
    Linux (none) 7.0.0-28-generic #28-Ubuntu SMP PREEMPT_DYNAMIC ... x86_64 GNU/Linux
    ~ # busybox ls /bin
    busybox  cat      echo     ls       mount    sh       uname

**A6's criterion, and the reply is the whole of it.** The kernel's own output
only ever proved a kernel runs. A reply proves the guest read a line we sent
it, ran it, and answered - the receive FIFO, the 8259, the injection and the
frame loop, end to end, in one line that cannot be produced any other way.
It is checked rather than eyeballed: `lin_sink` counts lines that START with
the marker (the echoed command line begins `~ # echo`), and the status block
says `shell ANSWERED` or `shell silent`.

The 8259 pair is in `unovdev_pc.c`, which is where the legacy PC now lives:
`unovdev.c` says "virtio-mmio transport" at the top and had quietly become
half a chipset, so the port-I/O world was split out before adding a third
device to it. The interrupt itself carries the vector **the guest programmed
in ICW2** (Linux remaps to 0x30), because injecting the ISA line number
delivers IRQ0 as vector 0 - a divide-error in the guest's own handler, a
plausible crash a long way from its cause (S-HV-35).

### Five findings, and only the first one was the interrupt controller

**One.** An 8259, IRQ4 on a received byte and IRQ0 from the PIT was the piece
that was actually missing, and it was also the smallest of the five.

**Two: the idle loop is `sti; hlt`, and that is a trap for the injector.**
STI leaves an interrupt shadow covering exactly one instruction, and that
instruction is the HLT - so the HLT exit arrives with the shadow set. Refusing
to inject while a shadow is set (which is otherwise correct) means refusing to
wake the guest whose HLT you just stepped past: it loops through its idle path
forever, takes no tick, does no work, and looks busy from the outside. The log
said so plainly once the right numbers were in it - **injections frozen at 76
while the PIC showed both lines up and nothing in service**. On hardware the
interrupt is delivered AT the HLT, which ends it; once the HLT is consumed the
shadow has done its job and retiring it is the accurate model (S-HV-37).

**Three: delivery must not depend on looking at a lucky moment.** Sampling the
guest's state once per entry and injecting if it happens to be ready is
starvation dressed as a policy, because a guest spends much of its life with
IF clear. Interrupt-window exiting is the mechanism the machine provides for
exactly this: arm it with a vector waiting and the CPU exits the instant the
guest becomes ready (S-HV-36). This is the fix that made the shell responsive
rather than occasionally responsive.

**Four: a periodic device must count GUEST time; a device the guest reads must
count the wall.** The two PIT channels want opposite clocks and each is wrong
with the other's. Channel 2 is compared by the guest against the real TSC it
reads directly, so it follows the wall (A6b). Channel 0 is a tick the guest
must SERVICE, and a guest with a quarter of a core driven by a wall-time tick
gets four periods per period of service: it re-enters the timer interrupt
forever. That is a livelock, not a fast clock (S-HV-38).

**Five: transmit-empty is a latch, received-data is a level.** This
transmitter never fills, so "holding register empty" is true forever - and
modelled as a level, the request cannot be quieted by anything the driver
does. The shell wedged mid-`ls`, at the exact moment the kernel first enabled
interrupt-driven transmit for a long output, and from then on did nothing but
service IRQ4 with its output frozen. On a real 8250 the THRE request is
latched and **reading IIR clears it** (S-HV-39). The DLAB bit belongs to the
same family: unmodelled, the driver's baud programming puts the low byte on
the console and **the high byte over the IER**, so setting the speed disarms
the interrupts just enabled (S-HV-40).

**The shape all five share** is the one A6b already named and this phase paid
for again: the default is never "works as on real hardware". Here it is
sharper - three of the five are cases where the *conservative* choice is the
broken one. Refusing to inject into a shadow, asserting a line that is
genuinely true, and counting real time are each defensible in isolation, and
each one stops the guest dead.

### The trap that cost more than any of them: `passes=3`

Two whole investigations went into a kernel that stopped after one line of
output, looking exactly like a hang. It was not a hang. **The shipped
DEBUG.CFG carries `passes=3`, so the stress driver finishes its passes and
powers the machine off about nineteen seconds in** - and a guest that runs at
4 ms per frame gets roughly a tenth of a second of CPU out of those nineteen
seconds, which is not enough to finish decompressing itself. The tell was
available and missed: the guest's exits were nearly all preemption-timer
exits, meaning it was busy and not stuck.

What makes it worth writing down is that the failure is indistinguishable from
the real thing and the fix is one word: **`noshutdown` goes in DEBUG.CFG
beside `vm-selftest`**. `tools/hv_remote.py`
carries the recipe in its docstring and now prints the guest's own output and
line count, so the next person reads the answer instead of ssh-ing for it.

## A7a: a disk, and the bug only a real driver could find

    ~ # mount -t devtmpfs dev /dev
    ~ # mount -t ext4 -o ro /dev/vda /mnt
    EXT4-fs (vda): mounted filesystem ba1e8a88-... ro without journal
    ~ # cat /mnt/HELLO
    UNODOS-GUEST-DISK-OK

**The guest mounted a filesystem out of an ordinary file on a UnoDOS volume.**
`EFINODOSVMROOTFS.IMG` is read through `uno_fs_read_at`, served as
virtio-blk, and Linux found an ext4 superblock in it, mounted it, and read a
file back. Every layer is under test at once: the transport, the feature
negotiation, a descriptor chain the DEVICE fills, the interrupt that announces
it, and the FAT read path underneath.

It is **read-only**, and that is the layer below rather than a choice: unofs
and the native FAT driver both read from an offset and write only whole files,
so one sector write would rewrite the whole image. `VIRTIO_BLK_F_RO` is offered
so the guest knows before it tries.

### The bug: RSI and RDI were swapped in the instruction decoder

`gpr()` maps an x86 register encoding onto our saved register file, and it had
encodings 6 and 7 - RSI and RDI - pointing at each other's slots. Every MMIO
access through either register therefore carried the OTHER one's value.

**It had been wrong since A5 and nothing noticed**, because nothing had used
those registers: A5's guest is hand-written and uses rax/rbx/rcx/rdx, and
Linux's port I/O goes through rax. It took a real driver. virtio-mmio writes
its status byte from ESI, so every status write arrived as an unrelated
`0x04ef0000`, the status register read back as zero, and the driver reported
`device refuses features: 0` - a conclusion about FEATURES that had nothing
whatever to do with features.

**A wrong register file is invisible until something uses the register you got
wrong**, and the report you get names the wrong subsystem. What found it was
recording the register TRAFFIC - which register, which way, what value - and
reading the driver's side of the conversation instead of its conclusion. The
driver's half was perfect throughout: it read the magic, the version and the
device id, selected both feature halves, and accepted `VIRTIO_F_VERSION_1`.
Only the writes were nonsense, and only from one register.

### And one that was not our bug at all

`/dev/vda` did not exist even after the driver created it, because this
initramfs carries exactly one device node - `/dev/console`, added in A6c
because the kernel opens it before init runs. Nothing populates `/dev`
afterwards; there is no udev here. `mount -t devtmpfs dev /dev` is the kernel
offering the nodes it already knows about. The shell reports the same "No such
file or directory" for a missing node as for a missing filesystem, so the
message pointed at the disk rather than at `/dev`.

### The harness cost more than the phase did

Four separate things, none of them about virtualization, each of which
produced a run that looked like a guest failure:

- **The "occasional empty capture" had a cause and was never flakiness.** A run
  killed by its timeout can leave QEMU alive, holding a write lock on the image
  and on `/tmp/hv.log`; the next run then shares the log with it, or fails to
  start at all. Five had accumulated. The run now waits for stragglers to go
  rather than sleeping a fixed second and hoping.
- **QEMU's stderr went to `/dev/null`**, so a QEMU that exited instantly on a
  locked image printed nothing and read exactly like a guest that said nothing.
- **`pkill -f qemu-system-x86_64` matched the run script itself**, because the
  whole script arrives as the remote shell's command line and names QEMU
  further down. The run killed itself before it started - silently, ssh exit
  255, nothing on either stream. Matching the process NAME fixes it; the usual
  `[q]emu` bracket trick does not.
- **The image check scanned the first 4 MB.** Once a 17 MB kernel and a disk
  image were staged beside `DEBUG.CFG`, it moved past that window and the
  harness started warning that a perfectly armed image was not armed - worse
  than not checking, because the warning sends you looking at the wrong thing.

The script is shipped to the box as a FILE now, which sidesteps the argument
quoting entirely and can be run by hand or under `bash -x` exactly as the
harness runs it. `tools/vm_stage.py` stages the payload and arms DEBUG.CFG in
one step, because the five hand-run commands it replaces are each silent when
skipped.

**One last trap worth the line it costs:** the console sink only emits a line
when it sees a newline, so a marker file written without a trailing newline is
read correctly, delivered correctly, and never appears. It looked like a hang
in the read path.

### The manager on real hardware, and the thing that stops it

Installed to the ZimaBlade (the always-on metal box) over URC on 2026-08-07:
kernel, `APPS\VMGR.UNO`, `ROOTFS.IMG` and `INITRD` all pushed and verified,
`BUILD.TXT` reporting `debug-c958b2e2` so the box is provably running this
code rather than an older one - which on that box is a claim worth checking
every time.

**The application cannot be launched, and it is not its fault.**
`pc64_shell_run_user()` - what Files calls when you open a `.UNO` - handles a
PYAPP and a CLASSIC app, the latter through `unoapp_user_run()`, which requires
the entry point to return a `UnoAppIface` with a `draw` member. A unoui-class
module returns a `UnoUuiApp`, so the load is refused and Files says
`Could not launch that .UNO.` There is no third branch, `uno.run_app` does not
exist, and the URC `launch <n>` verb indexes the shell's built-in slots. **A
unoui-class module without a desktop slot cannot be run by any means.**

That was recorded here as "it opens from Files meanwhile", which was wrong and
was inferred rather than tried: LOGVIEW's note says it opens from Files, but
LOGVIEW was a PYAPP when that was true and gained `EX_LOGVIEW` in the same
commit that made it unoui-class. The slot is a prerequisite, not a convenience,
and the request in `pc64/UNOAUTOMATE-REQUESTS.md` now says so.

**Two other things the run established**, both about the delivery path rather
than the hypervisor:

- **URC cannot carry the appliance kernel.** `PUT_MAX` is a fixed 8 MiB staging
  buffer, and the `bzImage` is 17 MB; it fails at 8386200 bytes. The disk image
  and initramfs fit. Requested of the unoautomate lane.
- **Files lists a directory in FAT order and does not scroll**, so a file pushed
  last is invisible in a directory of twenty. Neither is a unovirt problem; both
  are worth knowing before planning another hardware session.

### What is left

- **A real console, not a seeded one.** Input still comes from a string
  handed over when the driver arms its receive interrupt. Real keystrokes come
  from the frame loop, which already owns the keyboard, and that is A8's work
  when the guest gets a window.
- **A LAPIC.** `nolapic` is doing real work here: the guest has one legacy PIC
  and no per-CPU timer, which is fine for one core and is the thing A9 has to
  replace before a guest gets a core of its own.
- **virtio-console** as the guest's real console instead of the 8250 standing
  in for it, and virtio-blk/net (A7).

## Changelog

- **2026-08-21, API 1.** A8 (in progress): the guest gets a DISPLAY and
  INPUT. The boot protocol places a linear XRGB8888 framebuffer at the top
  of the carve, reserves it in the e820 map, and describes it in
  `boot_params.screen_info` as VIDEO_TYPE_EFI - which a stock kernel drives
  through sysfb (simpledrm or efifb) with no driver from us.  `uno_vmm_fb()`
  hands the surface to a blitter, NULL when nothing is armed (S-HV-49/50).
  Input is an emulated i8042 in unovdev_pc.c - PS/2 set-1 keyboard plus an
  IntelliMouse on the aux port, with the slave PIC wired through the cascade
  for IRQ12 (S-HV-51/52) - because virtio-input cannot cross virtio-mmio v2
  (bitmap-sized config reads BUG() in the guest's own transport) and every
  distro ships it =m anyway.  The Appliances app gained a Display view: the
  surface blitted (and swizzled - fb.h is 0xAABBGGRR) into a UI_CANVAS, keys
  and pointer forwarded, F12 to leave.  The appliance payload builds under
  `guest/appliance/` now: a 6.12 kernel with everything =y, an initramfs
  with a shell on ttyS0 AND tty1, and an Alpine+Xorg+Chromium rootfs for
  the browser milestone.  `uno.vm_status()` (PYRT) reports the guest's
  progress line so a harness can poll instead of guessing at wall time.
- **2026-08-07, API 1.** The backend seam is generic. `uno_hv_t` loses its nine
  per-phase entries and gains `vcpu_create` / `vcpu_run` / `map` / `inject` plus
  a `get`/`set` window on vCPU state; the phase tests move to `hv_phases.c`
  above the seam, with `unovirt_phase.h` declaring them and carrying the three
  result structs. `uno_vmexit` grows the neutral decodes a caller acts on
  (`instr_len`, `gpa`, `io_*`, `cr_*`) so port I/O, control-register writes and
  second-stage faults are handled once rather than per vendor. In `hv_vmx.c`
  the forty-line block that opened all six phase functions is one `vmcs_begin`.
  No behaviour change: A0..A6 pass on devbuntu with the selftest string
  field-for-field identical and the guest shell still answering. `uno_vmm_*` is
  untouched, so nothing outside the subsystem sees this.
- **2026-08-07, API 1.** A7a: virtio-blk over `EFI\UNODOS\VM\ROOTFS.IMG`, read-only (the
  layer below writes whole files only). The transport went from one device
  with one queue to several with two, the chain walk returns SEGMENTS with
  their direction, and the console learned that queue 0 is receive. Found a
  decoder bug latent since A5: **RSI and RDI were swapped in `gpr()`**,
  invisible until a real driver used them. Contracts S-HV-41..45.
- **2026-08-07, API 1.** A6e, and **A6 is met**: an 8259 pair, IRQ0 from the
  PIT and IRQ4 from the UART, injected through `VM_ENTRY_INTR_INFO` with
  interrupt-window exiting so delivery is prompt by construction. The guest
  shell reads a command and answers. `unovdev_pc.c` (the legacy PC platform)
  split out of `unovdev.c` (the virtio transport) first, as a pure move.
  New: `uno_vdev_irq_pending/take`, `uno_vdev_pc_state`, `uno_vdev_pic_state`,
  `uno_vmm_guest_cycles`. Contracts S-HV-34..40.
- **2026-08-06, API 1.** A0: the capability gate. `uno_vmm_probe`,
  `uno_vmm_eligible`, `uno_vmm_blocker_str`, `uno_vmm_carve_mb`,
  `uno_vmm_status_str`. Contracts S-HV-01..11.
- **2026-08-06, API 1.** A1 written: the `uno_hv_t` backend seam, the SVM
  backend (`hv_svm.c`), the vCPU register context, the marker guest and the
  crasher. `uno_vmm_selftest` / `uno_vmm_selftest_str`, opt-in behind DEBUG.CFG
  `vm-selftest`. Unproved on AMD: see the wedge above. (The seam carried a
  pointer per phase until 2026-08-07; see the entry at the top.)
- **2026-08-06, API 1.** A1 PASSES on VMX (`hv_vmx.c`), on devbuntu under KVM
  at L0: entered, round trip, crasher contained, boot continued. `tools/
  hv_remote.py` runs it.
- **2026-08-06, API 1.** A6 (partial): the bzImage loader, `boot_params`, an
  8250 on COM1, CPUID/MSR handling for a real kernel, CR shadow registers
  and unconditional I/O exiting. Linux boots and prints. Contracts S-HV-31..33.
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
