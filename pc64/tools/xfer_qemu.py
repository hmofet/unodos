#!/usr/bin/env python3
"""End-to-end gate for unoxfer (UnoTransfer) and the `xfer` URC verb, in QEMU.

WHAT IT ACTUALLY PROVES.  Not "the verb parses" - that a real recursive
directory transfer, driven from the dev PC over URC, lands byte-identical files
on the guest's disk without the payload ever crossing the URC link.  That is
the entire claim unoxfer makes over `put`, and it is the only claim worth a
gate.

The host runs a tiny WebDAV server (GET + PROPFIND + MKCOL + PUT) in-process.
Two reasons, and the second is the interesting one:

  1. A recursive pull needs LISTING, and plain HTTP has none - a web server's
     index page is HTML, not a directory, and unoxfer refuses to pretend
     otherwise (that way lies the web crawler this port deliberately left out).
     So the recursive half of the test has to speak a protocol that can list.
  2. Writing the server here means the PROPFIND parser is tested against XML
     the test author did not also write into the parser.  It answers with the
     namespace prefix, the percent-encoded hrefs and the self-first ordering a
     real server sends, because those are the three things a hand-rolled
     WebDAV client gets wrong.

From a SLIRP guest the host is 10.0.2.2, so the guest dials back to this
process's loopback.

    UNO_DEBUG=1 ./build.sh
    python3 tools/xfer_qemu.py

Exit 0 iff every check passes.
"""
import os, sys, time, socket, threading
import http.server

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from unoauto_remote import UnoAutoLink
import remote_qemu

URC_PORT = 5397
DAV_PORT = 8497
GUEST_HOST = "10.0.2.2"

# ---------------------------------------------------------------------------
# The tree the guest is asked to fetch.  Deliberately awkward in the ways that
# matter: a file that is not a round number of blocks, one that is empty, one
# whose name needs mapping onto 8.3, and a nested directory - so "it worked"
# cannot mean "it worked for the easy case".
# ---------------------------------------------------------------------------
TREE = {
    "readme.txt":        b"UnoTransfer end-to-end\n",
    "odd.bin":           bytes((i * 37 + 11) & 0xFF for i in range(4097)),
    "empty.dat":         b"",
    "sub/nested.bin":    bytes((i * 13 + 5) & 0xFF for i in range(1500)),
    "sub/deep/leaf.txt": b"the third level\n",
}


def build_tree(root):
    for rel, data in TREE.items():
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


