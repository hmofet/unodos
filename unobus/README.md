# unobus — drivers & buses: enumerate → bind → register (Phase 11, §7)

A driver is a backend bound at *runtime* implementing a §6 service (block/input/fb/
nic/audio). Buses enumerate device nodes; drivers claim by class and **publish** into
a service registry; the Primitive-Vtable slots are filled **from the registry**.
Static-link first (no loadable modules in 3.1). On fixed-hardware machines it
degenerates to hard-bound services at zero cost.

`sh unobus/build.sh` proves on host: a PCI/USB enumeration binds an `ide` block
driver + a `hid` input driver into the registry; a registry-bound `block` service
(the real `unofs` interface, RAM-backed here) **reads a sector**; an absent NIC stays
unbound; and the **Famicom Disk System detect-pin** path fills the *same* block slot
as the bus walk — the §7 scale-down ("the same binding question, answered by a pin").
Blocked tail: real bus hardware (PCI on PowerPC Mac, USB on DC/PS2/Xbox).
