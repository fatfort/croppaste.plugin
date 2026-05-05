---
slave: cropPaste / Phase 2 v1.5 (real-capture raster + layer isolation)
parent: aayush, via Opus
written: 2026-05-03
target: rMPP/Move 3.26.0.68 aarch64 (firmware locked permanently — see ~/.claude memory `firmware_lock.md`)
prerequisite: Phase 2 v1 shipped (200×200 black-square round-trip survives sync)
---

# SLAVE — cropPaste v1.5 (real-capture raster + layer isolation)

You are picking up from a previous slave that shipped Phase 2 v1 in one session.
**Read `MASTER.md` first** — it has the firmware constants, the known struct
layout, the AArch64 PCS x8-sret gotcha, and the deploy discipline. Then read
this brief for the specific v1.5 deltas. Do not re-read `SLAVE-PORT.md`
unless something in MASTER.md contradicts what you find on disk.

## Goal in one line

Replace the 1×1 hardcoded test PNG with a real framebuffer capture rendered
faithfully (pixel-for-pixel) into a SceneImageItem on a **dedicated new
layer**, so the user can manage / hide / delete the pasted image without
touching their strokes.

## Scope (two scoped deliverables + one investigation)

### Deliverable A — Real-capture raster paste

The Phase 2 v1 mechanism (`pasteTestImage` → factory @ 0xe58af0 → mutate
vector/QRectF/QTransform → install on Clipboard.items) is proven for a
hardcoded 1×1 PNG. v1.5 wires it up to a real capture.

A1. New C++ method `ClipboardInjector::captureAreaAsImage(int rx, int ry, int rw, int rh)`,
    parallel to the existing `captureArea` (stroke-trace, leave intact).
    Implementation:
    - Read framebuffer via the existing `ci_getFramebufferInfo` bridge
      (BGRA on rMPP/Move, type=2; you've done this in `captureArea`).
    - Construct a `QImage` directly over the FB pointer at the requested
      rect (`QImage::Format_ARGB32` for type=2). Use `QImage::copy(rect)`
      to detach a clean buffer.
    - Encode to PNG via `QImage::save(QBuffer, "PNG")`. Document the
      compression level chosen — default is fine for v1.5; tune later if
      the asset directory gets bloated.
    - Call the factory + mutation path you already have. Source bounds
      QRectF = (0, 0, rw, rh). Transform = identity scaled to land the
      image at a sensible scene location (top-left of selection rect in
      scene coords; reuse the view→scene translation already in
      `captureArea`).
    - Return success/failure the same shape as `pasteTestImage`.

A2. Update the qmd: `areaCaptureButton.onReleased` (currently calls
    `captureArea` + the stroke-trace ensureVtablePtr+loadFromJSON) should
    call `captureAreaAsImage` and set `Clipboard.items` from its return
    instead. Keep the old stroke-trace `captureArea` call site reachable
    behind a debug flag in the .xovi if you want to A/B for comparison —
    don't make it the default.

A3. Fold in the **cropOverlay→captureArea race fix**. The Phase 2 v1 deploy
    journal showed `cropOverlay.visible = false` doesn't synchronously
    repaint, so the next-line `captureArea` reads a framebuffer that still
    contains the white selection box and the 30% black tint. Two ways to fix:
    - **First-choice (correct):** connect the cropOverlay's hide to a
      `Window.frameSwapped` (or `QQuickWindow::afterRendering`) signal,
      and trigger `captureAreaAsImage` only after one full paint cycle has
      drawn the overlay-free frame. Use Qt6 signals correctly — this is a
      one-shot connect inside the onReleased handler.
    - **Fallback:** `ClipboardInjector.sleepMs(50)` between
      `cropOverlay.visible = false` and the capture call. Acknowledge it
      as a hack in MASTER.md if you take this path, with a TODO to revisit
      with the signal-based fix later.

A4. Verify with a real capture of mixed content (text + image). The user
    flagged "thickening" on the Phase 1 stroke-trace; v1.5 should render
    text crisp because it's pasted as raster, not edge-traced. If it
    doesn't look right after the timing fix, that's a finding worth
    investigating — but don't pre-emptively chase artefacts that may not
    exist.

### Deliverable B — Layer isolation

User wants the pasted image on its own xochitl notebook layer so it doesn't
entangle with their strokes. They can then hide/show/delete the layer as a
unit, independent of stroke work.

B1. xochitl's `SceneController` exposes `addLayer()` and `currentLayer` to
    QML — both visible in the QML dump (`sceneController.addLayer();` is
    used in stock notebook UI). The clean implementation is:
    - In the qmd's paste handler, before assigning to `Clipboard.items`,
      call `sceneController.addLayer()` to create a fresh layer.
    - Capture the new layer's id (whatever `addLayer` returns or what
      `currentLayer` reads as immediately after).
    - Set the SceneImageItem's `sourceLayerId` field at construction
      to that layer id. (You'll need to find this offset — Phase 1's
      SceneLineItem has it at offset 0x14; SceneImageItem may be the
      same or different. Likely same since both inherit from SceneItem.
      Check via the heavy-log hexdump of the default-init from Phase 2 v1
      — you may already have ground truth.)
    - Optionally: switch `currentLayer` back to the user's prior active
      layer after paste, so subsequent strokes don't accidentally land on
      the image's layer. User-preference question — default to switching
      back since "I just pasted, now I want to keep drawing where I was"
      is the more common case.

