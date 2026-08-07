/* ===========================================================================
 * unostream - guest-side screen STREAMER for demo-video capture.
 *
 * The `screen record` ring (unoauto_screen.c) captures a session into RAM and
 * the client pulls it afterwards; unostream is the live sibling: the guest
 * dials OUT to a host receiver over TCP and PUSHES frames on its own shell
 * tick, so a demo records at a steady fps for as long as the disk on the HOST
 * side lasts, not until a 4 MB ring fills. The host end
 * (tools/demo/stream_recv.py) decodes and pipes raw frames to ffmpeg.
 *
 * Wire protocol (v1, little-endian; see pc64/UNOSTREAM.md):
 *   on connect, one 16-byte hello:
 *       "UNSM" (4) | ver u8=1 | pad u8 | fps u8 | scale u8 |
 *       w u16le | h u16le | reserved u32le
 *     w/h are the EMITTED dims (FB_W/scale x FB_H/scale). A fresh hello
 *     mid-stream (desktop resolution changed) is a stream reset: the receiver
 *     re-arms on the new geometry and expects a keyframe next.
 *   then frames, each:
 *       type u8 (0 = keyframe: payload is a full-frame QOI image;
 *                1 = delta: payload is [QOI strip of changed 32x32 tiles]
 *                           [manifest: u16le tile index per changed tile],
 *                same shape as unoauto_screen's delta grab) |
 *       pad u8 | reserved u16le=0 | payload_len u32le | payload
 *     A delta's tile count is the strip's QOI height / 32 (the receiver reads
 *     it from the payload's own QOI header), so the manifest is the trailing
 *     2*nch bytes. payload_len == 0 is a valid "nothing changed" delta.
 *     A keyframe is forced every 120 frames.
 *
 * Driven by the URC `stream` verb (unoauto_remote.c pass-through, OBSERVE
 * power like `screen` - it exports exactly what the screen shows). Ships in
 * production: the gate is privilege, not a compile flag.
 * ======================================================================== */
#ifndef UNOSTREAM_H
#define UNOSTREAM_H

/* URC verb entry: `line` is everything after the `stream` verb token, i.e.
 *   start <ip4> <port> [fps] [scale]   dial out and start streaming
 *                                      (fps default 30, clamped 1..60;
 *                                       scale default 1, clamped 1..8)
 *   stop                               close the stream
 *   status                             one status line
 * Writes a NUL-terminated reply into out (cap bytes) and returns its length,
 * or -1 on error (out then carries the error text). `status` replies
 *   on=<0|1> fps=<n> sent=<frames> bytes=<n> drops=<n>
 * NOTE: the args string is parsed in place (tokenised), like every other URC
 * verb - pass a mutable buffer. */
int  unostream_cmd(char *line, char *out, int cap);

/* One call per shell frame (pc64_uui.c frame loop). Paces capture at the
 * requested fps, encodes at most ONE frame per tick, pumps the non-blocking
 * socket, and stands the stream down on close/stall. No-op while idle. */
void unostream_tick(void);

#endif
