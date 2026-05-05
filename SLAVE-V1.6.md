---
slave: cropPaste / v1.6 (color raster — populate or bypass the SceneImageItem cache)
parent: aayush, via Opus
written: 2026-05-04
target: rMPP/Move 3.26.0.68 aarch64 (firmware locked permanently)
prerequisite: v1.5 shipped (monochrome stroke-trace with thinning pass on ferrari)
---

# SLAVE — cropPaste v1.6 (color raster)

You are picking up from the v1.5 slave. **Read MASTER.md end-to-end first**
— specifically the v1.5 RE block under "v1.5 — RE findings to carry into
v1.6 (do NOT re-investigate)" — before touching any code or doing any RE.
That block has the dead-ends that will eat your day if you re-walk them.

## Goal in one line

Make the captured PNG actually render as **color raster** in xochitl when
pasted, instead of the dotted-rect placeholder it currently falls through
to.

## Why we're here

v1.5 ships the existing Phase-1 stroke-trace pipeline as the user-facing
output (with a thinning pass — visibly less blurry than v1, but still
monochrome line art, not raster). The reason we shipped strokes instead
of true raster is documented in MASTER.md and is critical to your design:

The Phase 2 v1 SceneImageItem path *plumbs* end-to-end (factory call,
struct mutation, vtable scavenge, Clipboard.items round-trip), but
xochitl's render code for SceneImageItem (`vt[3]` paint @ 0xe83630)
hashes `this->uuid` (offset 0x48), looks up an in-memory image cache,
and on cache miss draws a dashed-rect placeholder via
`setPen(DashLine) + drawRects(source_bounds, 1)`. **The cache is never
populated.** The std::vector<uint8_t> we set at offset 0x30 is dead — the
paint function never reads it. v1.5 RE established this empirically;
don't re-test.

## The hard constraint: no xochitl restart during use

Stated explicitly by the user during v1.5 closeout — see memory file
`feedback_no_restart_during_use.md`. This rules out designs that need to
close+reopen the destination notebook to trigger xochitl's file watcher
(or any other restart-style refresh). cropPaste is invoked mid-reading,
mid-annotating; restarts throw away the user's flow. **Bias all v1.6
design toward in-process render-time modifications.**

## Three paths to color raster

Ranked by parent's preferred sequencing. All three are technically
viable; the no-restart constraint biases away from path 1.

### Path 2 — Custom vtable with paint() override (parent's first recommendation)

Build a per-SceneImageItem custom vtable that overrides only `vt[3]`
(paint). Every other slot keeps xochitl's original implementation
(destructor, clone, all the other virtuals stay correct). When xochitl
calls paint() on one of *our* SceneImageItems, it runs *our* paint
function, which receives the QPainter and the source rect, and renders
our stashed QImage directly via `painter->drawImage(rect, qimage)`.

Why this is the cleanest path:
- **No cache RE needed at all.** We sidestep the lookup → insert
  problem entirely.
- We already control item construction (factory call + struct mutation).
  Adding a vtable substitution at construction time is a few lines.
- xochitl's stock SceneImageItems keep xochitl's original vtable —
  nothing else in the system is affected.
- No restart. No file-watcher tricks. Items render correctly the moment
  xochitl's render loop ticks them.

Implementation sketch:

```cpp
// One-time init, after factory call returns a fresh SceneImageItem:
static void* customVtable = nullptr;
if (!customVtable) {
    // Read xochitl's vtable size — count entries until typeinfo header
    // marker, or just allocate generously (e.g., 512 bytes) and copy.
    customVtable = ::operator new(VTABLE_BYTES);
    memcpy(customVtable, (void*)0x16a3df0, VTABLE_BYTES);
    // Slot 3 is paint — overwrite with our implementation:
    ((void**)customVtable)[3] = (void*)&ourPaint;
}
// Substitute on the freshly-created item:
*(void**)freshItem = customVtable;
// Stash the QImage in a side map keyed by `this`:
g_imageMap[freshItem] = std::move(capturedQImage);
```

