#!/usr/bin/env python3
"""End-to-end gate for netdisc (zero-config LAN discovery), in QEMU.

SLIRP is a NAT and never forwards broadcast, so discovery can't be tested over
it. This harness instead attaches the guest NIC to a REAL L2 segment: QEMU's
`socket` netdev tunnels raw Ethernet frames over a UDP pair, and this process is
the peer on the other end - a minimal host stack (ARP + DHCP + the UNODISC
discovery protocol) that can both SEE the guest's broadcasts and craft replies.

It proves the full discovery exchange over real broadcast:
  1. the guest BROADCASTs a UNODISC PROBE                     (pc64 -> LAN)
  2. we answer with an OFFER (role host, our URC ip:port); the guest records it
     and unicasts back a GOTHOST ack                           (pc64 recorded)
  3. we send our own PROBE; the guest answers with an OFFER (role pc64) carrying
     its leased IP                                             (pc64 answers)

Needs a debug build (UNO_DEBUG=1 ./build.sh) and WSL (qemu + OVMF + mtools).
Exit 0 iff all three are observed.
"""
import os, sys, socket, struct, subprocess, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remote_qemu as rq         # reuse OVMF paths, disk geometry, mtools helpers

HOSTPORT, GUESTPORT = 5610, 5611          # UDP tunnel carrying raw Ethernet
HOST_MAC  = bytes.fromhex("525400aabbcc")
BCAST_MAC = b"\xff" * 6
HOST_IP   = bytes([10, 0, 2, 1])          # us: gateway + DHCP server + disc peer
GUEST_IP  = bytes([10, 0, 2, 100])        # what we lease the guest
MASK      = bytes([255, 255, 255, 0])
DISC_PORT = 5400
URC_PORT  = 5099
HOST_URC  = ("10.0.2.1", URC_PORT)        # the URC listener we advertise (= us, so
                                          # the guest can actually dial it on the hub)
DISK = "/tmp/netdisc_disk.img"
FAT  = "/tmp/netdisc_fat.img"


# ---- checksums / frame helpers --------------------------------------------
def cksum(data):
    if len(data) % 2: data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def eth(dst, src, etype, payload):
    return dst + src + struct.pack("!H", etype) + payload

def ipv4(src, dst, proto, payload):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0, 0, 64, proto, 0, src, dst)
    hdr = hdr[:10] + struct.pack("!H", cksum(hdr)) + hdr[12:]
    return hdr + payload

def udp(sp, dp, payload):
    return struct.pack("!HHHH", sp, dp, 8 + len(payload), 0) + payload   # cksum 0 (optional)


