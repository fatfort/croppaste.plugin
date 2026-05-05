---
slave: cropPaste / Phase 0–1 (port + minimum viable raster path)
parent: aayush, via Opus
written: 2026-05-03
target: reMarkable Paper Pro (ferrari, aarch64) AND reMarkable Move (porsche, aarch64), both running xochitl 3.26.0.68
---

# SLAVE — cropPaste plugin (Port & Raster-Insert)

You are a child Claude session for a brand-new XOVI extension on the reMarkable
Paper Pro / Move (both 11.8" ferrari and 7.3" porsche, both aarch64, both
running xochitl 3.26.0.68). The parent session has done the upfront
investigation — you start with the full picture below and execute.

**Goal in one line:** add a button to xochitl's selection tool that copies
the selected region of a PDF (the actual rendered pixels, including the PDF
underlay) and lets the user paste it into another notebook (Quick Sheets,
regular notebook) as a movable/resizable image.

---

## 1. Why this is plausible (not a green-field RE project)

There is **near-complete prior art** at
[`pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector`](https://github.com/pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector).
Read its `README.md` and `REPORT.md` end-to-end before doing anything else,
then read every file in `clipboard-injector/`. It implements the exact
"crop-to-paste" interaction the user wants, but with three differences from
our target that you have to overcome:

1. **Wrong CPU**: targets rM2 (ARMv7-A + NEON, `-mfpu=neon -mfloat-abi=hard`).
   The Pro/Move are aarch64. NEON intrinsics largely port forward to AArch64
   Advanced SIMD (the upstream's `vmull_u8`, `vmlal_u8`, `vshrn_n_u16`, etc.
   compile fine on aarch64 with `<arm_neon.h>` and `-march=armv8-a`), but
   the build flags do not — drop `-mfpu`/`-mfloat-abi` entirely.
2. **Wrong panel format**: rM2 framebuffer is RGB565 grayscale; rMPP/Move are
   colour. Their `framebuffer-spy` already detects both
   (`rmppCondition`/`rmppmCondition` — type=4 BGRA/RGBA, see
   `framebuffer-spy/src/main.c:38-40`); only the BGRA→Gray path needs to run.
3. **Wrong output type — and this is the substantive one.** Upstream produces
   *traced line art* (Laplacian edges → tiny `Fineliner_2` strokes).
   That is not what the user asked for. The user wants **the image as an
   image** — i.e., a raster `SceneImageItem`, not a thicket of `SceneLineItem`s.

Your project is therefore a port + an upgrade. Bring up the upstream's
stroke-trace path on aarch64 first (that's Phase 1, fast feedback,
end-to-end toolchain validation), then replace the output side with
`SceneImageItem` (Phase 2, the actual deliverable).

The parent already verified `SceneImageItem` exists in xochitl 3.26 — the
RTTI string `14SceneImageItem` is present in the binary, and there's a
`make_shared<SceneImageItem>` instantiation symbol
(`St23_Sp_counted_ptr_inplaceI14SceneImageItemSaIvE...`). The class has
its own `Bounds` model (`xostd::tagged_union<SceneImageItem::Bounds,
std::__cxx11::basic_string<char>>`). So image-as-scene-item is real on
this firmware; you just need to find the vtable and the constructor or
an existing QML-callable factory method.

---

## 2. The architecture you are reproducing

The upstream's data flow, which we will copy structurally:

```
[user drags selection rect]  ←── QML overlay (qmldiff-injected)
        │
        ▼
ClipboardInjector::captureArea(x,y,w,h)   ←── Q_INVOKABLE in C++
        │
        ▼
Read framebuffer (via framebuffer-spy)    ←── XOVI inter-extension import
        │
        ▼
[Phase 1: edge-detect → horizontal lines]   [Phase 2: keep raster, encode PNG]
        │                                            │
        ▼                                            ▼
Write /tmp/clipboard_inject.json           Write /tmp/clipboard_inject.json
   (array of stroke records)                 (one image record + bytes/path)
        │                                            │
        ▼                                            ▼
QML "paste" button: ensureVtablePtr() then
   Clipboard.items = ClipboardInjector.loadFromJSON(path)
        │
        ▼
[user navigates to any other notebook]
[user hits native paste — works because Clipboard.items already holds
 valid shared_ptr<SceneItem> instances with correct vtables]
```

The **vtable-stealing trick** is the load-bearing hack. We cannot
instantiate `SceneLineItem` / `SceneImageItem` from outside xochitl
because we have no headers and the symbols are stripped. So we:
- Get QML to call a real native factory (e.g. `sceneController.addDrawingLine`
  for lines; you must find the equivalent for images).
- Clone the resulting clipboard items.
- Read the vtable pointer off the first 8 bytes of the cloned item
  (`SceneItem::vtable` in `clipboard-injector/rm_SceneItem.hpp`).
- Stash that pointer as a static, then reuse it when constructing our
  own struct payloads.

See `clipboard-injector/clipboard-injector.qmd:23-39` (the `ensureVtablePtr()`
function) and `clipboard-injector/rm_SceneLineItem.cpp` for the pattern.

---

## 3. The user's environment (what already exists locally)

Working directory: `/Users/aayushbajaj/Documents/remarkable/cropPaste.plugin/`
(this is your home; your `src/`, `bin/`, `build/`, `reference/` go here).

Sibling plugin dirs to study for build/deploy patterns and qmldiff style:
- `../freeColour.plugin/` — has `MASTER.md`, `Makefile`, `bin/compile-qmd.sh`,
  `reference/qmldiff-workflow.md`, `reference/hashtab` (the rMPP 3.26
  hashtab — 20 017 hashes, 1 353 reverse-resolvable). **You will reuse
  this hashtab directly** for compiling your `.qmd`.
- `../strokeEraser.plugin/` — another qmldiff plugin with `Makefile` you
  can copy.
- `../floatBarPersist.plugin/` — modifies the floating tool bar; QML
  patterns here will be similar to the selection floatbar work in Phase 3.

Already-extracted artefacts for cross-reference:
- `../ferrari/scratch/xochitl-3.26.0.68` — the live xochitl binary from
  device, ARM64 ELF, stripped but with full RTTI strings.
- `../ferrari/scratch/qml-dump/files/` — `Toolbar.qml`, `WritingTool.qml`,
  `PrimaryPenMenu.qml`, `SecondaryPenMenu.qml` extracted by zstd-decoding
  the 1030 frames embedded in the binary. **The selection menu QML
  (`SelectionContextualMenu.qml`) is NOT extracted yet — extracting it
  is on your task list for Phase 3.**
  - **Update 2026-05-03 (Phase 0e):** `cropPaste.plugin/reference/qml/`
    now holds `SelectionContextualMenu.qml` (the *base*
    `ArkControls.ContextualMenu` derivative, 143 lines) AND
    `SceneSelectionHandler.qml` (452 lines, the file that *uses*
    SelectionContextualMenu and contains the existing Cut/Duplicate/
    ConvertToText buttons), plus `DocumentView.qml` and
    `TextSelectionMenu.qml` for context, plus `reference/qml-dump/`
    holding all 739 QML frames as `frame-<hexoff>.qml`. Extractor
    script at `cropPaste.plugin/reference/extract-qml.py`.
  - **zstd-frame gotcha**: the python `zstandard.ZstdDecompressor()`
    `stream_reader(memoryview(blob)[offset:]).read()` API returns
    **0 bytes** for every frame on this binary (frame-end detection
    is broken or the binary uses a frame format the streaming reader
    rejects). Use `decompressobj().decompress(blob[offset:])`
    instead — it honours the zstd end-of-frame marker and returns
    the full frame. 981 of 1030 frames decode cleanly this way.
- `../ferrari/scratch/qml-dump/all-decompressed.bin` — 8.1 MB blob of all
  decompressed QML strings; grep this directly when you need to see
  how a QML construct is used elsewhere in xochitl.

Toolchain (already installed at the user's `~/src/`):
- `~/src/xovi/` — the XOVI framework. **CONFIRM THIS EXISTS AND CLONE IF NOT**:
  ```
  git clone https://github.com/asivery/xovi ~/src/xovi
  ```
  Then build for aarch64. The `clipboard-injector/Dockerfile` clones it
  to `/tmp/xovi` inside the container — same pattern.
- `~/src/qmldiff/` — clone of `asivery/qmldiff`, built at commit
  `533d2b9ceac41d2952d92090eed37298cd627440`. Binary at
  `~/src/qmldiff/target/release/qmldiff`. Used by `bin/compile-qmd.sh`
  in `freeColour.plugin/`.
- `~/src/rm-xovi-extensions/` — the XOVI extensions monorepo
  (asivery's). You depend on:
  - `framebuffer-spy/` — exports `getFramebufferConfig` and
    `refreshFramebuffer`. **Already supports rMPP and Move** (see
    `framebuffer-spy/src/main.c` lines around `rmppCondition` /
    `rmppmCondition`). You import these via `import?
    framebuffer-spy$getFramebufferConfig` in your `.xovi` file.
  - `qt-resource-rebuilder/` — consumes your `.qmd` on device. Required
    by every qmldiff-using extension.
- The user does NOT currently have a Qt6/aarch64 cross-compilation
  toolchain set up. Likely candidates:
  - `eeems/remarkable-toolchain` (the upstream uses `latest-rm2`; check
    if there's a `latest-rmpp` or `latest-aarch64` tag).
  - The reMarkable codex SDK image used to build official apps for the
    Pro/Move.
  - **Task for Phase 0 step 0c**: pick one and document it in `MASTER.md`.

---

## 4. What you should produce, in order

Each phase ends with a checkable demo. Do not move to the next phase until
the previous one is demoed and the parent has confirmed.

### Phase 0 — Setup (deliverable: `MASTER.md` + a building "hello world")

Create `MASTER.md` in this directory mirroring `../freeColour.plugin/MASTER.md`'s
structure (status section, decisions, gotchas). It is your persistence file —
keep it current.

0a. Verify or clone the prerequisite repos at `~/src/xovi`,
`~/src/qmldiff`, `~/src/rm-xovi-extensions`. Confirm `qmldiff` binary
is built and `~/src/rm-xovi-extensions/framebuffer-spy/` has source.

0b. Read end-to-end:
- `https://github.com/asivery/xovi` README and `src/external.h`
- `https://github.com/pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector`
  every file under `clipboard-injector/`, `README.md`, `REPORT.md`
- `../freeColour.plugin/MASTER.md` and `../freeColour.plugin/reference/qmldiff-workflow.md`
- `../freeColour.plugin/Makefile` and `../freeColour.plugin/bin/compile-qmd.sh`
  Do not skip these. The hashtab workflow and Makefile patterns transfer
  directly.

0c. Pick an aarch64 Qt6 cross-compile toolchain. Write a `Dockerfile` at
`./Dockerfile` (templating off the upstream's `clipboard-injector/Dockerfile`
but for aarch64). Document the choice in `MASTER.md`.

0d. Build a minimum "hello-world" XOVI extension: a `.so` that does nothing
but log `[cropPaste] loaded` from `_xovi_construct`. Deploy it to the device
(via SSH/SFTP — user's tablet IP and credentials should be in the user's
`../ferrari/scripts/` directory; if not, ask the user). Confirm xochitl
restarts cleanly with the extension loaded by tailing the systemd journal.
**This validates the toolchain.** Do not skip this — debugging a non-trivial
extension with a broken build path wastes hours.

0e. Extract `SelectionContextualMenu.qml` and `SelectionPreview.qml` (and any
related selection float-bar files) from xochitl. The parent has the technique:
zstd-decode the QML resource frames embedded in the xochitl binary; the
existing dump at `../ferrari/scratch/qml-dump/` shows that the script the
user used pulls 4 named files. Find that script (likely in
`../ferrari/scripts/` or `../ferrari/scratch/`), re-run it with the selection
files added to its target list, and place the outputs alongside the existing
ones. Commit them to your `reference/` subdir. **You will need these to
write the Phase 3 qmldiff.**

### Phase 1 — Port the rM2 stroke-trace approach to aarch64

The point of this phase is to validate the entire pipeline (QML overlay →
C++ capture → vtable trick → JSON bridge → Clipboard.items → paste in another
notebook) end-to-end on rMPP. The output being line-art is acceptable here;
Phase 2 replaces it.

1a. Vendor the upstream's `clipboard-injector/` into `./src/`. Keep their file
names. Do not delete anything yet.

1b. Update the build for aarch64:
- `clipboard-injector.pro`: drop `-mfpu=neon -mfloat-abi=hard`. Add
  `-march=armv8-a` (NEON is mandatory in armv8-a; no flag needed for
  intrinsics beyond `<arm_neon.h>`).
- `Dockerfile`: switch base image to whatever you picked in 0c.
- `clipboard-injector.xovi`: probably no changes needed; confirm
  `framebuffer-spy:0.2.0` dependency is satisfied by what's on device.

1c. Re-RE the `SceneLineItem` struct layout for aarch64:
- The upstream's layout in `rm_SceneLineItem.hpp` was reverse-engineered
  against ARMv7. AArch64 doubles pointer size from 4 to 8 bytes, which
  shifts every offset after the first pointer field. Specifically `vtable`
  goes from offset 0x00 (4 bytes) to 0x00 (8 bytes); `unk_x1c` (a `void*`)
  becomes 8 bytes wide; `unk_x24[3]` of `void*` grows from 12 to 24 bytes;
  alignment to 8 forces inner padding shifts.
- Method: load `xochitl-3.26.0.68` into Ghidra (or radare2/Cutter, both
  free). Find `SceneLineItem` via the typeinfo string `13SceneLineItem`.
  Walk vtable → destructor → cross-reference for the constructor. Trace
  member writes in the constructor to recover field offsets. Cross-check
  with the upstream's invariants: `unk_xc==0, unk_xe==0, unk_x78==1,
  unk_x20==0 || (unk_x20==2 && unk_x21==2)` — these are post-construction
  assertions and let you sanity-check your layout.
- Output: a corrected `rm_SceneLineItem.hpp` with `static_assert(sizeof(...))`
  and the `static_assert(offsetof(...))` lines that prove the layout.

1d. Re-hash the `clipboard-injector.qmd` against the rMPP 3.26 hashtab:
- Decompile the upstream's hashed `.qmd` into plain-name form using
  `~/src/qmldiff/target/release/qmldiff hash-diffs -r
  ../freeColour.plugin/reference/hashtab clipboard-injector.qmd`. **It
  may not round-trip cleanly if the upstream used hashes from a different
  hashtab** — most identifiers will resolve, but ones the upstream needed
  that aren't in our hashtab will surface as raw `[[number]]` blocks in
  the dump. Grep the rMPP hashtab dump for those numbers. If a number
  isn't in our hashtab, the corresponding identifier doesn't exist by
  the same name in rMPP 3.26 xochitl — investigate what changed.
- Once you have a clean plain-name diff, recompile against our hashtab
  using `bin/compile-qmd.sh` (copy from `../freeColour.plugin/bin/`).
- The qmldiff TRAVERSE selectors target `Toolbar.qml`. Confirm via
  `../ferrari/scratch/qml-dump/files/Toolbar.qml` that the structure
  the upstream targets (the `editingTools` row inside the toolbar) still
  exists at the same path on rMPP. If not, adjust selectors.

1e. Adapt framebuffer handling in `ClipboardInjector.cpp`:
- The `bgraToGray` path is what runs on rMPP (type=2 RGBA — actually
  the panel format is BGRA on rMPP, confirm by reading
  `~/src/rm-xovi-extensions/framebuffer-spy/src/main.c`). Keep it.
- The `rgb565ToGray` path is rM1 only; you can leave it dead, no harm.
- Coordinate translation: the upstream reads framebuffer in panel
  coordinates and emits stroke `points` in scene coordinates. On rMPP
  the panel is 1620×2160 (portrait) or 2160×1620 (landscape); the
  scene/page coordinates use a different scale. The upstream may
  hard-code rM2 dimensions — search for `1404` and `1872` and replace
  with reads from `getFramebufferConfig`'s width/height.

1f. Build, deploy, smoke test: drag a rect over some text in a PDF, hit the
upstream's "paste" button, navigate to a Quick Sheet, hit native paste, see
edge-traced strokes appear. **Demo this to the parent before proceeding.**

### Phase 2 — Replace stroke output with `SceneImageItem` (the actual deliverable)

2a. Reverse-engineer `SceneImageItem` against `xochitl-3.26.0.68`. Goal: a
`rm_SceneImageItem.hpp` with full struct layout, vtable address, and
construction recipe. Method:
- Find the typeinfo string `14SceneImageItem` in the binary's strings.
  In Ghidra, search references to that string — the typeinfo
  struct is usually the only one. The vtable for `SceneImageItem`
  immediately precedes (or is referenced by) the typeinfo struct.
- The presence of `St23_Sp_counted_ptr_inplaceI14SceneImageItemSaIvE...`
  proves there's at least one `make_shared<SceneImageItem>(...)` call
  site. Find that allocation; the function calling it is a constructor
  or a factory.
- The presence of `xostd::tagged_union<SceneImageItem::Bounds,
  std::__cxx11::basic_string<char>>` and the `get<0,...>` /
  `get<1,...>` accessors (visible as strings in the binary) tells you
  that a `SceneImageItem` carries either a `Bounds` value or a string
  — most likely the string is an asset path (filename inside the
  notebook bundle) and the Bounds is the in-scene placement.
- Walk the constructor to recover the full layout. Likely fields:
  inherits `SceneItem` (vtable), then probably `pageIndex / sourceLayerId`
  similar to `SceneLineItem`, then a `QImage` or `QString` for
  the image content, then the `Bounds`.

2b. Find a QML-exposed factory you can use for the vtable trick. Candidates,
in order of likelihood:
- The image-import path. xochitl already supports importing PNG/JPEG
  into a notebook (the user has confirmed this exists on rMPP — the
  Refine feature shows "View your original page as an image", and
  cloud-attached images can be inserted). Find the QML method that
  does this. Search `../ferrari/scratch/qml-dump/all-decompressed.bin`
  for strings like `addImage`, `insertImage`, `importImage`, `attachImage`,
  `imageFromPath`, etc.
- If no clean QML method exists, you can call directly into the C++
  constructor via XOVI: declare an `import? _ZN14SceneImageItemC1...`
  in your `.xovi` (after demangling the constructor symbol from your
  Ghidra work). XOVI's `dlsym`-based linker can grab any global symbol
  by name even if it's not exported in the dyn-sym table — but mangled
  names from C++ have to match exactly.

2c. Decide on image-data representation. Two paths:
- **Asset-file path**: write the captured PNG into the notebook's asset
  directory (`/home/root/.local/share/remarkable/xochitl/<doc-uuid>/`),
  reference it by relative filename in `SceneImageItem`. Pro: native,
  matches how xochitl already stores image assets. Con: requires knowing
  the target document UUID at capture time, and the user may want to
  paste into a different doc than the one they captured from.
- **Inline bytes**: stash PNG bytes inline in the `SceneImageItem` (if
  the layout allows it; many implementations would reference an external
  file even when "inline"). Pro: no UUID coupling. Con: may not match
  how `SceneImageItem` actually stores its data.
- **Tmp-file + late-bind**: write PNG to `/tmp/`, store the path in the
  `SceneImageItem` until paste happens, on paste rewrite to the target
  doc's asset dir. Pro: decouples capture from paste-target. Con: extra
  bookkeeping.
- Recommend the **tmp-file path** for v1; revisit after observing how
  xochitl persists pasted image items at sync time.

2d. Replace `loadFromJSON`'s output side: build `SceneImageItem`
shared_ptrs with the scavenged vtable instead of `SceneLineItem`s.
Update the JSON schema in `writeClipboardJSON` to a single image record
(path/bounds) rather than a stroke array.

2e. Update the QML `ensureVtablePtr` to scavenge the `SceneImageItem`
vtable using the factory from 2b instead of `addDrawingLine`/`addDrawingCircle`.

2f. Smoke test: drag a rect on a PDF, hit paste button, navigate to Quick
Sheets, hit native paste, **see the actual rendered region appear as a
movable image item.** Confirm it survives a sync round-trip.

### Phase 3 — Wire into the existing selection tool (UX)

The upstream adds two NEW toolbar buttons (crop + paste). The user
explicitly wants this hung off the existing **selection tool** floating
contextual menu, so that the workflow is:

> select → tap "snapshot" in the float bar → paste anywhere

3a. Using the Phase 0e extracted `SceneSelectionHandler.qml` (NOT
`SelectionContextualMenu.qml` — that's the base
`ArkControls.ContextualMenu` derivative, the *type* registered to QML;
the actual existing Cut/Duplicate/ConvertToText buttons are children of
`SelectionContextualMenu { id: tools; objectName: "selectionHandlerMenu" }`
inside `SceneSelectionHandler.qml`), write a qmldiff that injects a new
menu action ("Snapshot region") next to the existing copy/duplicate/cut
entries.

3b. Wire its handler to call your existing `ClipboardInjector.captureArea`
with the selection's `selectionBoundingRect` (which is in scene coords —
remember to translate to view/panel coords before reading the framebuffer,
or read directly from the rendered scene tile via tileManager).

3c. Optionally: add a sub-toggle "include strokes" / "PDF only" so the user
can grab pristine PDF pixels without their own annotations baked in. The
PDF-only mode would temporarily hide the stroke layer before the framebuffer
read, then restore.

### Phase 4 (stretch) — Polish

- Native Qt clipboard parity. Use `QClipboard::setMimeData` with `image/png`
  so the captured image is also on the system clipboard for any future
  external use.
- Move both the asset-write and the vtable scavenge off the QML thread to
  avoid UI hitches.
- Detect when paste lands in a target that doesn't support raster items
  (text-only canvas?) and either degrade to the Phase 1 stroke-trace
  output or refuse with a toast.

---

## 5. Constraints and rules

- **NEVER push a build to the user's daily-driver tablet without a confirmed
  recovery plan.** A bad XOVI extension can crash xochitl into a boot loop.
  The user has SSH access — confirm you can `ssh root@<ip>` and that
  `/home/root/xovi/extensions.d/` is writeable, *before* pushing your first
  build. If xochitl loops, recovery is to delete the offending `.so` over
  SSH and `systemctl restart xochitl`.

- **Pin to firmware 3.26.0.68.** Document every offset, vtable address, and
  hashed identifier in `MASTER.md` with the firmware version next to it.
  Future firmware bumps will need a re-RE pass; the easier you make that,
  the better.

- **Update `MASTER.md` continuously** in the style of `../freeColour.plugin/MASTER.md`:
  status / what's working / what's not / gotchas / decisions and their
  reasons. Treat it as the single source of truth for the next session
  to read.

- **Do not delete the upstream's stroke-trace path during Phase 2.** Keep
  it available behind a config flag — for content where image insertion
  fails (or for users who actually want vectorised tracing), it's still
  useful. The upstream's code is GPL-3.0-compatible; preserve attribution.

- **Don't invent a separate crop overlay if Phase 3 is reachable.** The
  user's whole point is that the selection tool already has a perfectly
  good selection mechanism — we should hang off that, not duplicate it.
  Phase 1 may temporarily use the upstream's crop overlay just to get a
  working pipeline; Phase 3 must remove it.

- **Don't write any commentary in code that explains "this fix" or "for
  the user request" or "added because X."** Code should describe what
  it does, not its origin story.

---

## 6. When to escalate to the parent

- After Phase 0d (toolchain validation): "hello world extension is
  loaded, here's the systemd log line confirming."
- If `SceneImageItem` reverse-engineering goes >2 days without a
  vtable + struct layout you trust. There's a fallback path
  (Phase 1 output is acceptable as a v1 ship if Phase 2 stalls), but
  the parent should weigh in before you commit to the fallback.
- If the framebuffer-spy doesn't expose what you need on rMPP/Move
  specifically. Test on the user's actual hardware; don't trust
  the upstream's RM2-only assumptions.
- If you discover that xochitl's paste path checks the scene-item type
  more aggressively than just calling `cloneAddAndSelectItems` (e.g.,
  it filters by an enum that `SceneImageItem` happens not to be in
  the allowed set for). That would mean Phase 2 needs a C++ hook on
  the paste path itself, which is a meaningful scope expansion.
- If you find that `SceneImageItem` is **read-only** in xochitl — e.g.,
  it's only used for displaying built-in illustrations and the
  notebook engine refuses to persist user-created instances. That
  would kill Phase 2 outright, and we'd have to fall back to a
  host-side flow (capture region → write `.rm` page using
  `drawj2d image` instruction per `pdf2rmdoc/main.py`, deposit as a
  new page in a notebook UUID dir, let xochitl rescan).

---

## 7. References

- XOVI framework: https://github.com/asivery/xovi (GPL-3.0)
- XOVI extensions monorepo: https://github.com/asivery/rm-xovi-extensions
  (specifically `framebuffer-spy/`, `qt-resource-rebuilder/`,
  `qt-command-executor/` for the qmlRegisterType pattern)
- qmldiff: https://github.com/asivery/qmldiff
- The prior art: https://github.com/pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector
  (this is your starting template — read REPORT.md word-by-word)
- Image-as-rmdoc reference: https://github.com/asivery/pdf2rmdoc proves
  `.rm` v6 supports image insertion via drawj2d's `image` instruction —
  if Phase 2 stalls, this is the host-side fallback.

Local files you will reference repeatedly:
- `../freeColour.plugin/reference/hashtab` (rMPP 3.26 hashtab — 20 017 entries)
- `../freeColour.plugin/reference/qmldiff-workflow.md` (compile/install loop)
- `../freeColour.plugin/Makefile` (template for yours)
- `../freeColour.plugin/bin/compile-qmd.sh` (copy this)
- `../ferrari/scratch/xochitl-3.26.0.68` (binary; load in Ghidra)
- `../ferrari/scratch/qml-dump/all-decompressed.bin` (grep for QML usage)
- `../ferrari/scratch/qml-dump/files/Toolbar.qml` (verify Phase 1 selectors)

---

## 8. Definition of done (project-wide)

The user opens a PDF on the rMPP, activates the selection tool, drags a
selection rectangle around a region of the PDF (text, diagram, photo —
anything visible), taps a new "snapshot" entry in the floating contextual
menu, navigates to a Quick Sheet (or any other notebook), and pastes —
seeing the captured region appear as a movable, resizable raster image
item that survives a sync round-trip.

If you can ship Phase 2 only (still using the upstream's separate
crop button rather than the selection tool's float bar), that's a
shippable v0.5 — the user's spec emphasised the underlying capability
("not unavoidable") more than the UI detail. Phase 3 is the polish
that makes it discoverable; don't let it block v0.5.

---

End of brief. Begin with Phase 0. Update `MASTER.md` as you go.
