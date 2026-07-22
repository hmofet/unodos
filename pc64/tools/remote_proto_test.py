#!/usr/bin/env python3
"""Protocol unit test for unoauto_remote (no device needed).

Stands up a real UnoAutoLink listener and a fake 'pc64' peer socket that speaks
URC, then exercises: host->device commands (with multi-line + err responses),
device->host LOG/MSG delivery, and device->host commands (bidirectional).
Exit 0 iff every assertion holds.
"""
import socket, threading, time, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unoauto_remote import UnoAutoLink

PORT = 5397


def fake_pc64(ready, result):
    """Connect to the listener and behave like the device end."""
    import base64
    c = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    f = c.makefile("rwb", buffering=0)
    stage = bytearray()

    def send(s): f.write((s + "\n").encode())

    send("HELLO pc64 1")
    send("LOG NET link up 192.168.1.5")     # -> host on_log
    send("MSG hello from device")           # -> host on_message
    send("CMD 900 save report.txt")         # device -> host command (bidir)
    ready.set()

    def handle(line):
        typ, _, rest = line.partition(" ")
        if typ != "CMD":
            return typ != "BYE"        # keep going unless BYE
        rid, _, tail = rest.partition(" ")
        verb, _, args = tail.partition(" ")
        if verb == "probe":
            send("RSP %s ok 1 1 0 0 win0" % rid)      # kind state v1 v2 name
            send("RSP %s ok 1 0 0 0 my window" % rid)  # name may contain spaces
        elif verb == "boom":
            send("RSP %s err kaboom" % rid)
        elif verb == "py":
            send("RSP %s ok %s" % (rid, "42" if "6*7" in args else "?"))
        elif verb == "vols":
            send("RSP %s ok 0 0 1 RAM" % rid)
            send("RSP %s ok 2 2 1 USB stick" % rid)   # kind 2 = firmware SFS
        elif verb == "put":
            p = args.split(" ", 3)
            a3, a4 = p[2], (p[3] if len(p) > 3 else "")
            if a3 == "done":
                total = int(a4, 16)
                if len(stage) == total:
                    result["pushed"] = bytes(stage)
                    send("RSP %s ok verified %d" % (rid, total))
                else:
                    send("RSP %s err size-mismatch" % rid)
            else:
                off = int(a3, 16)
                raw = base64.b64decode(a4)
                if off == 0:
                    stage.clear()
                if len(stage) < off + len(raw):
                    stage.extend(b"\0" * (off + len(raw) - len(stage)))
                stage[off:off + len(raw)] = raw
                send("RSP %s ok %d" % (rid, len(raw)))
        else:
            send("RSP %s ok %s" % (rid, verb))
        send("RSP %s end" % rid)
        return True

    try:
        for raw in f:
            if not handle(raw.decode().rstrip("\r\n")):
                break
    except (OSError, ValueError):
        pass
    finally:
        try: c.close()
        except OSError: pass


def main():
    logs, msgs, hostcmds = [], [], []
    link = UnoAutoLink("127.0.0.1", PORT)
    link.on_log(lambda ch, t: logs.append((ch, t)))
    link.on_message(lambda m: msgs.append(m))
    link.on_command("save", lambda args: hostcmds.append(args) or "saved " + args)
    link.listen()

    ready = threading.Event()
    result = {}
    threading.Thread(target=fake_pc64, args=(ready, result), daemon=True).start()
    assert link.wait_connected(5), "no connection"
    assert ready.wait(5), "device didn't start"
    time.sleep(0.3)  # let the async LOG/MSG/CMD frames arrive

    ok = True

    def check(cond, label):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label)
        ok = ok and cond

    # device -> host: log + message delivery
    check(("NET", "link up 192.168.1.5") in logs, "LOG delivered to on_log")
    check("hello from device" in msgs, "MSG delivered to on_message")
    # device -> host: bidirectional command reached the host handler
    check("report.txt" in hostcmds, "device->host CMD dispatched to handler")

    # host -> device: multi-line command response (name-last, spaces in name)
    rows = link.probe()
    check(rows == [{"kind": 1, "state": 1, "v1": 0, "v2": 0, "name": "win0"},
                   {"kind": 1, "state": 0, "v1": 0, "v2": 0, "name": "my window"}],
          "probe multi-line response parsed")

    # host -> device: py eval round-trip
    check(link.eval("print(6*7)") == ["42"], "py eval round-trip")

    # host -> device: err response raises
    try:
        link.command("boom")
        check(False, "err response raises")
    except RuntimeError as e:
        check("kaboom" in str(e), "err response raises")

    # host -> device: vols listing parsed (name may contain spaces)
    vols = link.vols()
    check(vols == [{"vol": 0, "kind": 0, "writable": True, "name": "RAM"},
                   {"vol": 2, "kind": 2, "writable": True, "name": "USB stick"}],
          "vols listing parsed")

    # host -> device: push_file chunks + finalize + verify (the A/B path)
    import tempfile, os
    payload = bytes((i * 7 + 3) & 0xFF for i in range(1750))   # spans >1 chunk
    tf = tempfile.NamedTemporaryFile(delete=False)
    tf.write(payload); tf.close()
    try:
        verified = link.push_file(2, "EFI\\BOOT\\BOOTX64.EFI", tf.name, chunk=750)
        check(verified, "push_file verified")
        check(result.get("pushed") == payload, "pushed bytes match exactly")
    finally:
        os.unlink(tf.name)

    link.send("BYE")
    time.sleep(0.2)
    link.close()
    print("\n" + ("ALL PASS" if ok else "FAILURES"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
