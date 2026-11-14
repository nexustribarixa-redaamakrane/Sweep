# Sweep Document Lifecycle (Phase 4/5)

Status: **Implemented (host-test-only)**, updated 2026-11-10. HTML, DOM, CSS,
JavaScript, layout and paint operate as one coherent document engine behind an
explicit lifecycle, with Phase 5 external stylesheet loading and selector-
dependency-aware style processing. Categories: **Implemented**,
**Partially implemented**, **Planned**, **Blocked**.

## 1. Lifecycle architecture

`SWEEP_DOCUMENT` (`sweep/sweep_document.{h,c}`) owns one `SWEEP_DOM`, the author
stylesheet text, computed styles (`SWEEP_COMPUTED_STYLE` per node), the
`SWEEP_LAYOUT`, and a bounded paint-command buffer (256 commands), plus an
optional JS runtime/context. It is allocated by `sweep_document_create()` (host
`calloc`) and freed by `sweep_document_destroy()`.

Explicit states: `EMPTY → PARSING → DOM_READY → (STYLE_DIRTY → LAYOUT_DIRTY →
PAINT_DIRTY) → READY`, plus terminal `FAILED` and `DESTROYED`. Dirty stages are
transient: `sweep_document_update()` sets each stage as it runs it.
`sweep_document_state()` reports the *effective* state — after a DOM mutation it
returns the pending dirty stage (`STYLE_DIRTY`/`LAYOUT_DIRTY`/`PAINT_DIRTY`)
even though the stored state is `READY`, so callers can see that rendering is
stale. Invalid transitions are rejected: `update` from `EMPTY`/`PARSING`/
`FAILED`/`DESTROYED` fails; `load` is `EMPTY`-only; viewport/stylesheet setters
fail from `EMPTY`/`FAILED`/`DESTROYED`. `FAILED` is reached only on pipeline
failure (invalid viewport, layout failure) — JS exceptions never fail a
document (they are reported with a bounded diagnostic and the document stays
usable).

`sweep_document_load()` (EMPTY-only) runs: HTML parse → DOM-ready → JS runtime
+ context init (if enabled) → classic `<script>` pass (errors recorded, not
fatal) → full pipeline → `READY`.

## 2. Invalidation

**Implemented:** every DOM mutation bumps `dom->revision` and sets dirty flags;
`processed_revision` records the revision consumed by the last pipeline pass.
`run_pipeline()` snapshots pending work *before* any pass clears its flag (a
previous per-pass clearing bug meant viewport resizes rebuilt layout but repainted
with old commands — now fixed and regression-tested: CLEAR covers the new
viewport).

Narrowing (correctness first):
- Text-content mutations set layout+paint only (computed styles read
  tags/attrs/tree, not text) → style pass skipped (asserted in tests via pass
  counters). Exception: text inside `<style>` feeds the stylesheet → full
  invalidation.
- Attribute/structural mutations (insert/remove, any attribute incl. class, id,
  style) set style+layout+paint — conservative, because descendant and
  attribute selectors make precise dependency tracking unsafe without a full
  dependency graph. Documented as such; nothing is claimed about incremental
  style/layout.

The paint buffer is memset before every paint pass, so old commands can never
remain visible after a mutation. Paint-capacity overflow still reaches `READY`
with the emitted prefix and `paint_truncated` set (reported, not fatal). Layout
truncation is reported the same way.

## 3. Style / layout / paint recomputation

Style: the document collects author sources in DOM order (`<style>` elements and
`<link rel="stylesheet">` loaded via the local resource provider, plus host
override) into a combined stylesheet and a selector-dependency index, then
computes styles via the indexed/candidate-filtered path
(`sweep_css_compute_all_indexed`) or falls back to full evaluation as needed.
Source order, cascade semantics, and UA defaults/inline precedence are preserved.
The style pass may be skipped when the dependency index proves a mutation cannot
affect author-rule matching (incremental invalidation); otherwise it runs as a
full, safe recompute over nodes (candidate filtering may reduce selector tests).
Layout: bounded full rebuild (`sweep_layout_build`) with the stored negotiated
viewport (8192px cap via `SweepViewportNegotiate`, zero rejected); no incremental
layout is claimed. Paint: `sweep_paint_from_layout` into the document buffer
(CLEAR first, viewport-clipped RECT/TEXT in tree order), reset each pass; each
paint regeneration increments `paint_revision` and marks any attached rendered
frame stale. Phase 8 adds controlled external/caller-owned render+presentation
integration (`sweep_document_attach_renderer`, `sweep_document_render`,
`sweep_document_present`, current/stale tracking by paint revision); see
`docs/SWEEP_RENDERING.md`. See `docs/SWEEP_STYLESHEETS.md` for stylesheet/resource/dependency details.

JS integration: `sweep_document_evaluate()` runs Duktape and returns; DOM
mutations performed by scripts only dirty state — rendering never happens inside
a Duktape callback. `sweep_document_update()` is the single controlled boundary.
Multiple mutations between updates batch into one pass (tested).

