#!/bin/bash
# Generate tiny 440Hz tone clips and embed them as a C header for SPECTEST's
# audio-decoder checks. Run under WSL (needs ffmpeg + xxd).
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=0.15" -ar 32000 -ac 1 -b:a 32k -f mp3 "$T/tm.mp3" 2>/dev/null
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=0.15" -ar 32000 -ac 1 -c:a aac -f adts "$T/tm.aac" 2>/dev/null
echo "mp3=$(stat -c %s "$T/tm.mp3") bytes, aac=$(stat -c %s "$T/tm.aac") bytes"
H=spec_media.h
{
  echo "/* Tiny 0.15s 440 Hz tone clips for SPECTEST audio-decoder checks (debug"
  echo " * build only). Regenerate with tools/gen-spec-media.sh. */"
  echo "#ifndef PC64_SPEC_MEDIA_H"
  echo "#define PC64_SPEC_MEDIA_H"
  xxd -i -n g_spec_mp3 "$T/tm.mp3" | sed 's/^unsigned/static const unsigned/'
  xxd -i -n g_spec_aac "$T/tm.aac" | sed 's/^unsigned/static const unsigned/'
  echo "#endif"
} > "$H"
rm -rf "$T"
echo "wrote $H ($(wc -l < "$H") lines)"
