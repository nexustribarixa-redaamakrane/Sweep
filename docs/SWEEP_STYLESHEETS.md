# Sweep External Stylesheets & Selector Dependencies (Phase 5)

Status: **Implemented (host-test-only)**, verified 2026-11-10. Builds on the
existing CSS parser/matcher/cascade/layout/paint (`sweep/sweep_css.{h,c}`),
DOM (`sweep_html`), resource provider (`sweep_resource`), and document
lifecycle (`sweep_document`). No duplicate parser, DOM, or resource system.
No networking. Categories: **Implemented**, **Partially implemented**,
**Planned**, **Blocked**.

## 1. Stylesheet resource architecture and lifecycle

**Implemented.** Author stylesheets are discovered from the live DOM in document
order: inline `<style>` elements and `<link rel="stylesheet" href="...">`
elements. Each is tracked as a source with stable identity, kind (inline/link/
host-override), document source order, DOM element id (where applicable),
path/origin (`<style>`, href, or `<override>`), enabled flag, load status
(pending/loaded/error), error text, parsed rule count, and revision.

- `<link>`: `rel` must contain a `stylesheet` token (case-insensitive). `href`
  must be present and is loaded via the explicit local resource provider
  (`config.js_fetch` / `sweep_resource_fetch_file`, same root-confined path).
  MIME must be `text/css`; paths/MIME/size follow the provider's allowlist and
  bounds. Duplicate href references are detected, recorded as duplicates, and
  not re-fetched/re-applied. Missing resources, bad MIME, and fetch failures
  are recorded as source errors; the document remains usable (READY) and other
  sources still apply.
- `<style>`: element text is parsed with the existing parser.
- Malformed CSS: handled by the existing parser's recovery (partial/valid rules
  may still apply); parse/tokenizer overflow is reflected in sheet state.
- Host override: `sweep_document_set_stylesheet` remains supported as a
  one-shot override for the next style pass (then discovery resumes), preserving
  the existing API contract.
- Ownership: parsed stylesheets and source metadata are value members owned by
  the document (no dangling provider-buffer pointers). Bounded by
  `SWEEP_DOCUMENT_MAX_SHEET_SOURCES` (8) and existing CSS limits
  (`SWEEP_CSS_MAX_RULES`, etc.); truncation is reported via sheet flags/stats.

## 2. Source ordering and cascade

**Implemented.** Loaded/enabled, non-duplicate sources are appended in document
order into a combined author stylesheet (`sweep_css_stylesheet_append`) which is
the cascade input together with existing UA defaults and inline styles. This
preserves existing cascade semantics: specificity, `!important`, inheritance,
inline style precedence, and source order (later rule wins at equal specificity/
origin layer). Tested with inline+external ordering, multiple sheets, duplicates,
conflicts, specificity, `!important`, inheritance, and inline precedence.

## 3. Selector dependency representation

**Implemented (bounded index).** `SWEEP_CSS_DEP_INDEX` is built from the combined
author stylesheet and records: presence of universal/type/class/id/attribute
selectors; presence of structural combinators (descendant, child); bounded sets
of referenced type/class/id/attr names and attr=value pairs (with conservative
overflow handling); per-rule `needs_tree` flag (rule has universal or structural
dependencies and therefore cannot be filtered by subject features alone);
selector/rule counts. Supported dependency categories: element/type names, class
names, IDs, attribute names, attribute name=value pairs, structural
(descendant/child), universal, selector lists. Index is rebuilt with the author
sheet on style passes.

## 4. Mutation-to-selector invalidation and incremental restyling (Phase 5 + Phase 6)

**Implemented (conservative, measurable).** DOM mutations record best-effort
details (kind, attribute name, class/id flags). The document computes a bounded,
generation-safe `SWEEP_RESTYLE_SET` (node id + generation, scope, reason, direct,
overflow) and consults the dependency index:

- **Provably unaffected attribute/class/id mutations** (no author rule affected,
  not stylesheet/structural): skip the **style pass** entirely; layout and paint
  still run as required. Counted as `style_recompute_skipped`.
- **Affected-element/subtree restyling (Phase 6, safe subset):** non-structural,
  non-universal attribute/class/id/inline-style mutations (no descendant/child
  selector dependencies) can change selector matching only for the affected
  element, but inherited properties may propagate to descendants; scope is
  **subtree** and only that subtree is recomputed in place
  (`sweep_css_recompute_subtree_indexed`) with the current parent style,
  preserving computed styles outside the subtree. Equivalent to full recompute
  for the subtree (reference-tested). Diagnostics: `restyle_candidates`,
  `restyle_applied`, `restyle_skipped`, `restyle_subtree_escalations`,
  `restyle_full_fallbacks`.
- **Conservative escalation:** structural mutations (insert/remove/reorder),
  stylesheet changes, universal/structural/descendant/child dependencies, stale
  generations, overflow, or uncertain cases escalate to **full-document restyle**
  fallback. Correctness is never traded for speed.