## 4. Node lifecycle (reclamation)

**Implemented** with slot generations (`SWEEP_DOM_NODE.generation`, starts 1,
increments on every reclaim/reuse):
- `textContent`/`innerHTML` replacement reclaims the discarded subtree
  (bounded traversal ≤128 nodes, parent-link verified, document node excluded),
  so repeated `innerHTML` no longer exhausts the 128-node cap (200× stress
  test passes).
- JS wrappers capture `(id, generation)`; after a slot is reused, stale
  wrappers are rejected with `TypeError` (frozen document access to reclaimed
  nodes is unsafe by design, not dangling). Event listeners capture generation
  and are dropped at dispatch if the slot changed hands (stale-listener test).
- `removeChild` keeps nodes detached and **re-appendable** (Phase 3 contract,
  preserved and tested). Documented interim limit: repeatedly removing *new*
  nodes via `removeChild` still consumes slots until the cap; reclaiming those
  would break the re-append contract because wrappers cannot be refcounted
  without a Duktape GC hook. This is precisely the remaining unsafe-to-free
  case, so it stays un-reclaimed (bounded RangeError, never a crash).

## 5. Resource provider

**Implemented:** `sweep/sweep_resource.{h,c}` — `SWEEP_RESOURCE_ROOT` (trusted
config, host-registered) + `sweep_resource_fetch_file` (`SWEEP_JS_RESOURCE_FETCH`).
Behavior: relative paths only; absolute paths, drive letters, backslashes,
control chars and any `..` segment rejected (no traversal, no escape);
root + relative join only; per-resource cap (65536 default); MIME from an
extension allowlist (`.js/.mjs` JavaScript, `.css`, `.html`, else ASCII-sniffed
`text/plain` — binary rejected). JavaScript cannot choose the provider, root, or
paths outside it; no network. Factors tested: valid JS, missing file, over-cap,
binary, traversal attempts, end-to-end external `<script src>` through a
document, and non-JavaScript MIME never executing.

## 6. SuperUnicode transport

**Implemented** with the SUTR-correct rule (see §7): DOM bytes stay bytes; the
document exports codepoints with `sweep_js_cesu8_decode` (CESU-8-aware,
mode-tagged). NATIVE_SU decodes every value in 0xD800–0xDFFF as itself —
never paired, never rejected — so the whole block round-trips; WEB_COMPAT
pairs at the web boundary (ECMAScript semantics). Layout measure and paint
glyphs thread the document's text mode, so a native 0xD800 byte triple reaches
a `glyph = 0xD800` command (tested). UTF-8 decode/encode in `sweep_text`
follows the same contract (see §7): NATIVE_SU accepts and emits the block.

What is still compatibility-only (honest): byte buffers in the DOM/layout are
UTF-8/SUTF-8 bridge bytes, not SUST-serialized native bytes; `> U+10FFFF`
encodings (SUTF 5/6-byte forms) are beyond this helper's 1-4-byte bridge and
would need the SUTF library APIs. Nothing in this layer claims to be SUST
serialization.

## 7. Surrogate-range audit (spec conformance)

SuperUnicode has no surrogates (SUTR-0 §4.1; SUTR-1 §1.2, §4.2):
`0xD800–0xDFFF` are ordinary valid PUA codepoints, never reserved/paired/
rejected. Sweep's `sweep_utf8_decode` previously rejected those sequences in
**both** modes — non-conforming — while `sucs_is_valid` (values) was correct.
Fixed (verified by reading `sucs_is_valid`, `sutf8_decode_char` gates, and the
SUTR validity tables):
- `sweep_utf8_decode`: NATIVE_SU accepts `ED A0 80..ED BF BF` as values;
  WEB_COMPAT keeps strict web UTF-8 rejection (web-boundary semantics).
- `sweep_utf8_encode`: NATIVE_SU emits the block as three-byte sequences;
  WEB_COMPAT sinks to U+FFFD at the web encoding sink.
- `sweep_js_cesu8_decode` (new transport helper): same contract.
- Deliberately kept: trap/sentinel rejection (`sucs_is_valid` false for
  `0x7FFFFFF0–0x7FFFFFFF`); overlong/malformed rejection; WEB_COMPAT
  UTF-16/CESU pair combining (web semantics, not SuperUnicode).
- Tests: full 0xD800–0xDFFF encode/decode/round-trip loop (2,048 values),
  block edges, adjacent-pair non-combining, trap/sentinel sinking, and
  PUA/astral neighbours, in `sweep/tests/sweep_text_test.c`.

## 8. Phase 6 affected-element restyle, stylesheet contributions, layout status