# ---- the host L2 responder -------------------------------------------------
class Peer:
    def __init__(self, sock):
        self.sock = sock
        self.guest_tun = None            # (host, port) QEMU sends from
        self.guest_mac = None
        self.saw_probe = False
        self.got_gothost = None          # the ip:port the guest echoed back
        self.pc64_offer_ip = None        # guest IP from its OFFER
        self.leased = False
        # URC dial (proves auto-dial after discovery)
        self.saw_syn = False             # guest opened a TCP connection to URC_PORT
        self.hello_seen = False          # guest sent a URC HELLO frame -> link up
        self.host_seq = 0x9000

    def send(self, frame):
        if self.guest_tun:
            self.sock.sendto(frame, self.guest_tun)

    def to_guest_udp(self, src_ip, dst_ip, sp, dp, payload, dst_mac=None):
        self.send(eth(dst_mac or self.guest_mac or BCAST_MAC, HOST_MAC, 0x0800,
                      ipv4(src_ip, dst_ip, 17, udp(sp, dp, payload))))

    # -- minimal TCP endpoint (just enough to accept the URC dial + read HELLO) --
    def send_tcp(self, dst_ip, dp, seq, ack, flags, payload=b""):
        seg = struct.pack("!HHIIBBHHH", URC_PORT, dp, seq, ack, 0x50, flags, 65535, 0, 0) + payload
        pseudo = HOST_IP + dst_ip + struct.pack("!BBH", 0, 6, len(seg))
        seg = seg[:16] + struct.pack("!H", cksum(pseudo + seg)) + seg[18:]
        self.send(eth(self.guest_mac or BCAST_MAC, HOST_MAC, 0x0800, ipv4(HOST_IP, dst_ip, 6, seg)))

    def tcp(self, src_ip, seg):
        if len(seg) < 20: return
        sp, dp, seq, ack = struct.unpack("!HHII", seg[0:12])
        off = (seg[12] >> 4) * 4
        flags = seg[13]
        data = seg[off:]
        if dp != URC_PORT: return
        if (flags & 0x12) == 0x02:                      # SYN (no ACK): guest dialing us
            self.saw_syn = True
            self.host_seq = 0x9000
            self.send_tcp(src_ip, sp, self.host_seq, seq + 1, 0x12)   # SYN,ACK
            self.host_seq += 1
        elif data:                                      # payload: ACK it, look for HELLO
            self.send_tcp(src_ip, sp, self.host_seq, seq + len(data), 0x10)
            if b"HELLO" in data:
                self.hello_seen = True

    # -- ARP -----------------------------------------------------------------
    def arp(self, frame):
        a = frame[14:]
        if len(a) < 28: return
        op = struct.unpack("!H", a[6:8])[0]
        sha, spa, tpa = a[8:14], a[14:18], a[24:28]
        self.guest_mac = self.guest_mac or sha
        if op == 1 and tpa == HOST_IP:                 # request for us -> reply
            reply = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2) + HOST_MAC + HOST_IP + sha + spa
            self.send(eth(sha, HOST_MAC, 0x0806, reply))

    # -- DHCP server ---------------------------------------------------------
    def dhcp(self, boot):
        if len(boot) < 240 or boot[0] != 1: return     # BOOTREQUEST
        xid = boot[4:8]; chaddr = boot[28:34]
        # find option 53 (message type)
        i, mtype = 240, None
        while i + 1 < len(boot):
            t = boot[i]
            if t == 255: break
            if t == 0: i += 1; continue
            l = boot[i + 1]
            if t == 53: mtype = boot[i + 2]
            i += 2 + l
        if mtype not in (1, 3): return                 # DISCOVER(1) / REQUEST(3)
        reply_type = 2 if mtype == 1 else 5            # OFFER / ACK
        pkt = bytearray(240)
        pkt[0] = 2; pkt[1] = 1; pkt[2] = 6
        pkt[4:8] = xid
        pkt[16:20] = GUEST_IP                          # yiaddr
        pkt[28:34] = chaddr
        pkt[236:240] = struct.pack("!I", 0x63825363)   # magic cookie
        opts = bytes([53, 1, reply_type,
                      1, 4, *MASK,
                      3, 4, *HOST_IP,
                      6, 4, *HOST_IP,
                      54, 4, *HOST_IP,
                      51, 4, 0, 0, 0x0E, 0x10,          # lease 3600s
                      255])
        self.to_guest_udp(HOST_IP, bytes([255, 255, 255, 255]), 67, 68,
                          bytes(pkt) + opts, dst_mac=self.guest_mac or BCAST_MAC)
        if reply_type == 5:
            self.leased = True

    # -- UNODISC -------------------------------------------------------------
    def disc(self, src_ip, sp, payload):
        try:
            parts = payload.decode("ascii", "replace").split()
        except Exception:
            return
        if len(parts) < 3 or parts[0] != "UNODISC": return
        typ = parts[2]
        if typ == "PROBE":                              # guest -> LAN: answer with our OFFER
            self.saw_probe = True
            offer = "UNODISC 1 OFFER host devbuntu 1 %s %d" % HOST_URC
            self.to_guest_udp(HOST_IP, src_ip, DISC_PORT, sp, offer.encode())
        elif typ == "OFFER" and len(parts) >= 8 and parts[3] == "pc64":
            self.pc64_offer_ip = parts[6]               # guest answered our PROBE
        elif typ == "GOTHOST" and len(parts) >= 5:
            self.got_gothost = (parts[3], parts[4])     # guest recorded our host offer

    def our_probe(self):
        p = b"UNODISC 1 PROBE host devbuntu 1"
        self.to_guest_udp(HOST_IP, bytes([255, 255, 255, 255]), DISC_PORT, DISC_PORT, p,
                          dst_mac=BCAST_MAC)

    # -- frame dispatch ------------------------------------------------------
    def feed(self, frame, tun):
        self.guest_tun = tun
        if len(frame) < 14: return
        etype = struct.unpack("!H", frame[12:14])[0]
        self.guest_mac = self.guest_mac or frame[6:12]
        if etype == 0x0806:
            self.arp(frame)
        elif etype == 0x0800:
            ip = frame[14:]
            if len(ip) < 20: return
            ihl = (ip[0] & 0x0F) * 4
            iplen = struct.unpack("!H", ip[2:4])[0]
            proto = ip[9]; src_ip = ip[12:16]
            l4 = ip[ihl:iplen] if iplen >= ihl else ip[ihl:]
            if proto == 6:
                self.tcp(src_ip, l4)
                return
            if proto != 17 or len(l4) < 8: return
            sp, dp = struct.unpack("!HH", l4[0:4])
            payload = l4[8:8 + (struct.unpack("!H", l4[4:6])[0] - 8)]
            if dp == 67:
                self.dhcp(payload)
            elif dp == DISC_PORT:
                self.disc(src_ip, sp, payload)


