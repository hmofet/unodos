# csslib - the vendored MIT CSS stack (NetSurf's libraries) + port layer

The real-CSS half of the second-engine programme
(`docs/BROWSER-ENGINE2-PLAN.md` phase 2, series CS0-CS3): libcss's parser +
selection engine upgrading unoweb's cascade in place. unoweb keeps DOM,
HTML and layout; this directory is the engine it consults for computed
styles.

## Provenance and licence audit

Vendored 2026-08-05 from the NetSurf project's GitHub mirrors, `.c`/`.h`
sources + `COPYING` only (tests, docs, buildsystem and the code GENERATORS
are excluded - every generated output is committed upstream, so an update
is copy-over, never regenerate):

| dir | upstream | version | commit |
|---|---|---|---|
| `css/` | github.com/netsurf-browser/libcss | 0.9.2 (release/0.9.2) | `703f1247b2fabd5dc685ba80bc26ccfa263c79ae` |
| `parserutils/` | github.com/netsurf-browser/libparserutils | 0.2.5 | `6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca` |
| `wapcaplet/` | github.com/netsurf-browser/libwapcaplet | 0.4.3 | `c7c128d3eb3223b216c974471f82e9337fbcf4ba` |

**Licence: MIT, audited per-file per the standing no-GPL rule** (user
ruling 2026-08-05; the NetSurf *browser core* is GPLv2 and is deliberately
NOT here). Audit result over all 267 vendored sources: 265 carry an
explicit in-file MIT notice; the 2 libwapcaplet files carry a
copyright-only header and are covered by that project's MIT `COPYING`
(kept alongside, one per dir); 0 files mention the GPL. Re-run the audit
on any version bump. When this ships in a build, surface the three
`COPYING` texts in the About dialog + `DOCS\LICENSES.MD` roster (the
unomedia precedent).

Vendored files are UNMODIFIED. Port decisions live in build flags and the
compat layer (CS1), exactly like `pc64/quickjs/` - whose VENDOR.md traps
(limit-macro `#if` sniffs, implicit declarations under `-w`, host-libc
symbol interposition, personality gates) apply to this port verbatim.

## Port notes (filled in by CS1)

- `-DWITHOUT_ICONV_FILTER` - the ONE non-C99 dependency
  (`parserutils/src/input/filter.c` wants iconv) is compile-gated off;
  the built-in UTF-8/UTF-16/fixed-charset codecs remain.
- Include paths: `-Icsslib/css/include -Icsslib/parserutils/include
  -Icsslib/wapcaplet/include` public; each lib's `src/` on its own
  compile's path for the quoted-relative internal includes.
