# Fix plan — blockquote spacing & ordered-list numbering

Status: **proposed** (not started)
Reporter: user report + screenshot (2026-08-27; screenshot not viewable in review session — diagnosis is code-traced; see Open questions)

## 1. Symptoms (as reported)

| # | Symptom | Where |
|---|---------|-------|
| A | Quoted text (blockquotes) lacks proper padding/spacing; it crowds or touches adjacent text | preview, both platforms |
| B | Ordered-list numbers written in the markdown are lost in the render; numbering resets to 1 | preview, both platforms |

Both frontends (Win32 `cpp/render.cpp`, macOS `frontends/macos/mac_render.mm`) paint the
single display list produced by `core/layout.cpp` from a `Document` built by
`core/markdown.cpp`. The bugs live entirely in the shared core — one fix covers
Windows, macOS, and the CLI alike.

## 2. Root-cause analysis

### Bug B — ordered-list numbers dropped (confirmed, deterministic)

1. **Parser discards the author's number** — `core/markdown.cpp:338-364`:
   `ParseMarkdown` detects `N.` but only records `b.ordered = true` and the content
   offset (`olStart`); the value `N` is never stored on the `Block`
   (`core/markdown.h:29-47` has no start-number field).
2. **Layout numbers sequentially from 1** — `core/layout.cpp:311,316,376-380`:
   a single global `olCounter`, reset to 0 by *any* non-ordered block, incremented
   per ordered item. Consequences:
   - a list written `5. a / 6. b` renders `1. a / 2. b` (number lost);
   - an interrupted list (`5. a / 6. b`, paragraph, `9. c / 10. d`) renders the
     second part as `1. c / 2. d` (number reset — the reported "reset");
   - **latent extra bug:** a nested ordered list continues the *outer* counter
     instead of numbering itself, and an intervening bullet item resets the outer
     list, because the single counter has no notion of list level.
3. **The current behavior is pinned as intended by tests** —
   `tests/layout_test.cpp:139-152` explicitly asserts "renumbered from 1
   regardless of source" and "source number not drawn". Those assertions encode
   the bug and must be updated together with the fix.
4. Inconsistency inside the project: the editor's list-continuation already
   *honors* source numbers (`DecideListEnter` increments `N.` on Enter,
   `core/edit_assist.cpp:65-70`). So the display is the outlier.

**Target semantics (CommonMark/GitHub):** each contiguous ordered list at a given
nesting level renders starting at its first item's source number
(`<ol start="N">` semantics), then continues sequentially (`N, N+1, …`); a list
interrupted by a non-list paragraph starts a new list at its own source number;
nested ordered lists number independently; a bullet list nested inside an ordered
one does not reset the outer list.

### Bug A — blockquote spacing (confirmed geometry; "crowding" is real, "overlay" not reproducible in plain sequences)

`core/layout.cpp:343-352`:

```cpp
case BlockType::BlockQuote: {
    ...
    double top = y;
    double yy = layoutWords(cx, words, cx.left + Sc(cx, 16), y);
    fill(cx, {cx.left, top - 2, Sc(cx, 4), (yy + 2) - (top - 2)}, th.border);
    y = yy + Sc(cx, 16);
    break;
}
```

