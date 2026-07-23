# AGENTS.md: working agreement for every UnoDOS agent

Read this before you start. It applies equally to **every** agent, whatever your
task, **including the unoautomate agent**. No agent owns another agent's lane.
Ownership is per subsystem and listed in the registry below, and nobody may widen
their lane to absorb a shared subsystem.

## TL;DR (the six rules)

1. **Stay in your lane.** You own one subsystem. Everything else you CONSUME as a
   neutral API.
2. **Branch short, integrate often.** New branch off `master` per slice; rebase
   onto `master` at the start of every session; land small.
3. **Claim before you build on a shared surface.** One line in the requests file
   first.
4. **Contracts are the source of truth.** Re-read the relevant contract after
   every pull.
5. **One commit = one lane.** Keep `master` always green.
6. **`master` is the only integration point.** Feature branches never merge each
   other.

## 1. Ownership registry (the anti-creep mechanism)

Each subsystem has one owner and a contract doc. To change a subsystem you do not
own, file a request to its owner (see §4); do not edit it directly. To start work
on a NEW subsystem, add a row here first, in the same commit. **No row may be
widened to swallow a shared subsystem.** The cautionary tale is unoautomate, which
once parked networking and on-device storage under its own contract and had to
hand them back (see `pc64/HARNESS-POLICY.md` changelog, 2026-07-22 "OWNERSHIP
re-home"). Ownership is of the CODE; the whole OS is a shared goal.

| Subsystem | Contract / spec | Root files |
|---|---|---|
| unoautomate (harness + URC remote channel) | `HARNESS-POLICY.md`, `REMOTE.md`, `unoauto.h` | `unoauto*`, `unoauto_remote*`, `upy_port/mod_unoauto.c` |
| unonet (ARP/IP/TCP/UDP/DHCP/DNS/TLS/sockets/discovery) | `NETSTACK.md`, `NETWORK.md` | `net.*`, `tls.*`, `tls_ca.*`, `netsock.h`, `netdisc.*` |
| NIC drivers (below the `uno_nic_t` seam) | per driver | `e1000*`, `e1000e*`, `igb*`, `r8169*`, `ax88179*`, `rtl8152*`, `iwlwifi*`, `rtwifi*`, `mrvlwifi*` |
| unostorage (GPT/ESP/FAT authoring) | `STORAGE.md` | `unostorage.*`, `uno_fat_mkfs` in `fat.c` |
| unofs (filesystems + block devices) | `STORAGE.md` | `fat.*`, `pc64_fs.*`, `blkdev.*`, `ahci/nvme/sdhci` |
| unosecure (identity / RBAC / audit) | `UNOSECURE-SPEC.md`, `UNOSECURE.md` | `unosecure.*`, `pc64_accounts.*` |
| unoscript (scripting surface) | `UNOSCRIPT.md` | `unoscript.*`, `upy_port/mod_unoscript.c` |
| unodevices (PCI device tree + driver registry) | branch `unodevices` | `uno_devmgr.*` |
| installer (install-to-disk + boot entries) | `INSTALL.md` | `installer.c` |
| unomedia (image / audio decoders) | `IMAGES.md`, `AUDIO.md` | `unomedia/` |
| pc64-python / PYRT | SDK docs | `upy_port/`, `apps/pyrt.c`, `pyhost.h` |
| toolkits (uno3d / unoui / sound) | their headers | `uno3d*`, `pc64_uui*`, `snd_*`, `hdaudio/ac97` |
| per-port ports | `AUDIT-<port>.md` | `amiga/`, `c64/`, `nes/`, `snes/`, ... |

Whoever is actively working a subsystem this session **claims it in the requests
file (§4)** so two agents never grab the same one.

## 2. Three kinds of file: own / consume / shared choke-point

**OWN** (your subsystem's files): edit freely.

**CONSUME** (every other subsystem's public API): use it, do NOT restructure it.
Need a change (a new call, option, or semantic)? File a request to its owner (§4)
and use the nearest existing primitive as a stopgap.

**SHARED CHOKE-POINTS** (additive-only): a handful of central files that *every*
feature has to touch. Structural edits here are near-guaranteed merge conflicts;
this is where essentially all of the 2026-07 merge pain came from. The rule:
**APPEND through the registration seam, never rename / reorder / restructure.**

| Choke-point | How to add without conflict |
|---|---|
| `build.sh` compile-file list | append your module name; do not reorder the list |
| `unoauto_remote.c` CMD dispatch | unoautomate owns it: new verb is a request, or use the weak-symbol pass-through (e.g. `r8169_dbg_cmd`) |
| `pc64_modload.c` kExports (`KX()` arrays) | append your `KX()` line in the right section |
| `upy_port/mod_uno.c` module table | append your entry |
| `uefi_main.c` boot wiring, `pc64_uui.c` frame loop | append ONE boot/tick call at the end of the relevant block |
| `REMOTE.md` verb table, `*-REQUESTS.md` | append only |

Prefer **self-registering seams** over central switches: weak-symbol stubs (link
green before the provider lands, auto-upgrade when it does), `KX()` / module
tables, and the `UNO_DRIVER` linker set. If your feature needs a brand-new central
switch, build it as a registration table so the next agent appends instead of
conflicts.

## 3. Branch and merge discipline

One worktree + branch per feature slice, off `origin/master`:

```
git worktree add ../unodos-<slice> -b <slice> origin/master
```

- **Rebase onto `origin/master` at the START of every session, and daily.** Small
  frequent rebases are trivial; the two-week rebase is the pain you just paid.
- **`master` is the ONLY integration point.** Never merge one feature branch into
  another.
- **Land small.** A subsystem is many short branches over time, not one long-lived
  branch accumulating phases.
- **Merge gate (definition of done)** before landing:
  1. rebased on latest `origin/master`
  2. builds both `UNO_DEBUG=0` and `UNO_DEBUG=1`
  3. the relevant QEMU / host gate is green
  4. no central-dispatch edit that bypasses a registration seam (§2)
- Land as **rebase-then-fast-forward** (or squash) for a clean linear `master`.
  **Delete the branch AND its worktree the same day it lands.** A contained branch
  left alive becomes archaeology.

Do NOT add a `develop`/`staging` layer between features and `master`. For a handful
of agents it is pure overhead; `master` is the integration branch.

### Working across sessions, and recovering from a crashed agent

The durable state of in-progress work is the **branch and its worktree**, never the
agent session. Work survives context limits and agent crashes as long as it is
committed to the branch (and pushed, for off-machine safety). The three save
operations have different jobs and cadences:

- **Commit** constantly, at every coherent step. Commits are your save points.
- **Push the branch** (`git push -u origin <slice>`) at the end of each session,
  before context fills, and at least daily on multi-day work. Pushing the branch is
  off-machine backup and cross-session handoff. It never touches `master` and never
  conflicts with another agent, so do it freely.
- **Rebase onto `origin/master`** at the start of every session.
- **Land to `master`** only when a slice passes the merge gate above.

To resume in a fresh session (a planned handoff, or after a crash):

1. Go to the existing worktree, or recreate it: `git worktree add ../unodos-<slice> origin/<slice>`.
2. Run `git status` (recover any uncommitted edits) and
   `git log --oneline origin/master..HEAD` (see what is already done).
3. Commit anything dangling, rebase onto `origin/master`, then continue.

An agent crash does NOT destroy the worktree: committed and uncommitted work is
still on disk, so a fresh agent pointed at the same worktree just continues. Only
uncommitted edits are ever at risk, and only from disk or machine loss, which
frequent commits plus a pushed branch cover. A continuation prompt saves the next
agent from re-deriving intent, but it is not load-bearing: the commit history plus
this file are enough to pick the work back up.

## 4. Claims and requests

- `<SUBSYSTEM>-REQUESTS.md` (today: `pc64/UNOAUTOMATE-REQUESTS.md`) is the async
  channel between agents. Append dated entries; **never edit an entry you did not
  write.**
- **Before building anything on a shared surface, file a one-line CLAIM there**
  ("taking install-to-disk on unostorage"). This is the fix for the install-verb
  duplication, where two agents built the same feature because neither claimed it.
- Need a capability from a subsystem you consume? File a request to its owner and
  use the nearest primitive meanwhile.

## 5. Commit hygiene

- **One commit = one lane.** A driver fix and the shared-file line that registers
  it are TWO commits (your file first, the seam second).
- **Prefix a shared-seam commit** so it is easy to spot and replay (e.g. `seam:`,
  `harness:`, `build:`).
- Commit small and often; push after each landed milestone.

## 6. Contracts move; stay current

Every subsystem's contract (its header plus spec doc) is the source of truth, and
it changes. Owners tag surface `[STABLE]` or `[EXPERIMENTAL]` and bump a version
marker on any break; consumers re-read the contract and its changelog after every
pull. The compiler catches C signature breaks, not semantic or wire-protocol
changes, so read the changelog and do not cache assumptions across a rebase.

## 7. unoautomate is a peer, not the landlord

The old `pc64/HARNESS-POLICY.md` read as "unoautomate's contract, enforced on
everyone." That framing is superseded by THIS file, which binds every agent
symmetrically. unoautomate owns exactly its registry row (the harness, the URC
channel, and their contract docs) and consumes everything else as a neutral API,
the same as you. `HARNESS-POLICY.md` remains only as unoautomate's **subsystem
contract** (its API and changelog), not as system-wide policy.
