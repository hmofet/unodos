# unolog - the UnoDOS system log

The record of what this machine did, kept in production, readable by the person
using it, and speakable to the rest of the network.

Not to be confused with the **debug harness** (`uno_debug.c`), which keeps a raw
byte ring inside the crash stash and compiles to `((void)0)` in every production
build. That is forensics for a developer holding a failing machine. This is the
system log: levelled, persistent, user-visible, and on the wire.

## 1. Severities are syslog's, on purpose

```
0 EMERG   the system is unusable
1 ALERT   action must be taken immediately
2 CRIT    critical condition
3 ERR     error
4 WARNING warning
5 NOTICE  normal but significant
6 INFO    informational
7 DEBUG   debug-level
```

Interoperating with rsyslog is a requirement rather than an afterthought, so the
wire format IS the internal format. A private severity scheme would only have to
be mapped at both edges, and every mapping is somewhere for the meaning to
change.

Facilities are a small local set (`kernel`, `net`, `storage`, `browser`, `ui`,
`security`, `app`, `remote`) mapped onto syslog's numeric facilities on the way
out. `unolog.h` is the list; append to it, never renumber - a renumber
retroactively relabels every line already on a remote collector.

## 2. What is kept, and where

**In memory:** a ring of RECORDS - severity, facility, monotonic ms, wall
clock, and the text - not a flat char buffer. The viewer filters by level and
facility, and the syslog sender needs the fields; neither is possible against
concatenated bytes without re-parsing them.

**On disk:** `\LOGS\SYSTEM.LOG`, plain text, one line per record, oldest first.
Rotated at `UNOLOG_FILE_MAX` to `\LOGS\SYSTEM.1` (one generation - this is an OS
that may be living on a 32 GB stick, and the interesting log is nearly always
the current one).

**When it is written** is the part worth getting right. The FAT layer is
write-back cached and a crash loses whatever has not been flushed, so a log that
only writes periodically loses exactly the part you wanted:

- every `UNOLOG_FLUSH_MS` (default 10 s) if anything is pending;
- **immediately, and with a `uno_fat_sync()`, for ERR and worse** - the lines
  that explain a machine about to stop being able to write them;
- on a clean shutdown or restart.

## 3. How much is kept is the user's decision

Two independent thresholds, because "what do I record" and "what do I ship
across the network" are different questions:

| setting | default | meaning |
|---|---|---|
| `level` | `NOTICE` (5) | records at or below this severity are kept |
| `remote_level` | `WARNING` (4) | ... and at or below THIS are also sent to the syslog server |
| `remote` | unset | `host[:port]` of the syslog server; sending is off without it |
| `listen` | off | accept syslog from the network (see §5) |

Read at boot from `\LOGS\LOG.CFG`, changeable at runtime from the viewer, and
written back so the choice survives a reboot. The config parser is unolog's own:
`pc64_stress_cfg_*` lives in the debug harness and does not exist in production.

**`LOG.CFG` is read whole**, unlike `DEBUG.CFG`, whose 512-byte read has twice
now silently swallowed keys that sat past the cutoff.

## 4. Talking to rsyslog: source

RFC 5424 over UDP to port 514 (`remote=host[:port]`):

```
<PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID [SD] MSG
<134>1 2026-08-06T18:22:04Z zimablade unodos - net - link up on r8169
```

`PRI` is `facility * 8 + severity`. A machine with no RTC sends `-` for the
timestamp, which RFC 5424 permits, rather than a plausible lie - a collector
merging streams from several machines can work with a missing timestamp and
cannot work with a wrong one.

UDP, not TCP: syslog's own default, no connection to keep alive on a cooperative
single-threaded OS, and a collector that goes away cannot block the machine
logging to it. The cost is that datagrams can be lost, which is why the local
file is the record of authority and the network copy is a copy.

## 5. Talking to rsyslog: sink

`listen=1` binds UDP 514 and files what arrives as ordinary records, tagged with
the sender's address, so a UnoDOS box can be the collector for a lab of other
machines and read them all in one viewer.

**Off by default, and deliberately.** It is an unauthenticated UDP listener:
anything on the LAN can fill the ring and the disk. When on:

- inbound records are marked `remote` and never re-forwarded (a pair of boxes
  each listening and sending to the other would otherwise amplify forever);
- a sender that floods is rate-limited per source rather than the whole ring
  being handed to it;
- the parser accepts RFC 5424 and RFC 3164, because real senders emit both, and
  falls back to filing the raw text at NOTICE rather than dropping a message it
  cannot parse. A log is the wrong place to be fussy about format.

## 6. API

```c
void unolog(int sev, int fac, const char *fmt, ...);
#define ulog_err(fac, ...)   unolog(LOG_ERR, fac, __VA_ARGS__)
...
```

`unolog()` never blocks, never allocates, and is safe to call before the network
or the filesystem exist - records go to the ring and are written out when a sink
appears. It is NOT safe from an interrupt handler; nothing in pc64 logs from one
today, and making the ring interrupt-safe would cost every caller.

`unolog_tick()` is the pump: called once per frame from the shell, it does the
periodic flush and services the syslog socket. Everything expensive happens
there rather than in `unolog()`, so a caller pays only a formatted line.

## 7. The viewer

`LOGVIEW.UNO` - the log as a list, newest last, with the level threshold and the
remote settings editable in the app. It reads the ring for what is still in
memory and the file for what is older, so the view does not have a hole at the
boundary where the ring wrapped.