**Affected-element restyle tracking (implemented, bounded):** each style pass
computes a bounded, generation-safe `SWEEP_RESTYLE_SET` (node id + generation,
scope, reason, direct flag; capped, overflow => conservative document scope).
Mutation-to-element analysis uses DOM mutation details and the selector
dependency index: stylesheet/structure/unknown => document; attribute/class/id/
inline-style changes escalate to document when structural/universal/descendant/
child dependencies exist (ancestor changes can affect descendants under supported
combinators), otherwise escalate to **subtree** scope rooted at the affected
element (selector matching can change only for that element, but inherited
properties may propagate to descendants). Stale/missing targets fall back to
full document restyle. `removeChild`-detached slots remain unreclaimed by
contract; reclaimed-subtree generations reject stale wrappers as before.

**Incremental style application (implemented, safe subset):** when the restyle
set identifies a single generation-valid subtree root, `do_style_pass` recomputes
only that subtree in place via `sweep_css_recompute_subtree_indexed` using the
current parent computed style, preserving computed styles of nodes outside the
subtree. This is equivalent to full recomputation for the subtree (initial +
inheritance from parent + UA defaults + cascade with candidate filtering, in
preorder). Diagnostics report `restyle_candidates/applied/skipped`,
`restyle_subtree_escalations`, and `restyle_full_fallbacks`. All other cases
(overflow, document/structural/stylesheet/uncertain scope, stale generation)
fall back to full-document recompute. No stale computed styles are left
silently: unchanged regions are preserved only when not in the recomputed
subtree.

**Incremental stylesheet contributions (implemented, safe subset):** inline and
external author sources are matched across passes by identity (inline element id,
link path). Inline text and fetched link content are content-hashed; unchanged
sources reuse their previously parsed `SWEEP_CSS_STYLESHEET` and keep stable
source id/revision (`stylesheet_sources_reused`), while changed/new/error sources
are reparsed and bump revision (`stylesheet_sources_reparsed`). The combined
author stylesheet and dependency index are rebuilt each style pass from the
current ordered source set (preserving document order, duplicates, enabled/error
behavior, and removing obsolete contributions), so results remain equivalent to a
full rebuild. This reduces unnecessary reparsing of unrelated unchanged sources;
it does not claim partial in-place rule splicing when unsafe.

**Incremental layout investigation (assessment, no incremental implementation):**
layout is a monolithic full-tree build (`sweep_layout_build`) producing boxes in
build order without a stable persisted node-to-box mapping/dirty-region graph.
Because width/height/wrapping/margin/padding/border changes can propagate to
ancestors, descendants, and following siblings (flow, line breaks, content
height), no provably safe bounded incremental layout case was implemented in the
current representation. Layout passes record deterministic diagnostics
(`layout_regions_considered`, `layout_full_fallbacks`) and always use full-layout
fallback/recompute. Paint remains a full command regeneration when paint runs.

## 9. Tests & build

`make sweep-tests`: **847 passed, 0 failed** (engine 24, services 18,
text 37, browser 39, HTML 40, CSS 208, JS 187, document 217, resource 15, render 110) with Phase 9 [p9] multi-frame/surface-replacement/presentation-failure-recovery lifecycle hardening and additional SUF pixel-reference/surface tests.
The document suite includes Phase 5 stylesheet cases, Phase 6 affected-subtree
restyle/reference-equivalence, stylesheet source reuse/reparse/revision, and
conservative escalation/layout-diagnostics cases (`[p6]`), plus Phase 8
document-render lifecycle tests (`[p8]`: attach/detach, render/present,
paint-revision invalidation, stale-frame rejection, viewport rerender). Render
suite covers raster/safety/text, SUF-backed boot-bitmap glyph provider (real
`assets/openwindows-terminal.suf`), presentation simulation boundary, and render
revision tracking. Sweep kernel `make -j 2`: exit 0 (Phase 6–8 host modules not
linked into kernel image; `openwinkrnl.owx` artifact timestamp may be unchanged).
OpenWindows `make hosttest`: 1418/0 (unchanged). The JS/Duktape/document/render
host-test layer stays host-test-only (hosted libc; freestanding kernel blocked).
Phase 5 details: `docs/SWEEP_STYLESHEETS.md`; rendering details:
`docs/SWEEP_RENDERING.md`.

## 10. Remaining limitations

- Layout and paint are conservative full recomputations (no incremental layout/
  paint). Style invalidation/restyling is incremental only for provably safe
  subtree-scoped cases (non-structural/non-universal attr/class/id/inline-style
  changes with generation-valid targets); structural, stylesheet, insertion/
  removal/reordering, uncertain, and overflow cases escalate to full-document
  restyle. Selector-candidate filtering still reduces selector tests.
- `removeChild`-detached nodes are not reclaimed (re-append contract; gap
  documented in §4).
- No physical renderer/compositor/rasterizer: output is validated paint
  commands. No HTTP/HTTPS or networking; external scripts remain
  provider-local. No SUST byte serialization or >U+10FFFF bridge encodings.
- Kernel-image integration of any of this (JS/Duktape, document host alloc)
  remains blocked by hosted-libc requirements.