def build_disk():
    cfg = "/tmp/netdisc_stress.cfg"
    with open(cfg, "w", newline="\r\n") as f:
        f.write("discover\nnonet\n")                    # arm discovery, skip boot net test
    rq.DISK, rq.FAT = DISK, FAT                          # redirect rq's disk builder
    orig = rq.build_disk.__globals__
    # rq.build_disk writes its own cfg; replicate its geometry but our cfg:
    SECTOR, MIB = 512, 1 << 20
    disk_sectors = 96 * 2048
    with open(DISK, "wb") as f: f.truncate(disk_sectors * SECTOR)
    rq.sh(["sgdisk", "--zap-all", DISK], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rq.sh(["sgdisk", "-n", "1:2048:0", "-t", "1:EF00", "-c", "1:UNODOS", DISK],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    part_start = 2048; part_sectors = disk_sectors - part_start - 2048
    with open(FAT, "wb") as f: f.truncate(part_sectors * SECTOR)
    rq.sh(["mformat", "-i", FAT, "-F", "-T", str(part_sectors), "::"],
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for root, dirs, files in os.walk(rq.ESP):
        rel = os.path.relpath(root, rq.ESP)
        if rel != ".":
            rq.sh(["mmd", "-i", FAT, "::/" + rel.replace(os.sep, "/")],
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for fn in files:
            src = os.path.join(root, fn)
            dst = "::/" + (fn if rel == "." else rel.replace(os.sep, "/") + "/" + fn)
            rq.sh(["mcopy", "-i", FAT, "-o", src, dst], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rq.sh(["mcopy", "-i", FAT, "-o", cfg, "::/STRESS.CFG"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(FAT, "rb") as pf, open(DISK, "r+b") as df:
        df.seek(part_start * SECTOR)
        while True:
            b = pf.read(MIB)
            if not b: break
            df.write(b)


def boot_qemu():
    rq.sh(["cp", rq.OVMF_VARS, rq.VARS])
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-m", "512", "-cpu", "max",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + rq.OVMF_CODE,
        "-drive", "if=pflash,format=raw,file=" + rq.VARS,
        "-drive", "format=raw,file=" + DISK,
        # raw-Ethernet tunnel: guest frames -> udp HOSTPORT; our frames -> localaddr GUESTPORT
        "-netdev", "socket,id=n0,udp=127.0.0.1:%d,localaddr=127.0.0.1:%d" % (HOSTPORT, GUESTPORT),
        "-device", "e1000,netdev=n0",
        "-display", "none",
    ]
    return subprocess.Popen(cmd, stderr=subprocess.DEVNULL)


def main():
    if not os.path.exists(os.path.join(rq.ESP, "APPS", "PYRT.UNO")):
        print("FAIL: no debug build at build/esp (run UNO_DEBUG=1 ./build.sh)"); return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", HOSTPORT))
    sock.settimeout(0.3)
    peer = Peer(sock)

    build_disk()
    q = boot_qemu()
    stop = threading.Event()

    def pump():
        while not stop.is_set():
            try:
                frame, tun = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                peer.feed(frame, tun)
            except Exception as e:  # noqa: BLE001
                print("  (feed error: %s)" % e)
    threading.Thread(target=pump, daemon=True).start()

    ok = True
    def check(cond, label, detail=""):
        nonlocal ok
        print(("PASS" if cond else "FAIL") + " " + label + (("  " + detail) if detail else ""))
        ok = ok and cond

    try:
        # give the guest time to boot, lease, and start probing; send our own
        # PROBE periodically so it answers.
        deadline = time.time() + 75
        last_probe = 0
        while time.time() < deadline:
            if peer.leased and time.time() - last_probe > 2:
                peer.our_probe(); last_probe = time.time()
            if peer.saw_probe and peer.got_gothost and peer.pc64_offer_ip and peer.hello_seen:
                break
            time.sleep(0.2)

        check(peer.leased, "guest took a DHCP lease over the L2 hub")
        check(peer.saw_probe, "guest BROADCAST a discovery PROBE")
        check(peer.got_gothost == (HOST_URC[0], str(HOST_URC[1])),
              "guest recorded our host OFFER (GOTHOST ack)", str(peer.got_gothost))
        gip = ".".join(str(b) for b in GUEST_IP)
        check(peer.pc64_offer_ip == gip,
              "guest ANSWERED our PROBE with an OFFER", "pc64 ip=%s" % peer.pc64_offer_ip)
        # auto-dial: with no remote= key, the remote channel dials the DISCOVERED host
        check(peer.saw_syn, "remote channel auto-DIALED the discovered host (TCP SYN to :%d)" % URC_PORT)
        check(peer.hello_seen, "URC link came up (guest sent HELLO) - zero-config end to end")

        print("\n>> " + ("netdisc discovery OK" if ok else "FAILURES ABOVE"))
        return 0 if ok else 1
    finally:
        stop.set()
        try: q.terminate()
        except Exception: pass
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
