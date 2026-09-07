# The MT6771 AFE and MT6358 codec, surveyed from the running Linux

**Status (2026-09-07): three things stood between the codec set and a sound,
and all three are now in `afe_regs.h` -- the AUDIO power domain in SPM (off at
LK handover), the two GPIO-pulsed external speaker amplifiers, and the second
DL1 -> DAC interconnect (`AFE_CONN28/29`, without which the DAC's SRC monitor
reads 0). See the two sections at the end. The full path (`afe.c` as pc64's PCM
backend) is built behind `AUDIO=1`; the DL SDM monitors show audio flowing.**
What follows
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

## WHAT THE HARDWARE SAID (2026-09-05): it is the clock, not the codec

The sine-generator attempt ran on the device, and it moved the blocker. Two
boots, in order:

```
afe: as found: DAC_CON0=00000000 DAC_CON1=00000000 SGEN=00000000
pmic: AUD 22ac (22ac) wanted 0008, reads 0000
pmic: audio set applied -- 22 of 23 rows took
```

then, after teaching the table which rows are status rather than control:

```
pmic: audio set applied -- 23 of 23 rows took
afe: AFE_ON -> DAC_CON0=00000000 (wanted bit 0 set)
afe: AFE_ON did not stick -- the block is mapped but not clocked.
```

Three things are now settled, and none of them were before:

1. **The AFE is MAPPED and does not fault.** Reads return zeros, not
   all-ones, and writes are accepted without taking the bus down. Whatever is
   wrong, it is not that the address is wrong.
2. **The codec is not the blocker.** All 23 PWRAP rows apply and read back.
   The MT6358's audio registers can be driven from bare metal, which was the
   risk worth being careful about and is now measured rather than hoped.
3. **`AFE_ON` does not stick, and that is the whole problem.** A register that
   accepts a write and reads back zero is the classic signature of a block
   whose bus is alive but whose functional clock is not running. The next work
   is therefore the audio CLOCK GATES and the AUDIO power domain -- topckgen's
   muxes (`audio_sel`, `aud_intbus_sel`, `aud_1_sel`), the INFRACFG module
   gate that Linux calls `infra_audio`, and whatever MTCMOS/SCPSYS state sits
   under them -- and NOT the codec, which is finished.

`0x22ac` also stopped being a mystery: a diff cannot tell a register we drive
from one we merely watched change, and a write it refuses was never a control
bit. Its `0 -> 8` under Linux was a consequence of the codec coming up.

### How to get the clock registers, given /dev/mem is absent

The obstacle is the same one that blocked the ordering: no raw MMIO from
Linux. Two things worth trying before anything else, in this order:

- **`mknod /dev/mem c 1 1`** and read from it. The node is missing, which is
  not the same as the kernel lacking `CONFIG_DEVMEM`; if the driver is there,
  the node is one command and the whole clock tree becomes readable.
- **`/sys/kernel/debug/clk/clk_summary`** already shows the audio tree with
  enable counts and rates (`infra_audio` enabled at 156 MHz, `aud_intbus_sel`,
  `aud_2_sel` at 196.608 MHz). That names the clocks even if it does not give
  their registers, and the MTK clock driver's gate registers are a much
  smaller and better-documented surface than the AFE's.

## The original first target: the sine generator, not DMA

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

## WHAT THE PROBE SAID (2026-09-05, later): the power domain, and nothing else

The clock question was answered without a reflash. `afeprobe.c` builds to
`AFEPROBE.UNO`, a unoui-class module pushed to the SD card over URC (`put`,
`rescan`, `launch afeprobe`) while the phone sat in UnoDOS; a module runs in
the kernel's address space and mmu.c identity-maps the first gigabyte as
Device memory, so it reads and writes SPM, INFRACFG, topckgen and the AFE
directly and narrates on the SCRIPT log channel. Two runs, in order:

```
spm:   cfg=00000003 sta=0000634c sta2=0000634c aud_pwr=0000ff12   <- bit 24 CLEAR
infra: sta1=00000000 (audio bit25=0)  sta2=00000000 (26m bit4=0)  <- open
topck: cfg5=01000100  (audio_sel=0 pdn=0, intbus_sel=1 pdn=0)     <- open
afe:   top0=00000000 dac0=00000000 ...                            <- reads zeros
AFE_ON after nothing: dac0=00000000 -> no
spm: AUDIO domain is OFF -- turning it on
spm: AUDIO on: sta=0100634c sta2=0100634c con=0000000d
AFE_ON after MTCMOS: dac0=00000001 -> STICKS
sgen: wrote 00580580 reads 00580580 -> TAKES
dl1: dac0=00000003 cur 4729f980 472a0940 472a1840 472a2740 -> ADVANCES
```

Three facts, each measured:

1. **The AUDIO MTCMOS was off.** `PWR_STATUS` bit 24 clear, `AUDIO_PWR_CON`
   = `0xff12` (SRAM powered down, isolated, clock disabled). The recovery
   slot's LK never turns it on. `spm_mtcmos_ctrl_audio()`'s on-sequence from
   `clk-mt6771-pg.c` -- `PWR_ON`, `PWR_ON_2ND`, wait for both status bits,
   clear `CLK_DIS`, clear `ISO`, set `RST_B`, release the four SRAM PDN bits
   one at a time -- brought it up first try, every ack arriving.
2. **Nothing else was missing.** `infra_audio` (INFRA_PDN_STA1 bit 25) and
   the 26 MHz bclk gate read open; `audio_sel` sits on clk26m ungated and
   `aud_intbus_sel` on syspll_d2_d4 ungated -- exactly what `AudDrv_Clk_On`
   would have set. `AUDIO_TOP_CON0` came up as `0xa0fd4038`, whose AFE, DAC
   and DAC_PREDIS power-down bits are already clear.