# ---------------------------------------------------------------------------
# A minimal WebDAV server.  http.server's handler with three methods added.
# ---------------------------------------------------------------------------
class Dav(http.server.SimpleHTTPRequestHandler):
    root = None

    def log_message(self, *a):            # keep the gate's output readable
        pass

    def _fs(self):
        rel = self.path.split("?")[0].lstrip("/")
        from urllib.parse import unquote
        rel = unquote(rel)
        p = os.path.normpath(os.path.join(Dav.root, rel))
        if not p.startswith(os.path.normpath(Dav.root)):
            return None                   # no escaping the served root
        return p

    def do_PROPFIND(self):
        import urllib.parse
        p = self._fs()
        if p is None or not os.path.exists(p):
            self.send_error(404); return
        depth = self.headers.get("Depth", "1")
        base = self.path.split("?")[0]
        if not base.endswith("/"):
            base += "/"

        def entry(href, isdir, size):
            # The namespace PREFIX is deliberately not "D": a client that
            # matched on the literal "<D:href>" would pass against a friendly
            # server and fail against half the real ones.
            rt = "<ns0:collection/>" if isdir else ""
            return (
                "<ns0:response>"
                "<ns0:href>%s</ns0:href>"
                "<ns0:propstat><ns0:prop>"
                "<ns0:resourcetype>%s</ns0:resourcetype>"
                "<ns0:getcontentlength>%d</ns0:getcontentlength>"
                "</ns0:prop><ns0:status>HTTP/1.1 200 OK</ns0:status></ns0:propstat>"
                "</ns0:response>" % (urllib.parse.quote(href), rt, size)
            )

        body = "<?xml version=\"1.0\"?><ns0:multistatus xmlns:ns0=\"DAV:\">"
        # SELF FIRST, exactly as a real server does - the client has to skip it
        # or a recursive walk never terminates.
        body += entry(base, True, 0)
        if os.path.isdir(p) and depth != "0":
            for name in sorted(os.listdir(p)):
                full = os.path.join(p, name)
                isdir = os.path.isdir(full)
                body += entry(base + name + ("/" if isdir else ""), isdir,
                              0 if isdir else os.path.getsize(full))
        body += "</ns0:multistatus>"
        raw = body.encode()
        self.send_response(207, "Multi-Status")
        self.send_header("Content-Type", "application/xml; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_MKCOL(self):
        p = self._fs()
        if p is None:
            self.send_error(400); return
        if os.path.isdir(p):
            self.send_error(405); return
        os.makedirs(p, exist_ok=True)
        self.send_response(201); self.send_header("Content-Length", "0"); self.end_headers()

    def do_PUT(self):
        p = self._fs()
        if p is None:
            self.send_error(400); return
        n = int(self.headers.get("Content-Length", "0"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(self.rfile.read(n) if n else b"")
        self.send_response(201); self.send_header("Content-Length", "0"); self.end_headers()

    def translate_path(self, path):
        return self._fs() or Dav.root


def serve(root, port):
    Dav.root = root
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), Dav)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


# ---------------------------------------------------------------------------
def main():
    import tempfile
    esp = os.path.join(HERE, "..", "build", "esp")
    if not os.path.isdir(esp) or not os.path.exists(os.path.join(esp, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)")
        return 1

    root = tempfile.mkdtemp(prefix="xfersrv-")
    build_tree(root)
    serve(root, DAV_PORT)

    remote_qemu.PORT = URC_PORT
    link = UnoAutoLink("127.0.0.1", URC_PORT)
    link.listen()
    remote_qemu.build_disk()
    q = remote_qemu.boot_qemu()

    ok = True

    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and bool(cond)

    def xfer(*args, timeout=30):
        return link.command("xfer", *args, timeout=timeout)

    try:
        if not link.wait_connected(120):
            print("FAIL: the guest never dialled in - is this the DEBUG build?")
            return 1
        print("PASS guest dialled in")

        # --- the verb exists and is reachable at all ------------------------
        caps = xfer("caps")
        check(any("scp" in l for l in caps), "xfer caps answers", "%d line(s)" % len(caps))
        check(any("webdav ready" in l for l in caps), "the webdav backend is ready")
        check(any("sftp unavailable" in l for l in caps),
              "sftp reports UNAVAILABLE rather than pretending",
              "(waiting on unossh's ssh_subsystem)")

        # --- pick a writable volume the same way a person would -------------
        vols = link.vols(timeout=15)
        cand = ([v for v in vols if v["writable"] and v["kind"] == 1] or
                [v for v in vols if v["writable"] and v["kind"] != 0] or
                [v for v in vols if v["writable"]])
        check(bool(cand), "a writable volume exists", "vols=%r" % vols)
        if not cand:
            return 1
        VOL = cand[0]["vol"]
        native = cand[0]["kind"] == 1
        print("     using volume %d (kind %d)" % (VOL, cand[0]["kind"]))

        base = "webdav://%s:%d/" % (GUEST_HOST, DAV_PORT)

        # --- one listing, before anything moves -----------------------------
        ls = xfer("ls", base)
        names = " ".join(ls)
        check("readme.txt" in names and "sub" in names, "xfer ls over WebDAV", names[:90])
        # The collection itself must NOT appear in its own listing.
        check(sum(1 for l in ls if l.strip().endswith("sub")) == 1,
              "the collection is not listed inside itself")

        # --- a single file over plain HTTP, which cannot list ---------------
        http_url = "http://%s:%d/readme.txt" % (GUEST_HOST, DAV_PORT)
        r = xfer("pull", http_url, "/readme.txt", str(VOL), "\\HTTP1.TXT")
        jid = None
        for l in r:
            if l.startswith("id="):
                jid = int(l.split("=")[1].split()[0])
        check(jid is not None, "http pull started", " ".join(r)[:90])

        # --- and the recursive one, which is the whole point ----------------
        if native:
            r = xfer("pull", base, "/", str(VOL), "\\XFER", "-r")
        else:
            # Only native FAT has subdirectories, so on a firmware-SFS or RAM
            # volume the recursive case cannot be run at all.  Say so rather
            # than reporting a pass for a test that did not happen.
            r = []
        rid = None
        for l in r:
            if l.startswith("id="):
                rid = int(l.split("=")[1].split()[0])
        if native:
            check(rid is not None, "recursive pull started", " ".join(r)[:90])
        else:
            print("SKIP recursive pull  (no native-FAT volume in this boot)")

        # --- wait for both, watching status ---------------------------------
        def wait(jid, label, limit=180):
            if jid is None:
                return None
            last = ""
            t0 = time.time()
            while time.time() - t0 < limit:
                st = xfer("status", str(jid))
                last = st[0] if st else ""
                if "state=done" in last or "state=failed" in last or "state=cancelled" in last:
                    break
                time.sleep(1.0)
            check("state=done" in last, label, last[:120])
            return last

        wait(jid, "http single-file job reached done")
        wait(rid, "recursive job reached done")

        # --- the only verification that counts: the BYTES ------------------
        # The guest's MicroPython has no hashlib, so the fingerprint is a
        # POSITION-WEIGHTED sum.  Unlike a plain sum it is sensitive to order,
        # so it also catches a file whose bytes all arrived and arrived
        # rearranged - which is the failure a chunked transfer actually has.
        def guest_sum(path):
            # ONE LINE.  `py` takes a single line - the URC frame is
            # newline-delimited, so an embedded newline in the source does not
            # make a two-line program, it makes a second frame the dispatcher
            # tries to read as a verb.
            out = link.eval(
                'import uno; d=uno.read(%d,"%s"); '
                'print(len(d) if d is not None else -1, '
                '(sum((i+1)*b for i,b in enumerate(d))&0xffffffff) if d else 0)'
                % (VOL, path.replace("\\", "\\\\")), timeout=60)
            if not out:
                return (-1, 0)
            parts = out[0].split()
            if not parts:
                return (-1, 0)
            return (int(parts[0]), int(parts[1]) if len(parts) > 1 else 0)

        def want(data):
            return (len(data), sum((i + 1) * b for i, b in enumerate(data)) & 0xFFFFFFFF)

        got = guest_sum("\\HTTP1.TXT")
        check(got == want(TREE["readme.txt"]),
              "HTTP pull landed byte-identical", "%r vs %r" % (got, want(TREE["readme.txt"])))

        if native and rid is not None:
            for rel, data in TREE.items():
                dos = "\\XFER\\" + "\\".join(
                    p.upper()[:8] if "." not in p else
                    (p.split(".")[0].upper()[:8] + "." + p.split(".")[-1].upper()[:3])
                    for p in rel.split("/"))
                got = guest_sum(dos)
                check(got == want(data), "recursive: " + rel,
                      "%s -> %r want %r" % (dos, got, want(data)))

            # --- and no partial must survive a successful job ---------------
            # The rename IS the commit point, so a leftover work file means
            # something was written and never committed - which a checksum test
            # that only looks at the names it expects cannot see.
            out = link.eval(
                'import uno; print(1 if uno.size(%d,"\\\\XFER\\\\ODD.$$$")>=0 else 0)'
                % VOL, timeout=30)
            check(out and out[0].strip() == "0", "no partial file survived the commit",
                  repr(out))

            # --- the per-file log is readable after the job ended -----------
            lg = xfer("log", str(rid))
            check(any(l.startswith("ok ") for l in lg),
                  "xfer log lists the per-file results", "%d line(s)" % len(lg))

        # --- refusing what cannot be done, with a reason --------------------
        # A refusal comes back as an `err` response, which the link raises.
        # The REFUSAL is the thing under test, so catch it and read the reason:
        # a check that only accepted a clean `ok` here would also pass for a
        # verb that quietly started an impossible job.
        def refusal(*args):
            try:
                return " ".join(xfer(*args))
            except RuntimeError as e:
                return str(e)

        msg = refusal("pull", "tftp://%s/x" % GUEST_HOST, "/x", str(VOL), "\\X", "-r")
        check("list" in msg or "not supported" in msg,
              "recursive TFTP is refused up front, with a reason", msg[:100])

        msg = refusal("ls", "scp://user:hunter2@example.com/x")
        check("not a URL" in msg or "password" in msg.lower(),
              "a URL carrying a password is refused", msg[:100])

        # And the property the whole job-id design rests on: a finished job is
        # still queryable, because a client that lost the link comes back to
        # ask what happened - which is exactly when it matters.
        if rid is not None:
            st = xfer("status", str(rid))
            check(any("state=done" in l for l in st),
                  "a finished job is still queryable", " ".join(st)[:90])

    finally:
        try:
            link.close()
        except Exception:
            pass
        q.kill()

    print("\n%s" % ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
