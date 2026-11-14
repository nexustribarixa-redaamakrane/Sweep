# Sweep HTML/DOM Architecture in the KAPA Image (R7)

**Date:** 2026-11-14  
**Scope:** `sweep/sweep_html.c`, `sweep/sweep_html.h`, `sweep/sweep_text.c`, `sweep/kapa_sweep/kapa_dom_fixture.h`, `sweep/kapa_sweep/kapa_dom_js.h`, `sweep/kapa_sweep/kapa_dom_js.c`  
**Classification:** Implementation + QEMU-validated (R7).

## 1. What the image carries

`sweep/sweep_html.c`, `sweep/sweep_text.c` (parser + bounded DOM, unmodified in behavior) and the R7 KAPA binding `kapa_dom_js.c` are compiled directly into `kapa_sweep.owx`. No new parser, no new DOM, no hosted-libc dependency was introduced:

- Tokenizer — `SWEEP_HTML_TOKENIZER` / `SWEEP_HTML_TOKEN`, state machine, bounded per-token buffers (text 1024 B, name 48 B, attribute 16 × 256 B), raw-text element handling (`script`, `style`).
- DOM — `SWEEP_DOM` with inline node table (`SWEEP_DOM_NODES_MAX = 128` bounded flat array, no heap allocation, AVL sibling order), per-slot generation counter for JS wrapper validation.
- Binding — a narrow, application-side adapter `kapa_dom_js.c`. The Sweep DOM is the single source of truth; the binding adds no state to the DOM and no C pointers to JS.

## 2. Fixture and tags

Canned fixture `sweep/kapa_sweep/kapa_dom_fixture.h`:

```
<!doctype html>
<html>
<head><title>Sweep R7</title></head>
<body><div id="marker" data-id="r7-marker" class="probe">hello <span class="inner">sweep</span> dom</div></body>
</html>
```

Element inventory after parse (observed, ≈20 nodes): document/html/head/title/body/div/span plus text and doctype nodes. No external resources; fixture is a C string constant — it is compiled into the image and is **not** fetched.

## 3. JS-facing API surface (R7)

| JS | C call | Notes |
|---|---|---|
| `document.getElementById(id)` | `sweep_dom_find_by_id(dom, dom->document, id)` | string arg required |
| `element.nodeName` | direct read | parser's lowercased tag name (as stored) |
| `element.getAttribute(name)` | `sweep_dom_get_attr(node, name)` | null if absent; name non-empty required |
| `element.textContent` (get) | `sweep_dom_text_content(dom, id, buf, cap)` | concatenated descendant text |
| `element.setTextContent(v)` | `sweep_dom_set_text_content(dom, id, v)` | >1023 bytes refused (RangeError) |
| `element.children` (get) | iterate `node->children[]`, ELEMENT only | array of wrappers |

Not implemented (documented): `innerHTML` read/serialization path, `style`, `classList`, events, CSS/selectors, `querySelector`, sibling navigation; unsupported by intent — the R7 milestone proves the parser/DOM/never-re-implemented binding only.

## 4. Safe wrapper lifetime (binding invariant)

Every wrapper object carries two hidden number properties, non-writable / non-enumerable / non-configurable:

- `__sweep_node` — the DOM slot id (1..`SWEEP_DOM_NODES_MAX`)
- `__sweep_gen`  — the slot's generation at wrap time

Every access re-validates both against the live DOM (`sweep_dom_get` bounds-checks id; `sweep_dom_node_generation` must equal the stored gen). A stale wrapper — slot reclaimed and reused — throws TypeError instead of following a recycled node. No wrapper is cached by the engine; nothing in the binding caches pointers; no JS-provided byte sequence is interpreted as anything but a string.

Globals added on the global object: `document` (non-config, non-writable) and `__kapa_proto` (hidden, non-enumerable/config/writable). Nothing else is installed.

## 5. QEMU proof

`build-generated/sweep_kapa_dom_qemu_boot.log` (5,012 B):

```
[KDB][LOG] ... kapa-sweep: dom parsed, nodes=20
[KDB][LOG] ... kapa-sweep: dom js bound
[KDB][LOG] ... kapa-sweep: dom js ok result=r7-ok
[KDB][LOG] ... kapa-sweep: dom verified (text=r7-js-mutated)
[SWEEP-RUNTIME] selftest instance 1 finished (state=stopped fault=0x0 exit=90)
```

"r7-js-mutated" is produced by `el.setTextContent('r7-js-mutated')` from inside the JS program and then read back through the C DOM API — the mutation is not a JS string coincidentally matching a constant; it is the string the JS engine wrote via the real DOM API.

## 6. Limitations

Text writes >1023 bytes refused; node/children/attr caps are the parser's (`SWEEP_DOM_NODES_MAX=128`, `SWEEP_DOM_CHILDREN_MAX=32`, `SWEEP_HTML_ATTR_MAX=16`, `SWEEP_HTML_ATTR_VAL_MAX=256`). No `innerHTML`. No CSS/layout/render/navigation. The parser is not incremental. See `docs/SWEEP_R7_REPORT.md` for the full classified account.