The paint function signature: dump xochitl's vt[3] disasm to recover the
exact prototype. Almost certainly something like
`void paint(SceneImageItem* this, QPainter* painter, ...)` —
the `...` may include source rect, transform, or render context. Match
xochitl's signature exactly; call convention matters. If you get the
signature wrong, you crash on first paint — same `tryCast`-equivalent
discipline as Phase 1: log the args you receive on first invocation,
disable after success.

Risks:
- **Vtable size unknown.** Generous allocation (512 B / 1 KB / 2 KB)
  bounds it — no concrete cost, just over-allocate. Pad with original
  vtable bytes so any high-slot virtual we don't know about still works.
- **Side map for `this → QImage`.** We need to look up the QImage in our
  paint callback. A `std::unordered_map<void*, QImage>` keyed by the
  item pointer works. Cleanup: hook the destructor (vt[1]) too — when
  it runs on one of our items, erase from the map *then* call original
  destructor.
- **Threading.** xochitl renders on a dedicated thread; the map needs
  a mutex if construction and paint can race. Default to mutex; revisit
  if profiling shows contention.

Investigation budget: half day. If the signature recovery from disasm
is solid, implementation is a long afternoon.

### Path 3 — Cache-insert function call (v1.5 slave's second-ranked path)

RE the 6 remaining `qHash(QUuid)` call sites (addresses listed in
MASTER.md "v1.5 — RE findings"). One of them is the cache-insert
function — distinguishable from the lookup pattern by writing to the
bucket array on miss/find rather than reading. Once found, call it
directly with `(uuid, our_QImage)`. Subsequent paint calls then hit
the cache and render correctly.

Why this is plausible but not first-choice:
- It's the architecturally cleanest result if the function exists and
  has a callable surface.
- The 5 already-classified sites were all reads — that's evidence the
  insert exists (xochitl populates the cache somehow), not yet evidence
  about which of the 6 remaining is it.
- Risk: the insert may live inside a member function with a complex
  this-pointer + state setup, making "call directly" mean "synthesize
  the right caller context." That can cascade.

Investigation budget: half day on RE. If a clean callable insert is
found, implementation is straightforward. If the only-callable path
requires synthesizing complex caller state, abandon and go to path 2.

### Path 1 — `.rm` scenefile mutation (last resort)

Write captured PNG to disk under a known UUID-derived path, append a
SceneImageItem entry to the destination page's `.rm` file, hope
xochitl's file watcher triggers a re-render.

Why this is last:
- The slave's v1.5 RE found that shotgun-writing PNGs to `.thumbnails`
  dirs **does NOT trigger an auto-load**. So either the cache load is
  gated on notebook reload (violates no-restart constraint) or fires
  through a code path we haven't identified.
- Even if the file-watcher path works, we now need destination
  doc-uuid + page-uuid discovery at paste time (QML-side or via paste
  hook), plus correct `.rm` v6 binary format authorship for
  SceneImageItem entries (rmscene Python lib helps but we'd port to C++
  or shell out).
- Architecturally invasive: abandons the Clipboard.items model; we'd
  bypass the user's "navigate then paste" flow.

Try this only if both path 2 and path 3 fail. If you reach this fork,
report to parent for a scope-vs-UX call before committing.

## Constraints

- **Color, not monochrome.** v1.5's stroke-trace already shipped the
  monochrome fallback. v1.6's deliverable must render the captured
  region with its actual colors (rMPP supports a 4-color palette plus
  greyscale; whatever the framebuffer holds is what we paste).
- **No xochitl restart, no notebook reload, during the user's invoke.**
  In-process render-time modifications only. See feedback memory.