- **No top margin of its own.** Every other block type reserves space on *both*
  sides (paragraphs 16px below, headings 18–24px above, code boxes 12px inner
  padding + 16px below, tables +16 below, hrule 8/16). The blockquote is the only
  block whose gap above is the *previous block's bottom margin*. List items leave
  just **6px** (`core/layout.cpp:391`) — so a quote after a (task) list starts
  within 6px of the previous line, and its 4px bar (emitted at `top - 2`, i.e. 2px
  above the quote's own text) sits only 4px below that text. The repo's own
  `test.md` (task list at lines 24-28, quote at line 30) is exactly this
  sequence — the most likely shape of the reported screenshot.
- **No background**, so the bar + muted text is the only visual separation from
  whatever is adjacent, which amplifies the crowded feel.
- The bar's 2px top overhang (`top - 2`) leans into whatever gap happens to
  precede the quote.
- A pixel-level *overlap* (glyphs on top of each other) does not occur for plain
  sequences in the shared core at scale 1.0 or 0.4–4.0 (traced numerically); the
  overlap in the report is most plausibly the 6px/touching cases, or a sequence
  not in the fixtures (see Open questions — needs the screenshot's exact case).

**Target geometry (GitHub `blockquote` = `margin: 1em 0; border-left: 4px;
padding-left: 1em`):** explicit 16px top **and** 16px bottom margins (paragraph→quote
then matches GitHub's 2em), bar stays 4px at the text edge, text inset stays 16px
from the page edge (12px from the bar, as today — optionally 20px, see T5).

## 3. Design decisions

1. **Add `int listStart` to `Block`** (default 0 = "n/a"; ≥1 for ordered items,
   holding the author's first number, clamped to a sane cap e.g. 1 000 000).
   ASCII `0-9` digit parsing only (do not reuse the locale-sensitive `iswdigit`
   pass for the value), mirroring `DecideListEnter`.
2. **Per-level counters in the layout**, not one global counter:
   `int next[8]; bool live[8];` (list levels; 8 covers the existing 24px/level
   indent scheme and far more than any markdown in practice).
   - ordered item at level L: `live[L] ? next[L]++ : (num = listStart>0 ? listStart : 1, live[L]=true)`
   - any list item at level L: invalidate `live[L+1..]` (deeper lists ended)
   - bullet item at level L: `live[L] = false` (does **not** kill `live[0..L-1]`)
   - non-list block (heading/paragraph/code/table/hrule/quote): invalidate all
   This reproduces all the GitHub cases in §2 while staying a trivially
   one-pass, allocation-free change (no perf impact on the layout-hot path).
3. **Blockquote gets its own 16px top margin** (`y += Sc(16)` at the top of the
   case), keeping the existing 16px bottom. Bar overhang stays within the margin
   band in every block sequence (verified at scale 0.4 and 1.0).
4. **No new paint commands needed** on either frontend — the change is inside
   `LayoutDocument`'s command stream; Win32 `cpp/render.cpp` and macOS
   `mac_render.mm`/`app.mm` need no edits. (The `--parse-dump` and CLI parse
   dump gain a `start=N` token, see T2.)
5. Behavior change is **intended and documented**; the two layout-test fixtures
   asserting the old behavior are updated in the same commit.

## 4. Plan / todos

### Parser (shared core)
- [ ] **T1 — `Block::listStart` field.** Add `int listStart = 0;` to
      `struct Block` in `core/markdown.h` with a comment (0 = unordered/unset);
      set it in the ordered-item branch of `core/markdown.cpp` from the parsed
      digits (ASCII only, capped). Unordered items keep 0.
- [ ] **T2 — Surface it in dumps (parity, debuggability).** Print `start=N` for
      ordered items in the Windows `--parse-dump` (`cpp/fmdv.cpp` ~line 159) and
      the CLI `parse` dump (`frontends/cli/fmdv_cli.cpp` ~line 121). CI substring
      checks (`ci.yml` `parse-dump missing:`) still pass — they match on block
      type/href, not the full line.

### Layout (shared core)
- [ ] **T3 — Per-level ordered numbering.** Replace the global `olCounter`
      (`core/layout.cpp:311,316,376-380`) with the `next[8]/live[8]` scheme from
      §3/2. Number string = formatted `num` + `.`. Keep marker
      non-selectable/unaffected-copy.
- [ ] **T4 — Blockquote top margin.** `y += Sc(16)` at the start of the
      `BlockQuote` case (`core/layout.cpp:343-351`); bottom margin unchanged.
- [ ] **T5 — (Optional, low risk) quote padding tuning.** Increase quote text
      inset 16→20px (`cx.left + Sc(20)`) for a touch more breathing room from
      the bar. Decide at implementation time against the screenshot.

### Tests
- [ ] **T6 — Update the two fixtures that pin the bug** (`tests/layout_test.cpp`):
      - ordered list (§ "ol: renumbered from 1 regardless of source", lines
        139-152): `5. a\n6. b\n\npara\n\n9. c` → now `5.  6.  9.` and a
        `10. d` case renders `10.`; "5." *is* drawn; counter does NOT reset to 1.
      - blockquote (§195-206): keep color/indent/bar asserts; add that the quote
        text top is ≥ 16px below the preceding block's last text line for the
        sequences: paragraph→quote, list-item→quote, hrule→quote (assert on
        `DrawCommand` rects).
- [ ] **T7 — New layout fixtures:** nested ordered list
      (`1. a / 1. x (indent 3sp) / 2. b` → 1,1,2), bullet-inside-ordered
      (`1. a / - sub / 2. b` → 1,2 — outer not reset), single non-1-start item
      (`5. only` → `5.`), sequential-from-start
      (`5. a / 6. b / 8. c` → 5,6,7), quote-bar-vs-adjacent-text clearance.
- [ ] **T8 — Parser fixtures** (`tests/markdown_test.cpp`): ordered items carry
      `listStart` (5/6/9 cases; multi-digit `12. x`); unordered items keep 0.

### Fixtures / CI / docs
- [ ] **T9 — Extend `test.md`** (the CI render+parse gate) with: an ordered list
      starting at a non-1 number, an interrupted list, and keep the existing
      quote-after-task-list sequence. This puts both fixes under the Windows and
      macOS `--dump *.png` CI renders (size-based checks continue to pass —
      content only grows).
- [ ] **T10 — Dev-log entry in `cpp/ISSUES.md`** recording the behavior change
      (ordered lists now honor source start numbers; blockquote gains a 16px top
      margin), round-style format like existing entries.
- [ ] **T11 — Verify.**
      - `make test` + `ctest` (macOS/Linux gate; CI runs both platforms)
      - Windows: `.\build.ps1` (or the CI matrix) + `powershell -File tests\run-tests.ps1`
      - Visual: render `test.md` at width 900 light+dark on each platform
        (`fmdv.exe test.md --dump light.png --width 900`, `fmdv-macos --dump …`)
        and inspect the quote + list regions; confirm zoom 0.5/1.5/2.0 in-app.
      - Bench lines (layout_ms) unchanged in the `--bench-render` output (guards
        the "layout runs once" claim).

## 5. Acceptance criteria

1. `5. a / 6. b` shows **5. 6.**; `5. a / 6. b •para• 9. c / 10. d` shows
   **5. 6. / 9. 10.**; nested ordered sub-lists number from their own first
   source number; a bullet between ordered items does not reset the outer list.
2. A blockquote has ≥16px clear space above its text **regardless of the
   preceding block** (in particular after list items, where it was 6px), 16px
   below, bar not touching any adjacent text, at zoom 0.4–4.0.
3. All pre-existing core checks still pass (~240 checks), the 2 updated fixtures
   now assert the new behavior, CI green on `windows-latest` + `macos-latest`.
4. No frontend code changes required; no new allocation in the layout hot path.

## 6. Risks / notes

- **Layout shifts:** content below any quote moves down 16px (TOC anchors,
  scroll positions, `blockTops` all derive from layout — no manual offsets to
  maintain). Selection/clipboard unaffected (quote markers already
  non-selectable; text fragments unchanged).
- **Deliberate behavior change:** currently-tested "reset to 1" goes away —
  callers relying on it: none found (only the test fixture).
- **Perf:** one array + field; single-pass. Bench gate covers it.
- Number cap: source numbers > 1 000 000 clamp to the cap (defensive; avoids
  int overflow in `num+1`).

## 7. Open questions (needs reporter input)

1. **Screenshot case:** the attached screenshot was not viewable in this review
   session (no image input). The diagnosis covers the provable crowding case
   (quote after list item = repo's own `test.md` shape). If the screenshot shows
   a *literal* overlap in a specific sequence (e.g. nested `>>` quotes, quote in
   a narrow window, a particular zoom), send that document snippet — the plan's
   T4/T5 still address it, but the fixture set (T7) should include that exact
   case first.
2. **Aesthetic:** add a subtle background fill to quotes (code-block `bg2`
   style) to strengthen separation? Not in this plan's scope by default
   (GitHub has none) — opt-in if desired.
