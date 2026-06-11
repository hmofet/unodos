"""Replicate fdd_encode_track's algorithm and validate the result."""
import struct

SPT = 11

def enc_half(data, prev_bit, out):
    """data = 32 bits in $55555555 positions; returns last data bit."""
    d = data & 0x55555555
    l = (d << 1) & 0xFFFFFFFF
    r = d >> 1
    clk = ~(d | l | r) & 0xAAAAAAAA
    enc = d | clk
    # boundary clock at bit31
    if prev_bit:
        enc &= ~(1 << 31)
    else:
        if d & (1 << 30):
            enc &= ~(1 << 31)
        else:
            enc |= (1 << 31)
    out.append(enc & 0xFFFFFFFF)
    return d & 1

def enc_long(value, prev_bit, out):
    prev_bit = enc_half((value >> 1) & 0x55555555, prev_bit, out)
    prev_bit = enc_half(value & 0x55555555, prev_bit, out)
    return prev_bit

def encode_track(track_no, sectors):
    out_words = []
    longs = []
    # lead gap: 64 words AAAA = 32 longs
    raw = [0xAAAAAAAA] * 32
    for sec in range(SPT):
        raw.append(0xAAAAAAAA)              # pre-sync zeros
        raw.append(0x44894489)              # sync
        prev = 1                            # sync ends in data bit 1
        info = (0xFF << 24) | (track_no << 16) | (sec << 8) | (SPT - sec)
        info_start = len(raw)
        prev = enc_long(info, prev, raw)
        for _ in range(4):
            prev = enc_long(0, prev, raw)
        hck = 0
        for l in raw[info_start:info_start + 10]:
            hck ^= l
        hck &= 0x55555555
        prev = enc_long(hck, prev, raw)
        # data checksum placeholder
        slot = len(raw)
        prev = enc_long(0, prev, raw)
        data_start = len(raw)
        data = sectors[sec]
        dl = list(struct.unpack(">128I", data))
        for v in dl:
            prev = enc_half((v >> 1) & 0x55555555, prev, raw)
        for v in dl:
            prev = enc_half(v & 0x55555555, prev, raw)
        dck = 0
        for l in raw[data_start:data_start + 256]:
            dck ^= l
        dck &= 0x55555555
        # backfill (asm uses prev=1 fudge then global fixup)
        tmp = []
        enc_long(dck, 1, tmp)
        raw[slot] = tmp[0]
        raw[slot + 1] = tmp[1]
    while len(raw) < 6400 // 2:
        raw.append(0xAAAAAAAA)
    # global clock fixup pass (mirrors the asm)
    for i in range(1, len(raw)):
        prev_bit = raw[i - 1] & 1
        cur = raw[i]
        if prev_bit:
            cur &= ~(1 << 31)
        else:
            if cur & (1 << 30):
                cur &= ~(1 << 31)
            else:
                cur |= (1 << 31)
        raw[i] = cur & 0xFFFFFFFF
    return raw

def mfm_rule_check(raw):
    """MFM legality: no two adjacent 1 bits; no more than three 0s."""
    bits = []
    for l in raw:
        for i in range(31, -1, -1):
            bits.append((l >> i) & 1)
    ones = zeros = 0
    bad1 = bad0 = 0
    prev = 0
    run0 = 0
    for i, b in enumerate(bits):
        if b and prev:
            bad1 += 1
        if b == 0:
            run0 += 1
            if run0 > 3:
                bad0 += 1
        else:
            run0 = 0
        prev = b
    return bad1, bad0

def decode_check(raw, track_no):
    """Decode like fdd_decode_track and verify checksums."""
    words = []
    for l in raw:
        words.append((l >> 16) & 0xFFFF)
        words.append(l & 0xFFFF)
    found = {}
    i = 0
    while i < len(words) - 600:
        if words[i] != 0x4489:
            i += 1
            continue
        while i < len(words) and words[i] == 0x4489:
            i += 1
        def get_long(k):
            return (words[k] << 16) | words[k + 1]
        odd = get_long(i); even = get_long(i + 2)
        info = (((odd & 0x55555555) << 1) | (even & 0x55555555)) & 0xFFFFFFFF
        fmt = info >> 24
        tt = (info >> 16) & 0xFF
        ss = (info >> 8) & 0xFF
        if fmt != 0xFF or tt != track_no or ss >= SPT:
            continue
        # header checksum check
        hraw = [get_long(i + k * 2) for k in range(10)]
        hck_calc = 0
        for l in hraw: hck_calc ^= l
        hck_calc &= 0x55555555
        ho = get_long(i + 20); he = get_long(i + 22)
        hck_disk = (((ho & 0x55555555) << 1) | (he & 0x55555555)) & 0xFFFFFFFF
        # data checksum
        do = get_long(i + 24); de = get_long(i + 26)
        dck_disk = (((do & 0x55555555) << 1) | (de & 0x55555555)) & 0xFFFFFFFF
        draw = [get_long(i + 28 + k * 2) for k in range(256)]
        dck_calc = 0
        for l in draw: dck_calc ^= l
        dck_calc &= 0x55555555
        data = bytearray()
        for k in range(128):
            o = draw[k]; e = draw[128 + k]
            v = (((o & 0x55555555) << 1) | (e & 0x55555555)) & 0xFFFFFFFF
            data += struct.pack(">I", v)
        found[ss] = (hck_calc == hck_disk, dck_calc == dck_disk, bytes(data))
        i += 28 + 512
    return found

secs = [bytes([(s * 7 + j) & 0xFF for j in range(512)]) for s in range(SPT)]
raw = encode_track(40, secs)
bad1, bad0 = mfm_rule_check(raw)
print(f"MFM rule violations: adjacent-ones={bad1} zero-runs>3={bad0}")
found = decode_check(raw, 40)
print(f"sectors decoded: {sorted(found.keys())}")
for ss in sorted(found):
    hok, dok, data = found[ss]
    print(f"  sec {ss}: hck={'OK' if hok else 'BAD'} dck={'OK' if dok else 'BAD'} data={'OK' if data == secs[ss] else 'BAD'}")