B2. Verify in the device UI: after paste, the layers panel (long-press
    layer icon? — find the UX) should show a new layer containing the
    image. Toggling its visibility should hide the image without affecting
    strokes. Deleting the layer should remove the image cleanly.

B3. Edge cases to handle or document:
    - User pastes multiple images in succession — each on its own layer,
      or all on the same "screenshots" layer? Default to each-on-own for
      v1.5; a "consolidate to screenshots layer" mode is Phase 3 polish.
    - Sync round-trip: confirm the new layer + its image item both persist
      across a notebook close/reopen and an actual cloud sync.
    - User has hit xochitl's max-layer cap (does one exist?). Document
      what happens; don't engineer a workaround for v1.5 unless the cap
      is reachable in normal use.

### Deliverable C — Porsche parity (Kiyomi's device)

cropPaste must also ship to porsche (rM Move, 7.3"). Same firmware version
(3.26.0.68, locked permanently). The qmldiff side is device-portable (same
QML resources baked in), but the **C++ binary offsets are NOT free** —
ferrari and porsche may have different xochitl builds even on the same
firmware version, and the v1's pinned constants
(SceneImageItem vtable @ 0x16a3df0, factory @ 0xe58af0, SceneLineItem
vtable @ 0x16a4338) were RE'd against ferrari's binary at
`../ferrari/scratch/xochitl-3.26.0.68`.

C1. Pull porsche's xochitl binary to host. Likely path on device:
    `/usr/bin/xochitl`. SCP to `../porsche/scratch/xochitl-3.26.0.68`
    (mirror ferrari's layout — confirm a `porsche/scratch/` dir exists or
    create it).

C2. Hash-compare ferrari and porsche binaries (`shasum -a 256`). Three
    cases:
    - **Bytewise identical** → no further RE work. The constants in
      MASTER.md apply to both devices; just deploy the same .so.
    - **Different but same size + same RTTI string offsets** → constants
      may shift by a fixed displacement; quick check in Ghidra.
    - **Genuinely different builds** → walk the same RTTI path
      (`14SceneImageItem` typeinfo string → vtable → destructor →
      factory cross-references) on porsche to find porsche-specific
      addresses.

C3. If porsche's constants differ, implement per-device runtime dispatch.
    `framebuffer-spy` already returns the panel dimensions
    (1620×2160 = ferrari, 960×1696 = porsche), so device detection is
    free at startup — no extra IPC needed. Wire a one-time init that
    picks the right vtable + factory based on FB geometry and stashes
    them in the existing `SceneImageItem::vtable_ptr` etc. statics.

C4. The address-signature check from v1 is your safety net here too:
    if a porsche deploy hits an address that doesn't match the prologue
    we expect, the check refuses to call and logs the actual bytes —
    you discover the mismatch in the journal in seconds, no SIGSEGV.

C5. Document both sets of constants in MASTER.md under a "Per-device
    pinned constants" section. The deployment story per-device is then:
    same .so, same recovery primitive, different constants picked at
    runtime.

C6. Test the full v1.5 deliverable list (real-capture, layer isolation,
    sync round-trip) on porsche after it works on ferrari. Same gate
    pattern: small capture before mixed-content, heavy-log first
    invocation.

### Investigation thread — Per-pixel erasability of pasted images

User explicitly asked "if that's even viable" — meaning they're aware this
may not be feasible. **Investigate, don't implement, in v1.5.** Report
back with findings; we decide whether to scope it into a future phase.

The v1 slave noted: "image items aren't strokes, deleted via selection
lasso instead — feature, not bug." That's xochitl's stock semantic: the
eraser tool only operates on stroke items. To make the eraser work on an
image, one of these would need to be true:

- xochitl's eraser tool dispatches to a per-scene-item-type erase
  handler, and SceneImageItem has one we haven't found. Cheap to check —
  grep the binary's symbols for `Erase`, `eraseAt`, etc., and walk the
  vtable of SceneImageItem to see if it has an "erase region" virtual.
- The eraser modifies the image in place (replacing erased pixels with
  the page background colour, or with transparency). This would be a
  C++ hook on the eraser path that we'd need to implement, not native
  behaviour.
- A pre-paste rasterise-into-strokes mode where each captured pixel
  becomes a tiny stroke (essentially what Phase 1 did, but without edge
  detection — every dark pixel is a 1×1 fineliner). Erasable but produces
  thousands of items per capture and revives the thickening problem.

Investigate option 1 first (cheapest signal). If SceneImageItem's vtable
has no erase virtual and there's no per-type dispatch in the eraser path,
report that finding and stop — options 2 and 3 are scope decisions for
the parent, not for v1.5.

Time budget for the investigation: half a day max. If you can't find a
clean answer in that time, report what you've ruled out.

## Constraints

- **Do not break the Phase 2 v1 test path.** The `imageTestButton` /
  `pasteTestImage` route stays intact for regression testing. Add the
  new method alongside, don't replace.
- **Keep MASTER.md current.** Pinned constants section already has the
  v1 vtable + factory. Add: PNG encode mechanism, layer-id offset (once
  RE'd), the timing fix actually deployed, and the erase-investigation
  outcome.
- **Same deploy discipline as v1** — see `~/.claude` memory
  `feedback_xochitl_deploy_discipline.md` (or just follow what worked in
  Phase 2: address-signature check, smallest-payload first, heavy-log
  first invocation, recovery primitive prepped, second SSH journal tail).
- **Don't touch Phase 3 (selection-tool menu integration).** v1.5 keeps
  the standalone toolbar buttons; the selection-tool integration is
  Phase 3's brief.

## Gate sequence

Same staged-blast as Phase 2 v1:

1. **Build gate** (no device touch): all four pre-deploy items in your
   build (signature check, heavy-log, allocator pairing, etc.) — confirm
   they're still wired into the new `captureAreaAsImage` path. Report
   to parent before SCP.
2. **First-deploy gate (small real capture)**: capture a 100×100 region
   of a blank notebook corner. Confirms the FB read + PNG encode + paste
   path works on a controlled input. Heavy-log on for this invocation.
3. **Mixed-content gate**: capture a region of actual text + image
   content from a PDF. This is what the user cares about — confirm
   fidelity (no thickening, no border artefacts after the timing fix).
4. **Layer isolation gate**: confirm the pasted image is on its own
   layer via the layers panel. Confirm hide/show/delete the layer
   works as expected.
5. **Sync round-trip gate**: close notebook, reopen, confirm image +
   its layer persist. If sync infrastructure is available (cloud or
   USB), do a full sync round-trip.

Report verbatim journal blocks at each gate. Disable heavy-log after
gate 3.

## Definition of done

User can, on **both ferrari and porsche** (firmware 3.26.0.68 on both):
1. Activate the existing `areaCaptureButton`.
2. Drag a selection over PDF content.
3. Release — the captured region appears on a new layer in xochitl's
   clipboard, faithful to the original (no edge-tracing artefacts, no
   selection-box border baked in).
4. Navigate to a Quick Sheet (or any notebook), tap native paste — the
   image appears as a SceneImageItem on its own dedicated layer.
5. Open the layers panel, see the new layer, can hide/delete it
   independently of any strokes they later draw.
6. Survives notebook close/reopen and sync.

The investigation thread on per-pixel erasability returns one of:
- "Found a viable mechanism, here's the path" (parent scopes it for next phase).
- "Confirmed not viable, here's what was ruled out" (parent accepts and the
  selection-lasso-as-erase-mechanism stays as the documented behaviour).

## Pickup list (priorities, sequenced)

1. Read `MASTER.md` end-to-end. Confirm Phase 2 v1 mechanism, firmware
   constants, deploy discipline, all match what you see on disk.
2. A1+A2: implement `captureAreaAsImage` and wire `areaCaptureButton`
   to it. Build, do not deploy.
3. A3: implement the timing fix (try frameSwapped first; sleepMs
   fallback if signal hookup is fiddlier than 30 minutes).
4. Build gate report to parent.
5. After parent sign-off: gate 2 deploy (small blank capture).
6. Gate 3 + 4: scale up, add layer isolation (B1).
7. Gate 5: sync round-trip.
8. Investigation thread (parallel with the above where it makes sense
   — vtable inspection of SceneImageItem doesn't need device touch).
9. MASTER.md updated end-to-end before closing.

Begin with the MASTER.md read. Then plan and report back before any code.