**Incremental selector candidate filtering (Phase 5, implemented):** rules that
cannot match a node based on its own features and the index are not selector-
tested for that node (structural/universal rules never skipped). Final computed
styles are identical to a full-reference recomputation — verified in tests.
Stats: `selector_tests`, `rules_skipped`, `nodes_recomputed`.

**Incremental stylesheet contributions (Phase 6, implemented safe subset):**
unchanged inline/link sources reuse parsed contributions (content-hash/text
identity, stable id/revision; `stylesheet_sources_reused`) while changed sources
are reparsed (`stylesheet_sources_reparsed`, revision bumped). Combined author
sheet and dependency index are rebuilt each style pass from current ordered
sources (order/duplicates/errors/obsolete removal preserved), equivalent to full
rebuild.

**Not claimed:** incremental layout or incremental paint. Layout and paint are
full recomputations when their stages run. Incremental selector-candidate
filtering, style invalidation/pass skipping, affected-element restyling, and
stylesheet contribution rebuilding are distinct capabilities.

## 5. Security and resource limits

**Implemented (reuses provider).** CSS loading is provider-local only: no
network, no JS-chosen provider/root/filesystem escape. Path policy: relative
paths only; absolute paths, drive letters, backslashes, control chars, and `..`
segments rejected. MIME allowlist: stylesheet links require `text/css`; provider
rejects binary/unsupported content per its policy. Size bounded by provider/
caller caps (`SWEEP_RESOURCE_MAX_BYTES`). Stylesheet/source counts bounded by
`SWEEP_DOCUMENT_MAX_SHEET_SOURCES`; rule/token/selector/declaration limits
bounded by existing CSS engine caps with deterministic truncation/error
reporting. No unbounded allocation or parsing. Tested for traversal/absolute/
backslash rejection, missing/oversized/unsupported/binary resources, malformed
CSS recovery, duplicates, and repeated loads.

## 6. SuperUnicode compatibility

**Implemented (preserved).** No SUTF/SUTR spec files modified. Native SUCS
`0xD800–0xDFFF` remain valid ordinary PUA values (no surrogate semantics) in
NATIVE_SU decode/encode/transport paths as fixed in Phase 4. WEB_COMPAT remains
distinct. Source text bytes and rendered glyph/codepoint handling remain the
separate concerns established earlier (no silent native↔Unicode round-trip).
CSS identifier/string handling follows the existing bounded tokenizer/parser
behavior and byte-level matching over stored names/values; no new
Unicode-normalization or SuperUnicode-specific CSS tokenization is claimed.

## 7. Tests and build outcomes

**Implemented.** Deterministic host tests: `sweep/tests/sweep_resource_test.c`
(15), `sweep/tests/sweep_css_test.c` (208), `sweep/tests/sweep_document_test.c`
(190). Phase 6 adds affected-subtree restyle tracking/application with
reference-equivalence and preservation checks, stylesheet source reuse/reparse/
revision tracking, conservative structural escalation, and layout-diagnostics
fallback coverage (`[p6]`). Full Sweep suite **758 passed, 0 failed** (engine 24,
services 18, text 37, browser 39, HTML 40, CSS 208, JS 187, document 190,
resource 15). Sweep kernel `make -j 2` builds `openwinkrnl.owx`. OpenWindows
`make hosttest` **1418 passed, 0 failed**. C99, `-Wall -Wextra -Werror` for
Sweep code (relaxed flags only for vendored Duktape).

## 8. Remaining blockers / not implemented

- **Partially implemented:** affected-element restyle tracking and incremental
  subtree restyling are implemented for provably safe non-structural/
  non-universal attr/class/id/inline-style cases (generation-safe, with
  inheritance-preserving subtree recompute and reference equivalence). Structural
  selector dependencies, DOM insert/remove/reorder, stylesheet mutations, stale/
  overflow/uncertain cases, and element-only inheritance-diff optimizations fall
  back conservatively to full-document restyle. Incremental selector-candidate
  filtering remains active.
- **Partially implemented:** per-source stylesheet revision/content identity and
  unchanged-source parse reuse are implemented; the combined author sheet and
  dependency index are rebuilt each style pass for correctness (equivalent to
  full rebuild, obsolete rules removed), not incrementally spliced.
- **Not implemented:** incremental layout, incremental paint, HTTP/HTTPS/network
  fetching, `@import`/`@media`/cascade layers, pseudo-classes/elements, full CSS
  syntax, JS-driven stylesheet manipulation APIs.
- **Partially advanced (R4):** a freestanding Duktape object build exists with
  zero undefined hosted symbols (host-harness smoke-tested), but kernel/KAPA
  execution and kernel-backed document integration remain unimplemented.
- **Blocked/unchanged:** physical renderer/compositor/rasterizer, KDB execution
  backend, SUST byte serialization and >U+10FFFF bridge encodings beyond existing helpers.