3. **The AFE runs at the rate it should.** The DL1 cursor moved 0xfc0 bytes
   per 20 ms = 192 KB/s = 48000 x 2 x 2. That is the clock, measured.

`POWERON_CONFIG_EN` (the SPM unlock) read `3` at handover and reads `1`
after the project-code write -- the code field is write-only, so the test is
"bit 0 set", not "code present".

The second run did the whole DAC path from the vendor driver
(`c64afe_dac_on()` in `afe_regs.h`: pad-top FIFO 0x31, MTKAIF protocol 1,
predistortion cleared, `AFE_ADDA_DL_SRC2_CON0` = `0x83001802` from
`SetDLSrc2(48000)`, the -0.3 dB gain word, SDM level 0x1d, `AFE_I2S_CON1` =
`0xa0a` from `SetI2SDacOut`, the DL1 -> DAC interconnect) and then DL1 on a
ring holding a 441 Hz sine for three seconds. Every enable bit held:

```
dac path: dac0=00000001 uldl=00006001 src2=83001803 i2s1=00000a0b conn3=00000020 conn4=00000040
dl1: PLAYING a 441 Hz sine for 3000 ms ... dl1: stopped (timer) after 3023 ms
```

Two corrections to the tables above, learned from the vendor source:

- The rows called "IRQ counters" (`0x02c` = `0x20`, `0x030` = `0x40`) are
  **`AFE_CONN3` and `AFE_CONN4`, the interconnect**: bit 5 = I05 (DL1 left)
  into O03 (DAC left), bit 6 = I06 into O04. The survey measured the right
  registers and misnamed them.
- `0x034` = `0xa0b` is **`AFE_I2S_CON1`**, the DAC-out configuration, which
  is `SetI2SDacOut(48000)` to the bit (rate 0xa<<8, I2S format, 32-bit word,
  enable). `0x04c` is `AFE_I2S_CON3`, an I2S output this path does not use.

And one correction to the method: the tone Linux played for the survey was
the sine generator riding on a DAC path Linux had ALREADY configured, so
`0x1f0` really was the only AFE register that changed. On a bare AFE the
generator has nothing to feed; the path above is what was missing between
"SGEN takes its value" and a sound. It comes from the vendor driver's code,
not from a diff, because the diff could not see what was already on.

**What a module cannot do is the codec.** PWRAP is not a module export, on
purpose. So the probe's proof stops at the DAC's digital input, and the
kernel (`afe.c`, `AUDIO=1`) is where the domain, the codec set and this DAC
path first run together. That build exists; it has not been booted.

## WHAT THE FIRST AUDIO=1 BOOT SAID (2026-09-07): silent, for two reasons that were never the codec

The kernel path came up exactly as the probe had -- domain on, 23 of 23 codec
rows, DAC path enabled, DL1 streaming -- and nothing was heard. Two causes,
both outside anything a PMIC diff could show:

1. **The speakers sit behind two external class-D amplifiers.**
   `k71v1_64_bsp.dts` (the Cosmo's board): `extamp` = GPIO153, `extamp2` =
   GPIO111, each switched by a pulse count (`AudDrv_GPIO_EXTAMP_Select`: mode
   3 = three low/high pulses 2 us apart), with `headphone_en` = GPIO108 held
   low first and 25 ms of warm-up (`Ext_Speaker_Amp_Change`). The codec set
   the survey captured is the HEADPHONE output path (`Audio_Amp_Change`:
   AUDDEC_ANA_CON0 0x3aff, CON1 0x3f03, CON2 0xc033, CON9, CON12-15, the HP
   gain in ZCD_CON2); on this board headphone-enable low routes it to the
   amplifiers. `c64afe_extamp_on()` in `afe_regs.h`; the GPIO block is
   `0x10005000` with DIR at 0x000, DOUT at 0x100, MODE at 0x300, SET/CLR at
   +4/+8.
2. **DL1 has to be connected to O28/O29 as well as O03/O04.** Measured on
   Trixie with `aplay` to hw:0,0, idle -> playing, the AFE regmap changed
   `0x02c/0x030` (CONN3/4) AND `0x4bc/0x4c0` (**AFE_CONN28/29**, the
   `I2S1_DAC_2` pair `mtk_pcm_dl1_start` connects with a second
   `SetIntfConnection`). With only CONN3/4 set, the DL SDM's left-channel
   monitor (`0xc64`) read 0 while the ring carried a full-scale chime; with
   CONN28/29 added it read `0x001ffb5b` at once. **The DAC's sample-rate
   converter is fed from O28/O29 on this SoC; O03/O04 alone deliver nothing.**
   Also matched from that diff: `AFE_MEMIF_HDALIGN` bits 16-30 set
   (`set_sram_mode(normal)`).

Two diagnostics worth keeping: `AFE_ADDA_DL_SDM_FIFO_MON` (`0xc60`) and
`AFE_ADDA_DL_SRC_LCH_MON` (`0xc64`) are read-only monitors that are nonzero
only when samples are actually reaching the DAC path -- they separate "the
enable bits are set" from "audio is flowing" without a speaker. And the boot
chime (`uno_pc64_chime`, C E G C) now runs right after `uno_snd_init`, so a
flashed image announces itself.

The Trixie measurement also showed the PMIC audio band does NOT change
between idle and playing while PulseAudio holds the sink, so a "cold idle"
diff needs PulseAudio stopped first (`systemctl --user stop pulseaudio` or
kill it) -- the 2026-09-05 survey diff evidently had it stopped.