- **Both ferrari AND porsche.** v1.5 shipped ferrari-only because
  porsche's factory address is still TBD. v1.6 needs to land both.
  - Per-device dispatch already sketched in MASTER.md; finish it.
  - Porsche factory RE is a parallel host-side workstream — start it
    early so the porsche binary tweaks are ready when path 2 / 3 lands
    on ferrari.
  - Path 2 (custom vtable) needs porsche's SceneImageItem vtable
    (already RE'd: 0x1572900) and porsche's paint vt[3] address (TBD).
    Path 3 needs porsche's qHash callers identified separately. Path 1
    is largely device-portable but still needs porsche-side .rm format
    handling.
- **Don't break v1.5's monochrome stroke-trace path.** Keep it
  reachable behind a debug flag or as a fallback if path 2/3 hits an
  edge case. The user has v1.5 in their hands; v1.6 should add color
  raster, not regress monochrome.

## Gate sequence

Same staged-blast pattern as Phase 2 v1. Don't skip gates.

1. **Plan-and-budget gate** (no code): pick path 2 or path 3 to start.
   Report decision + budget + risks to parent before any code or RE.
   Parent default-recommends path 2.
2. **Build gate** (no device touch): all six discipline items wired
   into the new code path (signature checks for any newly-pinned
   addresses, heavy-log first invocation, allocator pairing if
   relevant, recovery primitive prepped). Report to parent before SCP.
3. **First-deploy gate (smallest visible test)**: one fresh capture of
   a known-color region (e.g., a yellow highlight in a PDF, or a
   colored shape from a notebook page). Single-frame test. Confirm:
   (a) renders in color, (b) PID stable, (c) no journal complaints,
   (d) survives notebook close/reopen for the persistence question.
4. **Mixed-content gate**: real text + image PDF region capture. The
   user's actual use case. Confirm fidelity end-to-end.
5. **Porsche parity gate**: same flow on Kiyomi's device after porsche
   addresses are RE'd.

Heavy-log first invocation, disable after gate 4. Recovery primitive
unchanged: `ssh root@<ip> 'rm /home/root/xovi/extensions.d/cropPaste.so && systemctl restart xochitl'`. Stash the v1.5 .so before SCP — that's
your known-good rollback.

## Definition of done

User can, on **both ferrari and porsche**:
1. Activate the existing crop button.
2. Drag a selection over PDF content (any color, including highlights).
3. Release; switch to selection tool; tap paste in destination notebook.
4. See the captured region appear as a faithful **color raster image**
   that visually matches the source.
5. The image persists across notebook close/reopen and sync.
6. No xochitl restart was required at any point in the workflow.

Stretch (out of scope unless trivial): the stock layer-isolation
descope from v1.5 (image lands on currentLayer, not a dedicated layer)
remains — addressed in a possible v1.7 with destination-side qmldiff.
Don't pull that in here.

## Investigation: do not redo

The v1.5 slave burned cycles on these dead-ends. Re-read MASTER.md's
"do NOT re-investigate" list before any RE. Specifically don't:

- Cycle the byte at offset 0x28 expecting render changes.
- Shotgun PNGs to `.thumbnails` dirs expecting an auto-load.
- Pursue `Window.frameSwapped` for capture timing — Toolbar.qml import
  scope was the blocker, not the signal.
- Re-walk the 5 already-classified `qHash(QUuid)` call sites — they're
  all lookups. The 6 unsurveyed ones are at:
  `0xe126e8, 0xe12ab8, 0xe16590, 0xe16f1c, 0xe63700, 0xe6f1d0,
  0xe70d14, 0xe747a4, 0xed4b6c, 0xf1f3cc, 0xf2449c`
  (per MASTER.md — note the list shows 11 because 5 of those are the
  surveyed ones; the surveyed ones are documented in MASTER.md, exclude
  those when starting Path 3 RE).

## Pickup list

1. Read MASTER.md end-to-end. Especially the v1.5 retro RE findings.
2. Read this brief.
3. Pick path 2 or path 3 (default: path 2). Report decision + plan +
   budget to parent.
4. After parent sign-off: implement. Build gate.
5. After build sign-off: device deploy gates 3 → 4.
6. Porsche RE workstream in parallel (host-side, blocks nothing).
7. MASTER.md updated end-to-end before closing.

Begin with the MASTER.md read. Then plan and report back before any
code or RE.
