#!/usr/bin/env python3
"""unolog_urc - prove the system log reaches DISK, and speaks syslog.

Everything here is measured off the machine rather than off the log's own
counters: the file is read back over URC, and the syslog traffic is measured by
a real collector socket on this host. A log subsystem asked whether it logged
will always say yes.

    cd pc64 && UNO_DEBUG=1 ./build.sh
    python3 tools/unolog_urc.py
"""
import sys, os, time, socket, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from urcui import UrcUi

HOST_IP  = "10.0.2.2"          # the QEMU user-mode gateway = this machine
SYSLOG_PORT = 5514             # our collector, guest -> host (outbound: free)
SINK_FWD    = 5515             # host -> guest needs a forward; see below.
                               # NOT the same host port as the collector - both
                               # bind on this machine and the second one loses.

# The guest's syslog listener is only reachable through a slirp forward.
os.environ.setdefault("URC_HOSTFWD", ",hostfwd=udp::%d-:514" % SINK_FWD)

_rx = []
_sock = None


def collector_start():
    """A real syslog collector. The assertion for 'we are a source' is what
    ARRIVES here, not what the guest says it sent."""
    global _sock
    _sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    _sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    _sock.bind(("0.0.0.0", SYSLOG_PORT))
    def loop():
        while True:
            try:
                d, a = _sock.recvfrom(2048)
            except Exception:
                return
            _rx.append((a[0], d.decode("latin1")))
    threading.Thread(target=loop, daemon=True).start()


def py(ui, src, timeout=40):
    return ui.link.command("py", src, timeout=timeout)


def read_log(ui):
    """\\LOGS\\SYSTEM.LOG off the boot volume, via PYRT."""
    out = py(ui, "import uno; b=uno.read(2,'LOGS'+chr(92)+'SYSTEM.LOG'); "
                 "print(len(b) if b else -1)")
    return out


def main():
    collector_start()
    print("collector listening on udp/%d" % SYSLOG_PORT)
    results = []
    try:
        with UrcUi() as ui:
            # ---- 1. the log exists on disk at all -----------------------
            # The kernel logs its own start at NOTICE, and the default level
            # keeps NOTICE, so there is something to find before we do
            # anything. A flush is forced rather than waited for.
            py(ui, "import uno; print(uno.log_flush())")
            n = py(ui, "import uno; b=uno.read(1,'LOGS'+chr(92)+'SYSTEM.LOG'); "
                       "print(len(b) if b else -1)")
            print("  SYSTEM.LOG on vol 1:", n)
            got = any("-1" not in l for l in n)
            results.append(("the log is written to \\LOGS\\SYSTEM.LOG", got))

            # ---- 2. levels actually filter ------------------------------
            # Write one record at each severity with the level at ERR, then
            # count what landed. A level control that does not drop anything
            # is a label.
            py(ui, "import uno; uno.log_level(3)")
            for s in range(0, 8):
                py(ui, "import uno; uno.log(%d,6,'levelprobe-%d')" % (s, s))
            py(ui, "import uno; print(uno.log_flush())")
            txt = py(ui, "import uno; b=uno.read(1,'LOGS'+chr(92)+'SYSTEM.LOG'); "
                         "print(b.count(b'levelprobe-'))")
            print("  records kept at level=err:", txt)
            # 0..3 kept (emerg, alert, crit, err) = 4
            results.append(("level=err keeps exactly the four at or below it",
                            any("4" == l.strip() for l in txt)))

            py(ui, "import uno; uno.log_level(6)")

            # ---- 3. we are a syslog SOURCE ------------------------------
            _rx.clear()
            py(ui, "import uno; uno.log_remote('%s',%d)" % (HOST_IP, SYSLOG_PORT))
            py(ui, "import uno; uno.log_remote_level(6)")
            for i in range(3):
                py(ui, "import uno; uno.log(4,1,'wire-probe-%d')" % i)
            time.sleep(2.0)
            hits = [m for _, m in _rx if "wire-probe" in m]
            print("  collector received %d datagram(s)" % len(hits))
            for m in hits[:3]:
                print("    " + m.strip())
            results.append(("syslog SOURCE: datagrams reach a real collector",
                            len(hits) >= 3))
            # RFC 5424 shape: <PRI>1 TIMESTAMP HOST APP - FACILITY - MSG
            wf = hits[0] if hits else ""
            results.append(("...framed as RFC 5424 with a PRI and version 1",
                            wf.startswith("<") and ">1 " in wf))
            # facility 1 (net) -> syslog local0 = 16, severity 4 -> 16*8+4 = 132
            results.append(("...with the right PRI (net/warning = 132)",
                            wf.startswith("<132>")))

            # ---- 4. we are a syslog SINK --------------------------------
            # Send the guest a message from this host and read it back out of
            # the guest's own log.
            #
            # THROUGH A PORT FORWARD, not at 10.0.2.15. Slirp is outbound-only:
            # the guest reaches the host at 10.0.2.2 and the host cannot reach
            # the guest without a hostfwd. Sending to the guest's slirp address
            # goes nowhere and reports a working sink as broken - which it did.
            # The probes carry PRI 134 = severity INFO(6). Inbound records are
            # filtered by the SAME level as local ones - a record is a record -
            # so the level must admit INFO or the sink correctly drops every
            # one and looks broken. It did, and that read as a bug in the sink.
            py(ui, "import uno; uno.log_level(7)")
            print("  log_listen(1) ->", py(ui, "import uno; print(uno.log_listen(1))"))
            print("  stat before   ->", py(ui, "import uno; print(uno.log_stat())"))
            time.sleep(0.5)
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for i in range(3):
                s.sendto(b"<134>1 2026-08-06T18:00:00Z host app - - - sink-probe-%d" % i,
                         ("127.0.0.1", SINK_FWD))   # the FORWARD, not the collector
                # net.c keeps ONE datagram per bound port (udp_q[].len is
                # overwritten, not queued), so a back-to-back burst arrives as
                # its last member only. Space them past a frame.
                time.sleep(0.4)
            s.close()
            time.sleep(2.5)
            # (next, dropped, sent, received) - `received` separates "the
            # datagram never arrived" from "it arrived and was not filed",
            # which are different bugs in different places.
            print("  stat after    ->", py(ui, "import uno; print(uno.log_stat())"))
            py(ui, "import uno; print(uno.log_flush())")
            seen = py(ui, "import uno; b=uno.read(1,'LOGS'+chr(92)+'SYSTEM.LOG'); "
                          "print(b.count(b'sink-probe-'))")
            print("  sink-probe records in the guest's log:", seen)
            results.append(("syslog SINK: messages from the network are filed",
                            any(l.strip().isdigit() and int(l.strip()) >= 1
                                for l in seen)))
    finally:
        try:
            if _sock: _sock.close()
        except Exception:
            pass

    print()
    bad = 0
    for name, ok in results:
        print(("pass " if ok else "FAIL ") + name)
        bad += 0 if ok else 1
    print("\n%d pass, %d fail" % (len(results) - bad, bad))
    return 1 if bad else 0


sys.exit(main())
