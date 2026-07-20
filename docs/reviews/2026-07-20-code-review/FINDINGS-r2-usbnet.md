# Round 2 - USB-Ethernet "network test" freeze (2026-07-20)

## Control-flow reframe (important)
`net_reset()` (`apps/network.c:67-89`) only binds a PCI NIC (e1000/e1000e/igb/r8169).
It never calls `rtl8152_nic()` or `ax88179_nic()`. The USB-Ethernet drivers bind once
at BOOT (`pc64_uui.c:1880-1882`: `uno_xhci_init(); if (!ax88179_nic()) rtl8152_nic();`).
So "plugged in USB adapter, froze" is, in code terms, a **boot-time USB freeze** while
bringing up the adapter, not something the Network app's test steps reach.

## 1. PRIMARY (confirmed) - unbounded `for(;;)` event-wait in `xhci.c:229` `run_command`
Verified first-hand. The loop's ONLY non-match exit is `poll_event` timing out, which
happens only when the event ring stays EMPTY for 2,000,000 spins. If the controller
keeps delivering events that are not this command's completion, the loop dequeues each
and spins again forever. Reached at boot binding via `TR_ENABLE_SLOT` (`:364`),
`TR_ADDRESS_DEV` (`:385`), `TR_CONFIG_EP` in `uno_usb_setup_bulk` (`:462`) /
`uno_usb_setup_intr_in` (`:552`) - exactly the calls the USB-eth drivers make.

**Why metal-only:** a real USB3 SuperSpeed adapter continuously emits Port Status
Change events (type 34) as the SS link trains and enters/exits U1/U2 LPM right after
the port reset. Those PSC events are dequeued inside `run_command`, never match
`TR_CMD_COMPLETE`, and keep the ring non-empty so `poll_event` never times out. QEMU
models no LPM and emits no such storm, so the ring drains and the timeout path works -
which is why this always passed in the QEMU harness. Self-inflicted contributor:
`reset_port` (`:261`) acks only PRC and leaves CSC/PLC change bits set, which re-fire.

## 2. PRIMARY (same defect) - `xhci.c:304` `poll_xfer`
Identical unbounded `for(;;)`; line 306 `continue`s on any non-XFER event (PSC), so the
same storm hangs it during every control/bulk register access in the driver bring-up
(`ax_reset`, `chip_init`, OCP reads/writes, MAC read, `set_config`).

**Fix (both):** bound total stray events / add a wall-clock deadline, not just a
per-`poll_event` timeout:
```c
int spins = 0;
for (;;) {
    if (!poll_event(&ev, 200000)) { if (++spins > 40) return 0; continue; }
    if (TRB_GET_TYPE(ev.control) == TR_CMD_COMPLETE && ev.param == mytrb) { ... }
    if (++spins > 4096) return 0;   /* consumed too many stray events: give up */
}
```
Also quiet the source: write-1-to-clear CSC/PLC in `reset_port` and/or disable U1/U2
LPM on the port so the SS adapter stops generating PSC churn. Note a device that merely
NAKs forever is NOT the trigger (CErr=3 posts an error event, so `poll_xfer` stays
bounded); the hang specifically needs other events to keep arriving.

## 3. SECONDARY (bounded, but a multi-second stall) - test targets QEMU-only 10.0.2.x
If a PCI NIC exists so the test runs, every `net_step` case is `gTimer`-capped except
the synchronous `S_TLS` step (`apps/network.c:143-166`) to `ECHO=10.0.2.100:9443`.
`10.0.2.x` are QEMU-SLIRP-only guestfwd targets that do not exist on a real network
(comment at `apps/network.c:24-27`). On metal the SYN is unanswered and the code spins
`tls.c:120` (t=4,000,000), `low_read` `:76` (6,000,000), `low_write` `:84`
(12,000,000). Bounded, but 4-12M `net_poll` iterations to a black hole is a long UI
stall a user would call a freeze. **Fix:** make these deadlines time-based
(`uno_pc64_delay_ms` + a few-second tick budget) and point the test at the
DHCP-provided gateway/DNS, not the hardcoded SLIRP literals.

## Other bugs noticed
- **`ax88179.c:169-170` `ax_link()` never reports real link state.** Returns
  `ax_rd16(AX_MEDIUM_STATUS_MODE) & AX_MEDIUM_RECEIVE_EN`, but RECEIVE_EN is a bit the
  driver itself set in `ax_reset()` (`:109-111`) and never clears, so `ax_link()`
  returns "up" permanently even with the cable unplugged. Should read PHY link status.
- RX parsers (`rtl_recv:430-445`, `ax_recv:150-166`) are correctly bounds-checked
  against transfer length; DNS resolver and `ip_recv` clamp are guarded. No driver RX
  OOB found.
- `net.c:159-176` `net_ping` stashes `g_ping_dst = ip` as a raw pointer to the caller's
  array; the test passes file-static GW/ECHO so it survives. Fragile, not a live bug.

## Bottom line
Single most likely hard hang: the unbounded event-wait loops in `xhci.c`
(`run_command:229`, `poll_xfer:304`), hit during boot-time USB-Ethernet binding, when a
real USB3 adapter floods interrupter 0 with PSC/LPM events QEMU never generates. Add a
total-event/deadline cap and quiet the PSC source. Separately cap the `tls.c`
spin-counters and stop the test from targeting QEMU-only 10.0.2.x.
