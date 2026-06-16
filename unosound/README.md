# unosound — the audio subsystem voice/score floor (Phase 9, CONTRACT-ARCH §6)

The floor sits at the **voice/note** altitude, not PCM — the §6 "audio-altitude
trap": SID/Paula/DOC/SPC700/PSG are *synthesis* chips, not PCM DACs. A **score** of
notes is the write-once input (the Tracker song format is byte-identical on every
port). Chiptune backends realize voices natively; PCM platforms software-synth the
same score. `unosound.c` is that software floor + a WAV sink for host evidence.

`sh unosound/build.sh` verifies: a sustained A440 square wave measures ~440 Hz by
zero-crossing count (±3%); a `rest` renders true silence; an 8-note melody across
square/triangle/saw renders non-silent PCM to `build/unosound.wav`. Blocked tail:
the native chiptune accel backends (SID/Paula/DOC/SPC700/PSG) need their hardware/
emulators and an ear check.
