# unomedia decoder security review (2026-07-20)

## Bottom line

Adversarial memory-safety review of all 16 unomedia decoders, the core dispatcher/
allocator, and the pc64 consumer (`pc64_media.c`, `apps/photos.c`). **No exploitable
memory-safety bug found**: no heap overflow, OOB read/write, integer-overflow-into-
undersized-alloc, VLC/Huffman table overread, LZ window underflow, or unchecked
palette/chunk index reachable from an attacker-controlled file. This code is
unusually disciplined about untrusted input. Every decoder applies dimension guards
before it allocates, and every write into output/scratch buffers is either
bounds-checked or provably bounded by a preceding validated width.

Correction: a second, deeper pass over the audio decoders found **one genuine
memory-safety issue** (HIGH, out-of-bounds read), below. Everything else holds.

## HIGH-1 (CONFIRMED) - MIDI: OOB read of `kFamily[]` from an unmasked Program Change

`unomedia/um_midi.c`. `kFamily[]` has 16 entries (line 131), indexed
`kFamily[m_ch[chan].program >> 3]` (line 271). The program byte is stored straight
from the file with no 7-bit mask (`m_ch[chan].program = (unsigned char)a;` at lines
490 and 496, where `a` is a raw file byte 0-255). A crafted SMF with a Program
Change data byte >= 0x80 (e.g. `C0 FF`) followed by a Note-On makes `program >> 3` =
16-31, reading `kFamily[16..31]` (up to ~15 `mpatch` structs, ~300 bytes) past the
array; the garbage `wave`/`att_ms`/`gain` are then used. Read-only OOB in `.rodata`
(no attacker-visible leak, garbles the instrument), but genuine UB, fuzz-reachable,
and can fault if `kFamily` lands at a page tail. **Fix:** mask at the store,
`m_ch[chan].program = (unsigned char)(a & 0x7F);` (lines 490, 496).

Verified first-hand: `kFamily[16]` at line 131; unmasked store confirmed at 490/496;
`program >> 3` reaches 31.

## Why the classic bug classes do not hit

- **Integer-overflow allocation sizing:** only `pc64/build.sh` compiles unomedia,
  and pc64 is x86-64 (64-bit `size_t`), so `(long)w*h` never overflows. The central
  guards `UM_MAX_DIM = 16384` and `UM_MAX_PIXELS = 64M` (`unomedia.h:110-111`) are
  enforced in every decoder's `open()` before any size math. `um_image_open`
  (`um_image.c:67-73`) re-checks `w/h` as a backstop before the consumer's
  `malloc(w*h*4)` in `photos.c:238`.
- **LZ77 / inflate window underflow:** `um_inflate.c:224` guards `dist > z->total`;
  reserved symbols 30/31 and 286/287 rejected. GIF LZW prefix links strictly descend
  and `stack[4096]` writes gated by `next < 4096` (`um_gif.c:232`). VP8L LZ77 guards
  both `dist > pos` and `len > npix - pos` (`um_webp.c:387-390`).
- **Palette / index overreads:** PNG index checked vs `pal_n` (`um_png.c:212`); BMP
  masks by `(1<<bpp)-1`; TGA colormap range-checked; VP8L color-cache index checked.
- **JPEG (deepest surface):** progressive coefficient indexing provably `< bw*bh*64`;
  AC runs check `k>63`/`k>se`; Huffman `fast[256]`/`vals[256]` bounded by the
  `sum>256` DHT check; chroma upsample stays within `cwv`.
- **MP3:** polyphase write `v[off+i]` (`um_mp3.c:704`) safe because `m_synth_off` is
  always a multiple of 64 (masked `&1023`), so `off+63 <= 1023`. Reservoir
  `m_res[4096]` keeps `m_res_len+need <= RESERVOIR`.
- **AAC:** `max_sfb` rejected if `> ics_nsfb`; spectral `pos+width` checked `<=1024`;
  frame length checked `<= 8192` before read; MP4 `stsz`/`stco`/`stsc` loops bounded;
  recursion depth-limited (`mp4_walk<8`, `mp4_find_asc<10`).
- VP8, WebP, BMP/ICO/RLE, QOI, TGA, PNM, WAV, MIDI all similarly bounded.

## Minor, non-security observations

1. **PNM binary samples not clamped to `maxval`** (`um_pnm.c:193-201`): a P5/P6 with
   `maxval=1` and a sample byte of 255 yields a garbled color, but still a single
   in-bounds write. Cosmetic; add the clamp the ASCII path already has.
2. **MIDI/MP3/AAC/WAV-float use floating point** despite the library's stated "no
   float" design rule. Documented-invariant mismatch, not a security issue.
3. **Testing gap (highest-value addition):** `unomedia/test/` are round-trip /
   reference tests only. There is **no malformed-input / fuzz harness**. Given how
   much of the safety rests on precise guards, a libFuzzer/AFL harness over
   `um_image_open`+`um_image_frame` and `um_audio_open`+`um_audio_decode` (with the
   allocator and a memory byte-source) would lock in what is currently verified only
   by hand-tracing. The code is written to fail cleanly on every path reached, so it
   should hold up, but that is currently unverified by fuzzing.
