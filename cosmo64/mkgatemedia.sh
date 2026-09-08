#!/bin/sh
# cosmo64/mkgatemedia.sh -- the QEMU gate's audio files, made ON quill.
#
#   ssh quill 'cd /work/unodos-pc64arm/cosmo64 && sh mkgatemedia.sh'
#   -> build/gatemedia/{GATE.WAV,GATE.MP3,GATE.M4A,SCALE.MID}
#
# WHY GENERATED AND NOT pc64/media. The demo media there is right for a
# listener and wrong for a gate: ODEJOY.WAV is 700 KB and FURELISE.MP3 plays
# for 13 s, and a gate that pushes them over the serial URC link and waits
# for them to end costs a minute per run for no more proof than three
# seconds gives. These are three seconds each, one per decoder family, from
# ffmpeg's own generators -- so nothing here is anyone's recording -- plus
# the smallest committed MIDI file, which the MIDI synthesiser renders in a
# fraction of a second of audio. Regenerated on demand; not committed.
#
# The shape of each file is chosen for the decoder under it:
#   GATE.WAV  22.05 kHz mono s16   -- the resampler's mono + rate paths
#   GATE.MP3  44.1 kHz stereo 128k -- LAME frames with an ID3v2 tag in front
#   GATE.M4A  44.1 kHz stereo AAC-LC, brand M4A, moov FIRST (+faststart) --
#             the MP4 demuxer's ordinary case; unomedia refuses HE-AAC and
#             a moov at the end is a seek the windowed source can do but the
#             gate need not pay for
#   SCALE.MID a 197-byte scale: the sequencer-backed decoder end to end
set -e
cd "$(dirname "$0")"
OUT=build/gatemedia
mkdir -p "$OUT"
F="ffmpeg -y -hide_banner -loglevel error"
# two tones a fifth apart, 3 s: audibly a chord on the phone, and a signal
# whose peak level the ring's VU can see under QEMU
SRC="-f lavfi -i aevalsrc=0.4*sin(440*2*PI*t)+0.3*sin(660*2*PI*t):s=44100:d=3"
$F $SRC -ac 1 -ar 22050 -c:a pcm_s16le "$OUT/GATE.WAV"
$F $SRC -ac 2 -c:a libmp3lame -b:a 128k -id3v2_version 3 \
   -metadata title="UnoDOS gate tone" "$OUT/GATE.MP3"
$F $SRC -ac 2 -c:a aac -profile:a aac_low -b:a 96k -movflags +faststart \
   -metadata title="UnoDOS gate tone" "$OUT/GATE.M4A"
cp ../pc64/media/SCALE.MID "$OUT/SCALE.MID"
ls -l "$OUT"
