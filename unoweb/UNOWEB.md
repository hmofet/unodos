# unoweb — subsystem contract

**Owner:** the unoweb row in [`/AGENTS.md`](../AGENTS.md) section 1.
**Public surface:** [`unoweb.h`](unoweb.h). Everything else here is internal.
**Contract version:** 0.1 — `[EXPERIMENTAL]` until M2 lands on `master`.

unoweb is the web core: the DOM store and HTML parser today; CSS, layout and
paint from M3. It is the other half of the split in
[`docs/WEB-ENGINE-DESIGN.md`](../docs/WEB-ENGINE-DESIGN.md); `unojs` is the
JavaScript half and landed in M1.

## The rule that defines this subsystem

**unoweb knows nothing about JavaScript.** No `ujs_val` in this header, no
script evaluation, no engine dependency. It must parse and render a document
with no JS engine linked at all — the "NoScript build", which is exactly how
the test suite runs and how `test/Makefile` is written. Scripts reach the
outside through **one** callback (`uw_hooks::script`); the embedder decides
whether anything happens.

That is what makes `pc64/webjs.c` (M5) the single file in the system that sees
both engines. If you find yourself wanting to evaluate something here, it
belongs in the binding layer instead.

## Memory model

One **arena per document**: nodes are bump-allocated from a chain of chunks and
never individually freed, so a `uw_node*` stays valid for the life of the page
and the whole tree dies in O(1) with `uw_doc_free()`. Navigation is the unit of
reclamation.

The cost is that detached subtrees are held until the page goes away. That is
the deliberate trade: it removes the entire dangling-node class of bugs, and a
document that churns the DOM hard enough to matter meets `arena_max` and gets
flagged rather than growing without bound.

| Knob | Default | At the limit |
|---|---|---|
| `arena_max` | 16 MB | parse stops, `uw_doc_truncated()` returns 1 |
| `max_depth` | 256 | same — deep nesting is a classic DoS shape |
| pending input | 4 MB | same |

## Parsing

`uw_parse_begin` / `uw_parse_feed` / `uw_parse_end` is a **push** interface:
bytes arrive as they do from the network and the tree grows incrementally.

The tokenizer is deliberately **not a resumable state machine**. It scans a
pending buffer, consumes only complete tokens, and leaves a partial tail for
the next feed. A resumable machine needs every state duplicated with a "ran out
of input here" exit, and that duplication is where hand-written HTML parsers
accumulate their bugs. Buffering costs memory bounded by the cap above and buys
a scanner that is obviously correct at every call.

It also makes `document.write` fall out for free: `uw_parse_insert` splices at
the read cursor, which **is** the spec's insertion point, so written markup is
parsed before whatever followed the script. A test proves the resulting tree is
byte-identical for every input chunk size from 1 to 7.

### What the tree builder handles

Implied `html`/`head`/`body`; auto-closing of `p`, `li`, `dt`/`dd`, `option`,
`tr`, `td`/`th`, and headings; void elements; RCDATA (`title`, `textarea`),
RAWTEXT (`style`) and script data; comments, doctype, and bogus `<?...>`;
character references in text and attribute values; duplicate attributes with
first-wins; table foster-parenting so stray text does not vanish inside a
table; ASCII case-insensitive tag and attribute names.

### Known gaps

1. **No adoption agency algorithm.** Mis-nested formatting (`<b>a<i>b</b>c</i>`)
   degrades gracefully instead of reproducing the spec's tree exactly.
2. **The named entity set is the practical subset** (~90 entries), not the full
   2231-entry table. An unknown reference is left as written, which is also
   what a browser does with `&foo;`. The generated full table is M2b.
3. **No charset detection.** UTF-8 is assumed; `<meta charset>` is parsed into
   the DOM but not acted on. windows-1252 fallback is M2b.
4. **No `<template>`, no foreign content** (SVG/MathML namespaces).
5. **Fragment parsing ignores context-sensitive rules** — `uw_parse_fragment`
   parses into the context element directly, so a `<tr>` fragment in a `<div>`
   context is not corrected the way the spec prescribes.
6. **`uw_get_element_by_id` scans linearly.** The index is an array, not a
   hash; fine for document-sized id counts, revisit if profiling says so.

## Output conventions

`uw_serialize`, `uw_dump` and `uw_text_content` all follow one rule: append
what fits, keep counting what does not, and return the **full** length. A
caller can therefore size a buffer and retry, and truncation never looks like
success.

`uw_dump`'s indented format is what the golden tests compare, so its shape is
part of the contract:

```
#document
  html
    head
    body
      p class="x"
        "hello"
```

## Testing

`unoweb/test/` builds with plain gcc and **no unojs** — if it ever needs the JS
engine, the split has been broken.

```bash
cd unoweb/test && make test     # 37 checks
cd unoweb/test && make asan     # the merge gate
```

37 checks cover golden tree dumps, serialization round-trips, entity decoding,
error recovery, streaming at every chunk size, the script hook and
`document.write`, the DOM mutation/query API, and the limits. A
malformed-input case walks 30-odd broken fragments (bare `<`, unterminated
comments and attributes, `<table><td>`, a NUL byte) purely to prove none of
them crashes.

**Sanitizers are part of the gate.** The parser eats untrusted bytes and the
arena is hand-rolled, so use-after-free and overruns are real defects that
passing tests will not reveal on their own.

## Changelog

- **0.1** (2026-07-27) — first cut: DOM store, streaming HTML parser, and the
  browser bridge. `pc64_browser.c` now renders by walking a real DOM instead of
  scanning tag text inline, which is what gives it correct nesting through
  unclosed tags, character references, quoted attributes containing `>`, and
  RAWTEXT so a `<` inside `<style>` is not mistaken for markup.
  `[EXPERIMENTAL]`: the surface may still move before M2 lands on `master`.
