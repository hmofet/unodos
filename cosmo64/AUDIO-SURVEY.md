# The MT6771 AFE and MT6358 codec, surveyed from the running Linux

**Status: SURVEY ONLY. This machine does not make a sound yet.** What follows
is measured, not guessed, and it exists so the bring-up that follows is
transcription rather than archaeology. Every address here came off the device
on 2026-09-05 by the same technique that produced the RTC map (`rtc.c`): make
Linux do the thing, and watch what changes.

## Why the OS half is nearly free

`pc64/snd_pcm.c` asks a backend for three functions and nothing else:

```c
if (uno_hda_init()) {
    g_ring = uno_hda_ring(&g_frames);   /* interleaved s16 stereo ring */
    g_pos  = uno_hda_pos;               /* hardware read cursor, frames */
    g_kick = 0;                         /* optional, AC'97 only         */
}
```

Everything above that seam is portable and already compiles for aarch64: the
square voice the Sound Manager drives, the sample stream the Music player
pushes, the effects mixer, the resampler, and the looping-ring design that
makes underruns benign. So the whole job is **one MT6771 AFE backend of that
shape**, plus a seam in `snd_pcm.c` to try it (the `#ifndef` pattern already
used for `pc64_http.c` and `tls_entropy.c`).

## The AFE

Base address **`0x11220000`**, from the device tree (`audio@11220000`, 4 KB;
`audio_sram@11221000` is a further 768 KB of SRAM). Register offsets below are
from that base, and they are the classic MediaTek AFE offsets -- confirmed,
not assumed, by two independent landmarks: `0x010` reads 1 when Linux is idle
(AFE_ON) and 3 while playing (AFE_ON | DL1_ON), and `0x1f0` changes when and
only when the sine generator is toggled.

| offset | name | measured |
|---|---|---|
| `0x010` | `AFE_DAC_CON0` | `1` idle, **`3` streaming** -- bit 0 = AFE on, bit 1 = DL1 on |
| `0x014` | `AFE_DAC_CON1` | `0x00000aaa` (rate/format fields) |
| `0x040` | `AFE_DL1_BASE` | `0x70390000` -- the ring, in DRAM |
| `0x044` | `AFE_DL1_CUR` | **the DMA read cursor** (see below) |
| `0x048` | `AFE_DL1_END` | `0x7039bfff` -- a 48 KB ring |
| `0x04c` | DL1 attribute | `0x00000a0b` |
| `0x02c`, `0x030` | IRQ counters | `0x20`, `0x40` |
| `0x034` | | `0x00000a0b` |
| `0x0cc` | memif MSB/attr | `0x01ff0000` |
| `0x1f0` | `AFE_SGEN_CON0` | `0x00580580` with the sine generator on |

**`0x044` is the cursor `uno_afe_pos()` needs**, and this is the measurement
that matters most, because everything else can be copied but a wrong cursor
produces audio that sounds fine for a second and then tears. Five reads,
300 ms apart, during a real stream:

```
base=70390000 cur=703989b0 end=7039bfff
base=70390000 cur=7039b450 end=7039bfff
base=70390000 cur=70391e00 end=7039bfff   <- wrapped
base=70390000 cur=70394830 end=7039bfff
base=70390000 cur=70397400 end=7039bfff
```

It advances monotonically and wraps at `END`, so `pos()` is
`(CUR - BASE) / 4` frames.

## The codec (MT6358, over PWRAP)

The analog path lives in the PMIC's **`0x2200`-`0x2500`** band -- a different
neighbourhood from the power rails `pmic.c` guards (`0x1B00`-`0x1E00`), which
matters for the risk argument below. Turning the sine generator on from a cold
idle changed exactly these, `off -> on`:

```
  0x220c  0x66   -> 0x0        0x2408  0x3000 -> 0x3aff
  0x2240  0x1    -> 0x0        0x240a  0x0    -> 0x3f03
  0x2288  0x0    -> 0x1        0x240c  0x8033 -> 0xc033
  0x228a  0x0    -> 0x1        0x2410  0x0    -> 0x40
  0x2292  0xef   -> 0x2a       0x241a  0x0    -> 0xf201
  0x2296  0xcba0 -> 0xcba1     0x2420  0x155  -> 0x55
  0x229a  0x0    -> 0xb        0x2422  0x10   -> 0x1
  0x22ac  0x0    -> 0x8        0x2424  0x0    -> 0x1055
  0x22d6  0x0    -> 0x2a       0x2426  0x0    -> 0x1
  0x2394  0x60   -> 0x61       0x248a  0xf9f  -> 0x0
                               0x248c  0x912  -> 0x50a
                               0x2492  0x3f3f -> 0x2020
```

and, outside the audio band, four more that are almost certainly the clock
buffer and the audio LDOs: `0xd8` `0x0 -> 0x249`, `0x7ac` `0x82b5 -> 0xa2b5`,
`0x1822` `0x4 -> 0x6`, `0x18aa`/`0x18ac`.

## What is still missing, and it is the hard part

**The ORDER.** A diff gives the set of registers and their target values; it
does not give the sequence, and codec bring-up is order-sensitive (rails and
clock before digital, digital before analog, unmute last, with settling delays
between). Two attempts to recover the order failed and are recorded so nobody
repeats them:

- **`/dev/mem` does not exist** on this kernel and no `devmem`/`devmem2` is
  installed, so the AFE cannot be read directly from Linux -- the debugfs
  regmap (`mt-soc-dl1-pcm`) is the only window, which is why the table above
  is offsets rather than raw MMIO.
- **`regmap` tracepoints do not see the codec.** They exist, but the MT6358 is
  driven through MediaTek's own PWRAP path rather than a regmap, so a trace of
  a bring-up captures only a polling loop from the audio regmap
  (`reg=a8/84/80/a4`, repeating) and nothing from the codec at all.

The order will therefore have to come from experiment on our own image: write
the set, in a plausible order, and use the fact that this is cheap to iterate
over URC once the driver exists.

**The risk to weigh before writing any of it.** These are PMIC writes, and
`pmic.c` deliberately has no arbitrary-write capability -- writes go through a
whitelist with no address parameter, because a wrong address there is not a
corrupted partition but silicon at a voltage it was not built for. Audio needs
about 27 new whitelist entries. The mitigating argument is that they are all
in the audio band and none is a regulator; the honest counter-argument is that
this is exactly the sort of reasoning the whitelist exists to not depend on.
Build with `PMIC_WRITE=0` first, as that file's own header instructs after any
change to its table.

## The suggested first target: the sine generator, not DMA

`AFE_SGEN_CON0` (`0x1f0`) makes the AFE emit a tone with **no DMA ring at
all** -- clocks, codec, one enable. It is a much smaller first light than a
streaming ring, it exercises everything the hard part depends on (the power
domain, the clocks, the codec chain), and it fails in a way that is
unambiguous: either the speaker makes a noise or it does not. Get that, then
add the DL1 ring, and only then wire `snd_pcm.c`.

## Reproducing the survey

Trixie, with `alsa-utils` installed (it is now):

```sh
cat /sys/kernel/debug/regmap/mt-soc-dl1-pcm/registers   # the AFE
cat /sys/kernel/debug/mtk_pmic/dump_pmic_reg            # the whole PMIC
amixer -c 0 cset name='Audio_SineGen_Amplitude' 4       # tone on
aplay -D hw:0,0 -f S16_LE -r 48000 -c 2 \
      --period-size=3072 --buffer-size=12288 /tmp/n.raw # a real stream
```

`speaker-test`'s defaults are rejected by this driver (it picks
`period_size=12000` against a 12288-frame buffer, which does not divide);
pass `-b 256000 -p 64000` or use `aplay` with the explicit sizes above.
