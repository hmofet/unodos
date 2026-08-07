# unovdev, the subsystem contract

The machine an appliance guest finds when it looks around: a virtio-mmio
transport, and the legacy PC platform a kernel talks to before it can drive
anything else. The hypervisor itself is `pc64/UNOVIRT.md`; this file is what
sits on the far side of a decoded guest access.

**Status: [implemented]** for the console transport (A5) and the legacy
platform an Ubuntu kernel needs to reach a shell (A6). One guest, one queue.

## The seam, and why it is this narrow

Two functions, and neither knows what a VMCS or a VMCB is:

| Call | What it answers |
|---|---|
| `uno_vdev_mmio(gpa, is_write, size, &val)` | an MMIO access to the transport; 1 = it was ours |
| `uno_vdev_pio(port, is_write, size, &val, sink)` | a port-I/O access; `sink` takes each completed console line |
| `uno_vdev_irq_pending()` / `uno_vdev_irq_take()` | may a vector be injected, and which one |
| `uno_vdev_serial_push(c)` / `uno_vdev_serial_seed(s)` | put a byte where the guest will read it |
| `uno_vdev_queue` / `uno_vdev_output` / `uno_vdev_cycle_refused` | place a queue, read what came through, and the bounds test |
| `uno_vdev_pc_state()` / `uno_vdev_pic_state()` | the UART and interrupt state in one word each, for a trace |

A backend hands over an address, a direction and a size and gets an answer.
Everything vendor-specific - decoding the instruction, walking the guest's page
tables, the exit qualification - happens above this line, once per vendor.

**MMIO and port I/O are not equally hard, and the asymmetry is architectural.**
A port-I/O exit carries the port, the size and the direction, so there is
nothing to decode. An EPT violation carries the faulting address and a
direction bit and *nothing else*: the register and the operand size are only
knowable by decoding the instruction, which is why the backend carries a small
`mov` decoder and refuses everything else loudly.

## Two files, because they are two different jobs

- **`unovdev.c`** is the virtio-mmio transport and its console device: the
  register file, the descriptor-chain walk, the used ring.
- **`unovdev_pc.c`** is the legacy PC: the 8254, the 8259 pair, the CMOS and
  the 8250. A booting kernel talks to all four long before it can drive a
  virtio device, so they are not a detour, they are the platform.

They were one file until A6e, and the split happened before adding the 8259
rather than after: a file whose header says "virtio-mmio transport" and whose
body is half a chipset stops being read by anyone looking for either.

## The security posture, which is the point of the transport file

**Every address in a virtqueue came from the guest, including the ones inside
descriptors, and second-stage translation does nothing about them.** It bounds
the guest's own accesses, not the ones the host makes on its behalf. So:

- every guest address goes through `uno_vmm_gpa()`, which checks address and
  length **together** - checking them separately is how an overflow gets
  through (S-HV-30);
- every chain walk is bounded by the queue size, because a descriptor whose
  `next` points at itself is one store by the guest, and a device that trusts
  a chain to terminate hangs the machine on request (S-HV-29).

`uno_vdev_cycle_refused()` is that second rule under test, and the interesting
result is not what it returns but that it returns at all.

## Four rules a device model here has to obey

Each of these was learned by breaking it, and each is in `pc64/SPEC.md`.

**Absent hardware reads as quiet, not as busy.** The natural default for an
unimplemented port is all-ones, and in a *status* register all-ones means every
flag is set - including update-in-progress, which a kernel spins on until it
clears. The CMOS read as permanently mid-update and the boot ended there.

**A periodic interrupt counts GUEST time; a register the guest reads counts the
wall.** The two PIT channels want opposite clocks and each is broken by the
other's (S-HV-38). Channel 2 is compared against the real TSC the guest reads
directly, so it follows the wall. Channel 0 is a tick the guest must *service*,
and a quarter-core guest driven by a wall-clock tick gets four periods per
period of service: it re-enters its timer handler forever.

**Know which of your interrupt causes is a level and which is a latch.**
Received-data is a genuine level and deasserts when the driver drains the
receive register. Transmit-empty is not: this transmitter never fills, so the
condition is true forever, and modelled as a level it can never be quieted by
anything the driver does. On a real 8250 it is latched and reading IIR clears
it (S-HV-39).

**Model the mode bits, not just the registers.** DLAB redirects the UART's
first two ports at the divisor latch; unmodelled, a driver setting its baud
rate puts one byte on the console and the other **over the interrupt-enable
register** (S-HV-40).

## What is deliberately not here

- **No PCI.** Linux takes its mmio transports from the command line
  (`virtio_mmio.device=`), so a whole host bridge and its config space are
  avoidable. A Windows appliance cannot work this way and is a separate phase.
- **No second queue and no second device.** One console queue is what A5 and
  A6 need; blk and net are A7, and they are what the bounds seam was written
  for.
- **No interrupt for the transport.** `R_INTR_STATUS` is maintained but the
  guest is never interrupted by the virtio device: its console is the 8250
  today. That changes when virtio-console becomes the real console.
