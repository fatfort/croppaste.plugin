# MASTER — cropPaste project state

You are the **slave** Claude session for the cropPaste xovi extension on
reMarkable Paper Pro / Move (firmware 3.26.0.68, both 11.8" ferrari and
7.3" porsche, both aarch64). When re-invoked, read this file end-to-end
first, then `SLAVE-PORT.md` (the parent's brief) for full context.

## What this project is

A xovi/qt-resource-rebuilder distribution that adds a **crop-and-paste**
button to xochitl's selection tool. User flow:

> open PDF → activate selection tool → drag a rectangle → tap
> "snapshot" in the floating contextual menu → navigate to any
> notebook (Quick Sheet etc.) → native paste → captured region appears
> as a movable, resizable raster image.

The captured region is the **rendered pixels** (PDF underlay + the
user's strokes), not vectorised line art. The output goes onto the
clipboard as a `SceneImageItem` so the existing native paste path
handles it.

Project is a port + upgrade of
[`pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector`](https://github.com/pragmatically-dev/RM2-Screenshot-to-Clipboard-Injector)
(rM2, ARMv7, vector-trace output). We change CPU (aarch64), framebuffer
format (BGRA), output type (raster image vs traced strokes), and UI
home (selection float-bar vs new toolbar buttons).

## Status — v1.7 SHIPPED (2026-05-04 late, ferrari only)

User-facing flow against the no-compromise bar: open PDF or notebook →
crop → drag rect → switch to selection tool → paste → captured region
appears as a movable, resizable, full-color raster image. Survives
navigate-away-back, `systemctl restart xochitl`, reboot, OOM, OTA.
PDF rendering stable across multiple opens with patch active.

**Final binary:** `build/v1.6j/cropPaste.so`, sha256
`0418d77d364ac6d9299b79a41d82c6254b45cc104620c4f3a2930709b07b316f`,
1 036 584 B. Running on ferrari at PID 67617 (deployed 03:39:11Z
2026-05-04, ship-confirmed 03:42Z). All rollback rungs preserved on
disk: `build/v1.5-rollback/`, `build/v1.6f-rollback/`, `build/v1.6g/`,
`build/v1.6h-rollback/`, `build/v1.6i-rollback/`. Keep ≥1 week post-ship
for bisection if user-reported regression comes in.

### Architecture (four pieces)

1. **Stock-vtable global patch.** mprotect+write `SceneImageItem::paint`
   slot @ `0x16a3df0[3]` to redirect through `ourGlobalPaint`. Stock
   items pass through unchanged via `g_origPaint` @ `0xe83630`.
2. **UUID-keyed in-memory side-map.** `unordered_map<UuidKey, QImage>`.
   UUID written at `obj+0x48` via `setImageUuid` at capture; xochitl's
   serializer preserves it across `.rm` round-trips losslessly (verified
   at byte level; see Gate-5 PASS, Problem B (b) section below).
3. **PNG disk store at `/home/root/xovi/exthome/cropPaste/<uuid>.png`.**
   Save on `registerImage`, scan-and-load on `installPaintHook`
   post-guard. Survives process restart, reboot, OTA. Filename =
   `QUuid::toString(WithoutBraces)` for host-script-readable export.
4. **Install-time program-name guard.** `strcmp(program_invocation_short_name,
   "xochitl") != 0 → return success without dereferencing.` Prevents the
   LD_PRELOAD'd .so from running install code in `xochitl_pdf_renderer`
   (separate binary, address-space mismatch). Runtime PID guard inside
   `ourGlobalPaint` retained as defense-in-depth.

### v1.6d → v1.6j compressed timeline

- **v1.6d/e** — global vtable patch shipped color + selectability;
  renderer crashed within 60s of deploy. Two rollbacks.
  Persistence-across-revisit unverified (interrupted by crashes).
- **v1.6f-diagnostic** — install stubbed; 0 renderer-exits in 60s
  → confirmed the patch is the trigger. Static init / .so load innocent.
- **v1.6g** — UUID generator switched from manual urandom+bit-twiddle
  (non-conforming v4 under QUuid struct LE interpretation) to
  `QUuid::createUuid()`. Install still stubbed. Persistence still failed
  on revisit → UUID conformance was not the cause.
- **v1.6h** — install re-enabled + disk-persistent dispatch log
  (`/tmp/cropPaste_dispatch.log`) added. User no-PDF-90s protocol let
  patch reach stable state. **Outcome A confirmed**: deserialized items
  dispatch through our hook, side-map hits, drawImage paints. Persistence-
  across-navigation never broken — just untested. PDF open still crashed
  renderer (Problem A live).
- **v1.6i** — added install-time program-name guard. PDF open clean,
  guard fires for `xochitl_pdf_renderer` and skips install. **Problem A
  solved.** New gap surfaced: pre-restart pastes octagon because side-map
  didn't survive process restart.
- **v1.6j** — added PNG disk store with save on `registerImage` and
  load-on-install scan. Verified: paste → restart → revisit renders in
  color, not octagon. Selection/move work post-restart. **Ship.**

### Mechanism breakthrough — Problem A's real cause

`xochitl_pdf_renderer` is a fork+exec child of xochitl, not a fork-only
child. LD_PRELOAD inherits across `execve`, so xovi.so loads in the
renderer too, our `_xovi_construct` runs in the renderer too, our install
code runs in the renderer too. The install dereferences hardcoded
xochitl-binary addresses (`*reinterpret_cast<uintptr_t*>(0x16a3dc8)` for
the typeinfo check). In the renderer's address space (separate 266 KB
binary, totally different mapping), `0x16a3dc8` is unmapped → SIGSEGV →
renderer dies → xochitl logs `no document renderer!`.

The diagnostic signal `_xovi_construct entered: program=xochitl_pdf_renderer
pid=N` was visible in the journal as early as v1.6f. We didn't connect
"extension loads in renderer" with "install dereferences xochitl-binary
addresses" until v1.6h's trip-wire trace forced the connection.
**Lesson: when LD_PRELOAD'ing an extension that hardcodes host-binary
addresses, always guard install on host-binary identity (exact
`program_invocation_short_name` strcmp) before any address dereference,
even before considering runtime concerns.** This collapsed every H4+
hypothesis (Qt mutex held across fork, PDFium global state torn,
atfork handlers, anonymous-shm region sharing) into one mundane root
cause. See memory `gotcha_xovi_install_addresses_must_match_host.md`.
The prior memory `gotcha_xovi_global_hook_fork.md` has been corrected
with a header note: the runtime PID guard it documents is still useful
defense-in-depth, but the renderer-crash mechanism it attributed to
fork-only-child mutex deadlock was a hypothesis we never falsified or
confirmed; install-time address mismatch was the real cause.

### Pinned firmware constants (3.26.0.68 ferrari, locked)

`SceneImageItem` vtable `0x16a3df0`, typeinfo `0x16a3dc8`,
paint @ vt[3] `0xe83630`, QPainter helper @ ctx+0xd0 `0xec5ad0`,
generic factory `0xe58af0` with prologue `(0xd503233f, 0xa9bd7bfd)`,
patch slot index 3. Image store `/home/root/xovi/exthome/cropPaste/`.
Dispatch log `/tmp/cropPaste_dispatch.log`. Per-device table for porsche
(`SceneImageItem` vtable `0x1572900`, factory TBD via ADRP+ADD walk)
elsewhere in this file under "Per-device pinned constants".

### Known limitations + retained debug logging (do NOT strip in v1.7.0)

- **Porsche parity not built.** Ferrari only this ship. Porsche
  `SceneImageItem` vtable known; factory address + factory prologue
  signature + paint vt[3] need RE because porsche loads vtable via
  ADRP+ADD PC-relative. Architecture (vtable patch + UUID side-map +
  PNG disk store + program-name guard) ports verbatim — only addresses
  change. Per-device constant dispatch on the C++ side is the entire
  scope. 1-2 sessions when Kiyomi's device reachable.
- **No GC for orphaned PNGs.** When user lassos+deletes a SceneImageItem,
  the PNG file stays on disk. Each PNG ≤2MB typical. Queued for v1.7.1
  as lazy GC: scan all `.rm` under `/home/root/.local/share/remarkable/xochitl/`
  for referenced UUIDs at install time, delete unreferenced PNGs. More
  bulletproof than eager destructor-hook GC which could miss edge cases.
- **No restart-persistence for pre-v1.7 pastes.** Items pasted under
  v1.6g/h/i (before PNG disk store existed) octagon on revisit because
  no PNG file backs them. Acceptable; user understands.
- **Heavy-log first-event gates retained** (`[cropPaste:img] heavy=1`
  on first paste, `[cropPaste:disk] save:` on first save, `[cropPaste:hook]
  first hit / first miss` once each, `[cropPaste:disk] startup scan`
  on every install) plus per-invocation entries appended to
  `/tmp/cropPaste_dispatch.log` (bounded ~30 lines per session). Cost is
  rounding error vs. observability if a user-reported regression comes in.
  Strip in v1.7.1 after a week clean.

### Queued v1.7.x work (ordered by priority)

1. **Porsche parity** — host-side RE (factory @ porsche, prologue, paint
   vt[3]); per-device dispatch on C++ side; smoke deploy on Kiyomi's
   device when reachable.
2. **v1.7.1 cleanup pass** — strip dispatch log + first-event chatter
   once a week of user-stable runtime confirms nothing else is loose.
3. **Lazy GC** — `.rm`-scan-driven orphan PNG cleanup at install time.
4. **Multi-device side-map sync** (stretch) — sync the image store via
   the device's existing sync pipeline so a paste on one device
   round-trips to the other.

## Investigation history — v1.6 exploration (2026-05-04, SUPERSEDED by v1.7 — kept for archaeology)

Three architectures tried, none shippable.

**Device state corrected (2026-05-04 late, parent verified):** prior closeout
claimed "v1.5 restored as working build" — that claim was FALSE. Verified
2026-05-04: `/home/root/xovi/extensions.d/cropPaste.so` sha256 =
`14d1430878fb11c2521ce5d28753064e1a170a8810c5c4875c989aa55b7d279b` (matches
local `src/clipboard-injector/clipboard-injector.so`, the v1.6f diagnostic
build). v1.5 was never redeployed. xochitl PID 18545, exe `/usr/bin/xochitl`.
v1.5-rollback `.so` is stashed at `build/v1.5-rollback/cropPaste.so` (sha256
`6abb86be2843827e6372f1cd802b63833856064e42f6da462cf340b5739c0f2e`) but is
NOT on device. Current on-device behavior: `installPaintHook` stubbed → no
vtable patch active → SceneImageItem cache miss → dashed-rect placeholder
for our captured items, PDFs work because no patch. **Don't redeploy any
v1.6 variant without re-establishing the gate-3 trip-wire (any renderer-exit
in the 60s post-deploy window = rollback).**

User's stance, verbatim: *"I insist that we continue digging and iterating
until we can have our cake and eat it too."* The full bar — color raster +
selection-tool resize/move + survives navigate-away-and-back + no PDF
regression — must hold simultaneously before ship. Don't re-propose
ship-with-limitations paths in future sessions; the user has rejected
v1.6c-style tradeoffs and v1.5-as-final. See memory `feedback_cropPaste_no_compromise.md`.

### Session 2026-05-04 (late) — host-side triage results

Investigated whether xochitl's source code or any official image-insertion API
exists, before committing the next session to more vtable-patch RE work.

- **xochitl source on GitHub: NO.** Full enumeration of the `reMarkable` GitHub
  org (107 repos across 2 API pages) found no xochitl/scene/document/canvas/
  notebook repo. Code search for `SceneItem` across the whole org returns only
  matches in their qtbase fork's accessibility test code; `xochitl` matches are
  only sibling-component *references to* xochitl (kernel resume comment;
  `epaper-qpa/epaperintegration.cpp` says "Don't change these strings. They
  must agree with xochitl."). Xochitl is closed-source and its developer page
  confirms it: "Xochitl is proprietary software, meaning it's source code is
  **not** made available." (developer.remarkable.com/documentation/xochitl)

- **SDK image-insertion API: does not exist.** The official SDK
  (developer.remarkable.com/documentation/sdk → /qt_epaper) exposes only
  standard Qt Quick types (`Window`, `Text`, `MouseArea`, `Screen`) for
  *standalone* apps that run alongside xochitl. There is no documented
  extension/IPC/plugin surface for inserting content into a notebook page.
  The community xovi monorepo (`asivery/rm-xovi-extensions`, 9 extensions:
  fileman, framebuffer-spy, palm-rejection, qt-command-executor,
  qt-resource-rebuilder, random-suspend-screen, webserver-remote,
  xovi-message-broker, xovi-setup) has no image-add primitive either —
  code search for `SceneItem|addImage|insertImage|addAsset|sticker` returns
  zero hits across the monorepo. `qt-resource-rebuilder` can replace QML
  files in the Qt resource DB but xochitl's QML surface itself doesn't
  expose an image-add API to wrap. **The "route through xochitl's existing
  image-import path" architecture pivot has no foundation.**

- **`reMarkable/depixelator` as v1.7 vector-with-color basis: not viable.**
  Header-only API is 1-bit-in (`Bitmap{ unsigned char *data; ...; bool checkBit(x,y); }`)
  and polylines-out (`vector<vector<Point{x,y}>>`). Zero color channels
  anywhere — README says "creating contours out of black&white images."
  A bucket-quantize-then-vectorize workaround (one mask per color bucket,
  one contour set per mask, emit as filled polygons) compounds two new
  blockers: (1) xochitl's vector item primitive is `SceneLineItem`
  (stroke-with-width); no filled-region primitive in the writing-tools
  surface — adding one is a fresh SceneItem-subtype RE task that lands us
  back in Problem B for a different class. (2) Anti-aliased screenshot text
  quantizes to thousands of micro-contours per bucket — not "vectorize",
  "explode the scene graph". depixelator is for sharp B/W scans, not for
  user-pasted photo/screenshot content.

- **`reMarkable/sequrerender` ("playing around with libseccomp, qt and pdfium"):
  not investigated this session.** Plausibly the prototype/precursor of the
  renderer subprocess crashing in Problem A. Worth a look from a fresh-budget
  session if the v1.6g UUID fix doesn't unblock and we need to re-attempt the
  vtable patch with a better understanding of the renderer's seccomp filter and
  IPC shape. Third priority — only after Problem B's resolution is known.

- **H3 reproducer EXECUTED on ferrari today, H3 DENIED.** See the "Run on
  device next session" block lower in this file (now annotated EXECUTED) and
  `reference/h3_reproducer_result.txt`. Renderer-crash mechanism is *not*
  generic mprotect/fork; widen to Qt/PDFium-specific hypotheses next session.

- **`build/v1.6g/cropPaste.so` deploy DEFERRED to next session.** Artifact is
  staged (1.0 MB, present at expected path). Not deployed this session because
  the deploy → paste → navigate-away → navigate-back → observe loop wants the
  slave alive for follow-up regardless of outcome (octagon-gone → re-enable
  vtable patch + iterate Problem A; octagon-persists → instrumented build to
  trace deserialize dispatch). Starting that loop with limited remaining
  tokens risks stopping mid-investigation. Next session's first action: read
  `SLAVE-V1.6.md` for the deploy/observe protocol, then deploy.

### What was tried

- **v1.6a/b — captureAreaAsImage with custom-vtable per-item swap + QML
  early-return.** QML's `imgItems.length > 0` evaluates false despite C++
  returning 1 item — `QList<shared_ptr<SceneItem>>` doesn't expose `.length`
  in QML/JS bridging the way we expected. Both paths fired, strokes
  clobbered the image item. Not the architecture's fault, just QML wiring.
- **v1.6c — same per-item vtable, QML simplified to always assign image
  to Clipboard.items.** Color rendered correctly, gate-3(a) PASS. But:
  (i) cannot select with selection tool, (ii) renders as black octagon
  on revisit (xochitl unloads page → reloads from `.rm` scenefile →
  fresh SceneImageItem with stock vtable → cache miss → dashed-rect
  placeholder). Pointer-keyed side-map dies with the original item.
- **v1.6d/e — global vtable patch (option 5).** mprotect+write slot 3
  of stock SceneImageItem vtable @ `0x16a3df0` to redirect dispatch
  through `ourGlobalPaint`. UUID-keyed side-map. Color renders, items
  selectable. **Persistence claim was unverified empirically** — the
  prior slave inferred "survives navigation" from the architecture but
  renderer-exit crashes interrupted gate 3(d) testing before persistence
  was actually observed. **But broke PDF rendering**: 7–14
  `renderer exited while waiting for a response` events in the 60s
  post-deploy window vs. 0 pre-deploy. Rolled back twice.

  **CORRECTION (2026-05-04 late, user empirical):** persistence is
  ACTUALLY broken with the global vtable patch too. User observed
  "octagon on revisit" with v1.6e color paste AND with v1.6f
  white-placeholder paste. Option 5 (global patch) gave us color and
  selectability, NOT persistence. v1.6 investigation now has two
  problems: (A) renderer-crash mechanism, (B) why persistence fails
  even with global patch.

  **Side-map keying ruled in (2026-05-04 late):** verified by code
  inspection — `customVtable.cpp:65-81` declares the side-map as
  `unordered_map<UuidKey, QImage>` keyed by 16-byte UUID copied from
  `self+0x48`. Write side at `ClipboardInjector.cpp:467-476` generates
  one v4 UUID, calls `setImageUuid` → memcpy to `obj+0x48`, then
  `registerImage(uuid, captured)` with the same bytes. In-process
  register/lookup contract is consistent. **The persistence bug is
  downstream of write/keying** — candidates: (b) `.rm` serialize doesn't
  preserve the +0x48 UUID (serializer expects asset-path + UUID field
  set via xochitl's normal image-add path, neither of which we set),
  (c) deserialized item dispatches paint via a different vtable / code
  path that bypasses our patched stock vtable, (d) xochitl drops our
  bare-construct SceneImageItem from the .rm at serialize time and
  reconstructs a placeholder on revisit. Need to inspect the .rm bytes
  on device after a paste to disambiguate.
- **v1.6f — diagnostic stub deploy** (no-op `installPaintHook`,
  `__attribute__((constructor))` + early `_xovi_construct` log lines).
  0 renderer-exits in 60s window. **Confirmed: the mprotect+write IS
  the cause.** Static init / `.so` load are innocent.

### Renderer-crash mechanism — hypothesis 1 ruled out

`xochitl_pdf_renderer` is a **separate binary**, not a re-exec of
xochitl:

| | xochitl | xochitl_pdf_renderer |
|---|---|---|
| size | 23,185,312 B | 266,352 B |
| inode | 1006 | 1007 |
| `/proc/<pid>/exe` | `/usr/bin/xochitl` | `/usr/bin/xochitl_pdf_renderer` |

So our `.rodata` patch on xochitl shouldn't share memory with renderer's
address space (different binary, different mappings). Yet renderer
crashes when patch is active. Hypothesis 1 (same-binary re-exec) DEAD.

Remaining hypotheses (post 2026-05-04 late narrowing — see "Host-side
investigation progress" below):
- **H2 (shared memory with vtable pointers): EFFECTIVELY DEAD** as of
  2026-05-04. Renderer DT_NEEDED contains no `libxochitl-*`, has zero
  `Scene*` symbols/strings, doesn't link any code that dispatches via
  SceneImageItem vtable. Even if scene-state were IPC'd by reference,
  the renderer can't dereference it.
- **H3 (kernel side effect): PRIMARY CANDIDATE.** mprotect on a
  file-backed page may trigger TLB shootdowns, COW corruption, or
  inode-cache invalidation that destabilize subsequent fork+exec. C
  reproducer built tonight (`src/repro_h3/`) — run on device next
  session.
- **H4 (output difference / Qt-specific):** widening fallback if H3
  also dies. Possible angles: signal/slot infrastructure across
  processes, PDFium font cache init, anonymous-shm regions xochitl
  creates with `xochitl_anonymous` naming visible in renderer strings.

### Host-side investigation progress (2026-05-04 late)

**Item 4 DONE — major narrowing.** Pulled `/usr/bin/xochitl_pdf_renderer`
to host (266 KB aarch64 ELF, build-id `bd45945b…`). Inspected:
- `nm -D | grep -i scene` → 0 results.
- `strings | grep -iE 'Scene[A-Z]'` → 0 results.
- `objdump -p ...` DT_NEEDED list = `libpdfium.so`, `libQt6Gui.so.6`,
  `libQt6Core.so.6`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`,
  `libc.so.6`. **No `libxochitl-*`** — the renderer is fully standalone.
- Only Qt-image symbols are `QImage` / `QImageReader` for input/output;
  no SceneItem code linked at all.
- Notable strings: `memfd_create`, `xochitl_anonymous`, Breakpad's
  `ExceptionHandler::*` — confirms IPC is anonymous-shm-based and the
  renderer has its own crash dumper.

Conclusions:
- **H2 effectively dead.** Renderer never invokes paint dispatch on a
  SceneImageItem; it doesn't link any code that knows how. Even if
  xochitl IPC'd a scene-item by reference, the renderer has no vtable
  to dereference against the pointer.
- The patch site `0x16a3df0` lives in the xochitl ELF's `.rodata`,
  which the renderer never maps (different binary, fork+exec wipes
  parent address space).
- Items 1-3 (proc/maps diff, lsof) would only confirm the same — they
  can be skipped unless H3 also dies.

**Items 1-3 SKIPPED as low-value** given item 4's DT_NEEDED finding.

**Item 5 — H3 reproducer BUILT (host-side, ready to deploy next session).**
Source: `src/repro_h3/repro_h3_mprotect_fork.c` (~200 lines incl. comments).
Cross-compiled binary: `src/repro_h3/repro_h3_mprotect_fork` (aarch64
ELF, sha256 `ec5482c9f01e7fd5bb9270f789842cbad9302d5fc6484e5e59e7a82043dfafbc`).

Test design (full rationale in source):
1. `open("/proc/self/exe")` + `mmap(MAP_PRIVATE | PROT_READ, 1 page)` —
   mirrors how the dynamic linker maps xochitl's `.rodata`.
2. mprotect R → RW, write 8 bytes, mprotect back to R — same shape as
   our v1.6e vtable patch.
3. fork+execl `/bin/echo` × 32 iterations. Capture exit-status histogram.
4. Two passes: control (no mprotect dance) then patched. Compare crash
   counts. Verdict: control=0 crashes && patched>0 → H3 confirmed.
   Both 0 → H3 denied → widen to xochitl/Qt-specific mechanisms.

Run on device next session:
```
scp src/repro_h3/repro_h3_mprotect_fork root@10.11.99.1:/tmp/
ssh root@10.11.99.1 chmod +x /tmp/repro_h3_mprotect_fork
ssh root@10.11.99.1 /tmp/repro_h3_mprotect_fork
```
Expected runtime <1 s. No xochitl interaction — runs as a standalone
process. Safe to run alongside live xochitl.

**EXECUTED 2026-05-04 on ferrari — H3 DENIED.** Both passes ran 32 fork+exec
iterations cleanly: control ok=32 nonzero=0 crashes=0, patched (post-mprotect)
ok=32 nonzero=0 crashes=0. mprotect(file-backed page, R→RW→R) does NOT poison
subsequent fork+execve in the same process. The renderer-crash mechanism
(v1.6d killed the PDF renderer) is **not** a generic mprotect/fork interaction —
it's xochitl-/Qt-/PDFium-specific and the bare-C reproducer was insufficient
to reach it. Verbatim stdout + caveats in `reference/h3_reproducer_result.txt`.

**Next-session H4 hypothesis space (now that H3 is out):** Qt internal mutex
held across fork (matches `gotcha_xovi_global_hook_fork.md` — the patched-vtable
+ mutex-deadlock pattern); PDFium global state torn by parent-side patch;
vtable patch racing with the renderer subprocess's vtable lookup; signal-handler
or sigaction state leaking into the child; per-thread state (TLS/QThreadStorage)
left inconsistent across fork. None of these exist in a bare C reproducer,
which is why H3 (which mimicked only the mprotect shape) couldn't trip them.

### Problem B (persistence) — ruled out (b) and (d), confirmed (c) live

Inspection of `.rm` bytes (host-side, no deploy) on
`b0c7f063…/8f9dea6a…rm` (the file modified at 23:39 matching the
2026-05-04 paste timestamped in journalctl):
- File header is `reMarkable .lines file, version=6` (modern v6 binary).
- Searched the 75 686-byte file for every plausible byte-permutation of
  the captured UUID (`559578b2-…`). **Exactly one** 16-byte window is a
  byte-multiset permutation of our UUID, at offset `0x1272f`:
  ```
  ours (memcpy bytes): 55 95 78 b2 51 b0 4c f9 9e 7d ab 4d e5 08 87 07
  file @ 0x1272f:      07 87 08 e5 4d ab 7d 9e 4c f9 51 b0 55 95 78 b2
  ```
  The transform is exactly `QUuid::toRfc4122()` byte-reversed —
  equivalent to writing the QUuid's "as-uint128" value in
  little-endian. Deterministic and lossless; round-trip preserves the
  in-memory bytes at +0x48 exactly.

- **(d) REFUTED.** A SceneImageItem-equivalent entry containing our
  UUID IS in the .rm. xochitl did NOT drop our bare-construct item.
- **(b) REFUTED.** Serializer preserves the UUID — it's not rewritten
  or regenerated.
- **(c) LIVE.** Persistence failure must be in the deserialize-and-paint
  pipeline. Need an instrumented build (or a deploy + journalctl) to
  observe whether `ourGlobalPaint` is called on the deserialized item.

### Sub-finding: QUuid struct-field byte reinterpretation (FIXED in v1.6g)

Our v1.6f-and-earlier UUID generation in `ClipboardInjector.cpp` read
16 bytes from `/dev/urandom` and set the v4 version + variant bits in
**ASCII byte order**: `uuid[6]` (version nibble) and `uuid[8]` (variant
top bits). But xochitl interprets `+0x48` as a `QUuid` struct
`{u32 data1; u16 data2; u16 data3; u8 data4[8]}`, which on LE aarch64
reads:
- `data1` = LE uint32 from bytes 0..3
- `data2` = LE uint16 from bytes 4..5
- `data3` = LE uint16 from bytes 6..7  ← BE-display high nibble = HIGH
  nibble of byte 7, NOT byte 6
- `data4` = bytes 8..15 as-is

So our manual `uuid[6] = (uuid[6] & 0x0F) | 0x40` sets the wrong nibble:
xochitl's `QUuid::toString()` for the example above reports
`b2789555-b051-f94c-…` (version `f`, NOT `4`). Non-conforming v4.

This is a plausible cause of (c): if any deserialize-side code path
validates QUuid versions, our items get rejected/placeholdered without
us seeing it. **Cheap to fix; cheap to test.**

**Fix in v1.6g** (`src/clipboard-injector/ClipboardInjector.cpp`):
replaced the manual `/dev/urandom` + bit-twiddling with
`QUuid::createUuid()` + `memcpy(uuid, &quuid, 16)`. Now whatever Qt
generates is what xochitl reads back. Logging now uses
`quuid.toString(QUuid::WithoutBraces)` plus `version()` / `variant()`
for ground-truth observability.

v1.6g binary: `build/v1.6g/cropPaste.so`, sha256
`58110556140230565ce09ecd2c8b614c5df12a22dbbe08baf761748169238745`.
Same as `src/clipboard-injector/clipboard-injector.so` (overwritten by
the build). `installPaintHook` is still STUBBED (carried over from
v1.6f-diagnostic) — v1.6g is intentionally a v1.6f + UUID fix only,
NOT a re-enabled vtable patch. Persistence will not visibly improve in
v1.6g unless paired with the patch re-enable, but the .rm round-trip
will now contain a conforming v4 UUID, which is the actual test.

### Next session plan (conditional branching)

**Tonight DONE (host-side, no deploys):**
- Device state corrected; v1.5 was never restored (still on v1.6f).
- Side-map keying ruled OK (UUID-keyed not pointer-keyed).
- .rm UUID round-trip verified — (b)/(d) ruled out, (c) live.
- Renderer DT_NEEDED inspected — H2 effectively dead, H3 primary.
- v1.6g built with QUuid fix — sha `58110556…`.
- H3 reproducer cross-compiled — sha `ec5482c9…`.

**Two parallel deploy-tracks for next session:**

Track A — Problem A (renderer crash):
1. Deploy `repro_h3_mprotect_fork` to `/tmp/` on device, run.
2. **If H3 CONFIRMED** (control=0 crashes, patched>0): the kernel has a
   real interaction between mprotect on file-backed pages and fork+exec
   timing. Fix space: lazy install (mprotect-after-first-renderer-spawn),
   PLT/GOT redirection instead of vtable patching, or ship an external
   .so the linker maps RW from the start.
3. **If H3 DENIED** (both passes clean): kernel is innocent; widen to H4
   xochitl/Qt-specific. Candidates worth probing: signal-slot
   infrastructure across processes, PDFium font/cache initialization,
   shared anonymous-shm regions xochitl creates with `xochitl_anonymous`
   naming.

Track B — Problem B (persistence) — independent of Track A:
1. Deploy `build/v1.6g/cropPaste.so` to device. Restart xochitl. Confirm
   load and that `installPaintHook` STUBBED log fires (no patch active,
   no renderer regression risk this deploy).
2. User pastes once into a notebook page. `journalctl` should show the
   new heavy-log line with QUuid printed via `toString()` —
   verify `version=4 variant=2` (RFC 4122 / DCE).
3. Pull the destination `.rm` file. Re-run the byte-permutation hunt
   from tonight; verify the UUID is present AND that QUuid's
   interpretation of those bytes is now a proper v4.
4. **Without re-enabling the patch**, the user can't observe color or
   persistence directly (paint hook is stubbed). Still useful — proves
   the .rm round-trip carries a conforming UUID.
5. **If Track A produces a workable A-fix**, build v1.6h = v1.6g +
   re-enabled `installPaintHook` (with whatever H3 mitigation Track A
   pointed at). Deploy. User pastes + navigates back. Two outcomes:
   - **Octagon-on-revisit GOES AWAY**: UUID conformance was the (c)
     cause. Persistence solved trivially. Ship v1.6h pending PDF-renderer
     trip-wire (still 0 renderer-exits in 60s window).
   - **Octagon persists**: (c) is something else (deserializer uses a
     different vtable, or paint goes through a different code path).
     Need an instrumented build that logs every paint dispatch + the
     vtable address seen, deploy under user supervision, observe a
     navigate-away-and-back cycle. Will narrow to "did our hook fire on
     the deserialized item or not".

**Dependencies:** Track B step 5 (v1.6h) requires Track A to land
first; otherwise re-enabling the patch reintroduces the renderer crash.
Track B steps 1-4 can run independently of Track A — they don't enable
the patch.

### v1.6 / Phase-1 historical hypothesis-queue (superseded by tonight)

(Kept for archaeology; do not redo.)

If host-side narrows the search: bounded 4-variant `installPaintHook`
test (no-op / mprotect-only / write-to-scratch-on-page / write-to-vtable-slot)
behind a runtime flag, deploy each variant, count renderer-exits.
One deploy session, four data points. Don't run this until host-side
has narrowed the search.

### Plan B if global-patch is structurally dead

Deserializer-hook + per-item-vtable + on-disk PNG store. RE the
SceneImageItem deserializer entry point (the function reconstructing
items from `.rm` scenefile bytes). On reconstruction: if UUID at
this+0x48 matches one of ours (marker pattern), install our custom
vtable on the fresh item + load PNG from `/home/root/xovi/exthome/cropPaste/<uuid>.png`.
Sidesteps shared memory entirely. Survives navigation AND xochitl
restart. The cannot-select bug from v1.6c needs separate treatment
(probably trace which vtable slot xochitl's selection tool uses for
hit-testing, override that slot too with a stock-passthrough wrapper).

### v1.6 RE deltas to fold into MASTER.md proper next session

(Not yet integrated into the rest of this doc — placed here as a TODO.)

- **paint() prototype** at `vt[3]` @ `0xe83630`: signature is
  `void paint(SceneImageItem* this, RenderContext* ctx)`. 2 args,
  no FP, no sret. ctx may be null (early ret). ctx+0xd0 is read by
  helper `0xec5ad0` (which tail-calls `0xf37560`) to resolve a
  `QPainter*`. ctx+0x120 is the cache root pointer; reads vary
  per-render-context (not session-stable, killed option 1).
- **dtor slot mapping** (Itanium ABI):
  - `vt[0] @ 0xe83d30` — complete destructor (D1), frees inline-bytes
    vector, does NOT free `this`
  - `vt[1] @ 0xe83d60` — deleting destructor (D0), frees vector AND
    calls `::operator delete(this, 216)`. Tail-calls `_ZdlPvm` with
    size hardcoded as `mov x1, #0xd8` in body, NOT passed as arg —
    confirms GCC-typical no-extra-arg form.
- **clone vt[2] @ 0xe83bb0 hardcodes stock vtable into clone result**.
  `adrp+add` at `0xe83c7c-90` writes `0x16a3df0` to the cloned object's
  vtable slot. Clone wrappers must re-patch vtable on the result if
  using per-item-vtable approach. (Not relevant for global-patch,
  irrelevant for stock items, but pinned for reference.)
- **Itanium ABI typeinfo header** is at vtable_addr - 16 (offset_to_top
  = 0 for non-virtual base, int64) and vtable_addr - 8 (typeinfo* =
  `0x16a3dc8`, matches MASTER.md). Strong sanity invariant:
  `*(uint64_t*)(vtable_addr - 8) == 0x16a3dc8`.
- **Field offset rename**: `this+0x60` is the **destination QRectF on
  the canvas** (where the image gets placed on the page), NOT a
  sub-rect of the source image. Older comments call it `source_bounds`
  misleadingly. The QRectF for the source sub-rect is constructed on
  the stack at paint time as `(0, 0, image.width, image.height)`. Rename
  to `destBounds` / `boundsOnPage` in any code touched.
- **Qt `slots` macro gotcha**: `slots`, `signals`, `emit`, `foreach` are
  empty preprocessor defines in `<QObject>`. Don't name local variables
  any of those — produces a misleading GCC parse error cascade
  ("expected unqualified-id before '=' token" + bogus lambda-capture).
  See memory `qt_slots_macro_gotcha.md`.
- **XOVI multi-process hazard**: xochitl's worker subprocesses (PDF
  renderer, etc.) inherit LD_PRELOAD → load xovi.so → load our
  extension. `__attribute__((constructor))` fires in all of them.
  `_xovi_construct` only fires in main (xovi.so's runtime decides).
  Any global hook + threading state needs a PID guard against
  fork-only inheritance — see memory `gotcha_xovi_global_hook_fork.md`.
  **The renderer-crash mechanism is something OTHER than this guard's
  scope, since v1.6e had the guard and still crashed renderer.**

## Status — v1.5 SHIPPED (2026-05-03, stroke-trace + thinning), color raster deferred to v1.6

The user-facing tool ships as a single `crop` button on the editingTools toolbar.
End-to-end flow works: tap crop → drag rect → release → switch to selection
tool → paste in destination notebook. Renders as **monochrome stroke-trace**
of the captured region (PDF + annotations + everything visible at FB-read
time, edge-detected and emitted as `SceneLineItem`s).

Color raster via `SceneImageItem` is *captured* end-to-end (PNG byte-perfect
including color highlights — verified by SCP'ing the dbg PNGs back to host),
but xochitl's renderer requires populating an image cache the **cache-insert
function for which we did not locate**. Live `SceneImageItem` rendering
falls through to a `setPen(DashLine)` + `drawRects(source_bounds, 1)`
placeholder (the "octagon"/"thin outline" the user observed across v1.5b–e).

### v1.5 — what shipped

- **Single button** on the toolbar (the `crop` icon). The Phase-1 dev buttons
  (`clipboardPasteButton` duplicate-icon, `imageTestButton` shaded-circle)
  were stripped — they had no end-user purpose post-v1.
- **Timer 50ms defer** between `cropOverlay.visible = false` and the
  `captureArea` call. Gets rid of the v1 cropOverlay-baked-in border
  artifact (the "octagon frame" from Phase 1 retro). 50ms is well above
  the eink composit latency. Non-blocking (vs `sleepMs` which froze the
  event loop).
  - Implemented as a sibling Timer inside the cropOverlay Item in the qmd.
    See `clipboard-injector.qml-diff` `captureDeferTimer` block.
  - Decision rationale: `Window.frameSwapped` would have been more correct
    but Toolbar.qml doesn't import `QtQuick.Window`, so attached-property
    access would have needed an additional qmldiff IMPORT injection. Timer
    achieves the same observable behavior with strictly less risk.
- **v1.5f thinning pass** on the stroke-trace pipeline:
  - Dropped the `gaussianBlur3x3` step before Laplacian — the blur was
    the source of the user's "blurry" complaint (input softening spreads
    the LoG response). Laplacian now runs on raw grayscale.
  - Edge threshold `>= 96` filter on the Laplacian output to drop faint
    halo pixels (was: any non-zero edge → stroke; now: only strong edges
    → stroke). Significant reduction in stroke count + perceived weight.
  - Per-LinePoint width `25 → 8`. Thinner rendered strokes.
  - `gaussianBlur3x3` kept in source as `[[maybe_unused]] static` for
    quick A/B if a future capture proves the threshold is too aggressive.

### v1.5 — known limitations

- **Monochrome only.** Color is captured (RGB framebuffer wrap → QImage
  → PNG color type 2 verified) but the stroke-trace path quantizes to
  black-only. Restoring color requires either (a) the SceneImageItem
  cache-insert path (see v1.6 roadmap below) or (b) emitting per-pixel
  colored `SceneLineItem`s (would generate 100k+ items per capture; not
  attempted).
- **Edge-band thickening.** LoG inherently produces 1–2 px edges on
  each side of every transition. The thinning pass mitigates but doesn't
  eliminate this; thick strokes in the source will still trace as
  parallel double-lines. A skeletonization pass would help.
- **Pasted output is strokes, not images.** Therefore: (i) responds to
  the stroke eraser (not the selection-lasso → delete used by stock
  images); (ii) sits on whatever layer is current at paste time (no
  per-image layer isolation — Deliverable B was descoped).

### v1.5 — RE findings to carry into v1.6 (do NOT re-investigate)

The SceneImageItem render path was decoded in detail across v1.5a–e. Carry
these forward:

- **Paint function** = `vt[3]` @ `0xe83630`. Reads the global render context
  via helper `0xec5ad0` (which tail-calls `0xf37560`, an
  `add x0, x0, #0xd0` then jump). Reads cache root from
  `[QPainter+0x120]`. On valid cache + valid render context, hashes the
  16-byte UUID at `this+0x48` via `_Z5qHashRK5QUuidm`, walks a
  144-byte-bucket / 104-byte-entry hash table, and on hit calls
  `0xe10760` to populate a stack `QImage`. On miss, falls through to
  `setBrush(NoBrush) + setPen(DashLine) + drawRects(source_bounds, 1)`
  — the dotted-rect placeholder.
- **Image cache layout:** hash table indexed by `qHash(QUuid)`,
  `0xff` = empty bucket sentinel. Bucket entries 104 bytes each, with
  the UUID stored at offset 128 within each bucket array entry.
- **vt[2]** = clone (copies item + deep-copies the vector at 0x30,
  shallow-copies the pointer pair at 0x10/0x18, copies UUID at 0x48,
  copies bounds and transform).
- **Negative results** (don't repeat):
  - The `std::vector<uint8_t>` we populate at offset 0x30 is **never
    read by paint**. Setting it does nothing. v1's "200×200 black square"
    that we thought was a working PNG render was actually the 200×200
    dashed-rect placeholder at small scale — visually misleading.
  - The `tagged_union<Bounds, basic_string<char>>` typeinfo string is from
    `expected<T,E>` accessor mangling for *error handling* code paths,
    NOT the actual storage discriminator of SceneImageItem. There is no
    in-struct asset path string.
  - The byte at offset `0x28` (returned by `vt[6]` @ `0xe83900`) is a
    discriminator of some kind (vt[6] returns `(byte != 0) ? byte : 2`),
    but cycling values 0..7 produced no rendering change. Whatever it
    discriminates, it's not the cache vs inline-bytes branch.
  - Writing the captured PNG to `/home/root/.local/share/remarkable/xochitl/<doc-uuid>.thumbnails/<image-uuid>.png`
    (shotgunned to all `.thumbnails` dirs on disk) does NOT trigger an
    auto-load. Either the cache isn't populated from `.thumbnails` at
    all, or the load only fires through a code path we haven't reached
    (e.g., notebook reload via the `.rm` deserializer).
- **11 OTHER call sites** of `_Z5qHashRK5QUuidm` exist in the binary at:
  `0xe126e8, 0xe12ab8, 0xe16590, 0xe16f1c, 0xe63700, 0xe6f1d0, 0xe70d14,`
  `0xe747a4, 0xed4b6c, 0xf1f3cc, 0xf2449c`. At least one is the
  cache-insert function. Distinguishing inserts from lookups requires
  tracing each surrounding function for *writes* to the bucket array
  on miss/find (lookups walk read-only). Surveyed five so far; all five
  match the lookup pattern. The remaining six are the next RE target.

### v1.6 roadmap — color raster

Two viable paths, ranked by preference:

1. **`.rm` scenefile mutation (in-place, no restart)** — user's own
   suggestion at the close of v1.5. Approach:
   - At paste time, find the destination notebook's directory under
     `/home/root/.local/share/remarkable/xochitl/<doc-uuid>/`.
   - Write the captured PNG to a known location xochitl will resolve
     by UUID (`<doc-uuid>.thumbnails/<image-uuid>.png` is the empirically
     observed pattern).
   - Append a `SceneImageItem` entry to the destination page's `.rm`
     scenefile, referencing the UUID. Format documented in the
     `rmscene` Python library.
   - xochitl has a file watcher on its scenefile dir (used by the sync
     pipeline). Modifying the `.rm` should trigger a re-render WITHOUT a
     process restart (this is what the user explicitly asked for).
   - **Catch:** finding the destination doc-uuid + page-uuid at paste
     time requires either QML-side discovery (read `documentView.documentId`
     or similar) or hooking the paste path. Not insurmountable.

2. **Cache-insert function call** — the "proper" fix. Find which of the
   remaining 6 `qHash(QUuid)` callers is the insert; call it directly
   with `(uuid, our_QImage)`. Cleanest result, but bounded by deep RE
   time.

**Do not start with:** kind-byte cycling, on-disk thumbnails shotgun,
`Window.frameSwapped` (Toolbar.qml import scope was the issue, not the
signal itself). All ruled out empirically in v1.5.

## Status — Phase 2 v1 SHIPPED (2026-05-03), v1.5 next

Phase 2 image-paste mechanism is live on ferrari. Test:
`Clipboard.items = pasteTestImage()` with a hardcoded 1×1 black PNG +
scale-200 transform → user pastes via the **selection tool** → 200×200
black square renders on the page → xochitl persists the page to disk
(`rm.scenefile Scene written to: …/<doc-uuid>/<page-uuid>.rm.tmp`),
confirming local round-trip. Image item correctly does NOT respond to
the stroke-eraser (it's not a stroke; users delete via selection-tool
lasso → delete, same as any built-in image item).

### Phase 2 v1 — proven facts (don't re-verify)

- **Layout assertions held verbatim.** Default-init hexdump from the
  factory (vtable=0x16a3df0, type_tag=7, all-zeros except identity
  QTransform at 0x80..0xc8) matched my predicted layout exactly. No
  std::string surprise at 0x10..0x30 — that range is all zeros in
  default state, consistent with the tagged_union sitting in its
  "Bounds" variant with default-zero contents. The basic_string
  variant only fires if the discriminator is set elsewhere (we didn't
  set it; the factory leaves it at zero; xochitl's render path was
  fine with that).
- **Mutations all landed where predicted.** `setSourceBoundsF @ 0x60`,
  `setTransformScale @ 0x80` (with m_type cache update at 0xc8 — bits
  0-9 set to TxScale=2, bits 10-15 preserved), `setInlineBytes`
  populating the std::vector triplet at 0x30/0x38/0x40 with
  `::operator new(67)`-allocated PNG bytes. Post-mutation hexdump
  showed exactly what we wrote and nothing else moved.
- **AArch64 PCS gotcha:** the factory at `0xe58af0` returns
  `std::shared_ptr<SceneItem>`. PCS forces non-trivially-copyable
  return types via x8 sret regardless of size — std::shared_ptr is
  16 B but has a non-trivial destructor + copy ctor, so it goes via
  x8, NOT x0/x1. First C++ declaration was
  `void(int, std::shared_ptr*)` which puts the out-pointer in x1;
  factory read garbage from x8 and SIGSEGV'd. Correct declaration
  is `std::shared_ptr<SceneItem>(*)(int)` returning by value — GCC
  sets up x8 sret automatically. Codified in `factoryCreate`.
- **Paste-tool gating:** xochitl gates clipboard-paste by active tool.
  `SceneLineItem`s paste via the marker tool (Phase 1 path). 
  `SceneImageItem`s paste via the **selection tool** — same as any
  built-in image item. The user must switch tools to drop the image.
  Phase 3 (selection-menu integration) makes this the natural UX.

### Phase 2 v1 — known limitations to fix in v1.5

- `areaCaptureButton` still routes through the Phase 1 stroke-trace
  pipeline (`captureArea` → BGRA→Gray → blur → Laplacian → JSON of
  strokes). Real screenshot capture produces the same Phase 1
  artifacts the user noticed at first deploy: thickened text (LoG
  edge bands), no colour (grayscale conversion before edge detect),
  octagon-shaped frame around captured region (cropOverlay's white
  selection rectangle baked into the framebuffer at capture time
  via the qmd-redraw race). v1.5 task: route `captureArea` through
  the SceneImageItem pipeline instead, encoding the cropped region
  as PNG. Cropoverlay race fix folds in here (Window.frameSwapped
  preferred — see Decisions and rationale).

- Toolbar icon `imageTestButton` renders as a generic shaded circle
  because `qrc:/ark/icons/save` doesn't exist in the rMPP icon set.
  User explicitly opted to keep the shaded circle for now (it
  visually distinguishes "experimental modded button" from stock
  UI). Real-icon swap to `qrc:/ark/icons/paste` is staged in
  `clipboard-injector.qml-diff` but NOT deployed; revert if
  re-deploying without the icon decision. Custom icons via
  `QResource::registerResource` are possible but deferred to v2.

## Status — Phase 1 SHIPPED (2026-05-03), Phase 2 starting

The upstream rM2 stroke-trace pipeline ports to rMPP / aarch64
end-to-end. Verified on ferrari, two captures, 23 470 SceneLineItems
constructed by us with our struct layout + scavenged vtable, rendered
by xochitl's clipboard, no SIGSEGV. PID stayed on 54093 throughout.

User confirmed at deploy: "it's working pretty well, but the text is
being thickened. … why is there a border?". Both artifacts are
inherent to the upstream's algorithm and disappear in Phase 2 when we
swap stroke output for `SceneImageItem` raster paste — see "Decisions
and rationale" → 2026-05-03 below.

### Per-device pinned constants (3.26.0.68, both devices)

Ferrari and porsche binaries are **different builds** of the same firmware
release. Same `/etc/version` (`20260310084634`) but different SDK targets,
different sizes, different SHA256s. Layout/types match across both (RTTI
strings identical: `14SceneImageItem`, `13SceneLineItem`, `13ScenePathItem`).
Only addresses differ.

| Constant                       | ferrari (rMPP 11.8")   | porsche (rMPP Move 7.3") |
| ------------------------------ | ---------------------- | ------------------------ |
| Codename / SDK target          | `ferrari` / cortexa53-crypto | `chiappa` / cortexa55      |
| Binary size                    | 23,185,312 B            | 22,004,728 B              |
| SHA256                         | `4dcee4ad…3d29ee`       | `998c8f3c…5b66bc1`        |
| GNU BuildID                    | `fdf1d585…0c8382`       | `12313e7b…f2829`          |
| `SceneImageItem` vtable        | `0x16a3df0`             | `0x1572900`               |
| `SceneImageItem` typeinfo      | `0x16a3dc8`             | `0x15728d8`               |
| `SceneImageItem` paint (vt[3]) | `0xe83630`              | `0xca2180`                |
| `SceneLineItem` vtable         | `0x16a4338`             | `0x1572e48`               |
| Image factory address          | `0xe58af0`              | `0xc77740`                |
| Factory prologue (4-byte LE)   | `0xd503233f, 0xa9bd7bfd` | `0xd503233f, 0xa9bd7bfd` |
| ctx → QPainter* helper         | `0xec5ad0`              | `0xce4550`                |

Porsche addresses RE'd 2026-05-04 — see "## 2026-05-04 — porsche RE complete"
below for the disassembly walk. Per-device dispatch is now wired in the
C++ side (`firmware_addrs.{hpp,cpp}`, hostname-based selection), so the
single `cropPaste.so` builds for both ferrari and porsche.

Hardware deltas (informational, not directly relevant to this extension):
ferrari supports the rMPP Type Folio keyboard ("Seabird" / `RefineKeyLight`);
porsche does not. Different I2C buses (1 vs 0). Different battery IC
(`g2194-regulator` vs `max77818_battery`). Documented in
`reference/diff-ferrari-porsche.md` for reference.

### Known firmware constants (3.26.0.68, ferrari xochitl)

- `SceneLineItem` vtable address (logged from device runtime):
  **0x16a4338**. Use as a navigation anchor when RE'ing other
  scene-item vtables — they live in the same `.data.rel.ro` region
  in libxochitl, typically within a few KB of each other.
- Framebuffer (ferrari, portrait): `1620 × 2160`, BGRA, bpl `6528`,
  type=2 per `framebuffer-spy`.
- `SceneLineItem` aarch64 layout from runtime offsetof prints:
  `sizeof = 176 (0xb0)`, `line @ 0x48`, `unk_xc @ 0x10`,
  `unk_x78 @ 0xa0`. Upstream's struct (originally RE'd against rM2
  armv7) ports verbatim — all four `tryCast` invariants
  (`unk_xc==0`, `unk_xe==0`, `unk_x78==1`,
  `unk_x20∈{0, 2 with unk_x21==2}`) hold on rMPP without
  modification.
- `Line` aarch64 sizeof = `0x58 = 88` bytes (Qt6 `QList<T>` = 24 B,
  not 8 B as a naïve calc would assume — `QArrayDataPointer` is
  three pointers).
- `SceneImageItem` vtable address (RE'd from typeinfo walk):
  **0x16a3df0**, sizeof = **0xd8 = 216 bytes**. Single-inheritance
  from `9SceneItem` via `__si_class_type_info` (sibling of
  `SceneLineItem`). Typeinfo struct @ 0x16a3dc8, name string
  ("14SceneImageItem") @ 0x16a3db0. SceneLineItem and SceneImageItem
  vtables are 1 352 bytes apart in `.rodata`.
- `SceneImageItem` layout sketch from disassembling vt[0]/vt[1]
  (destructors) + the generic factory at 0xe58af0 case 15:
  ```
  0x00  void* vtable
  0x08  uint8_t type_tag        // = 7 in default init
  0x10  ptr (probably std::string _M_p)        ← 0x10..0x30 likely
  0x18  size_t                                    a basic_string<char>
  0x20  8 B (string SSO buf or other)
  0x28  uint16_t
  0x30  T* vec_begin            // std::vector<T>; destructor frees
  0x38  T* vec_end                  (alloc_end - begin) bytes here
  0x40  T* vec_end_of_storage
  0x48  3 × 8 B unknown
  0x60  QRectF source_bounds (32 B)
  0x80  QTransform transform (72 B; default = 3×3 identity loaded
        from template @ 0x16a3a00)
  0xc8  uint16_t (QTransform m_type cache, low 10 bits cleared)
  0xd0  8 B (zeroed)
  ```
  **Layout NOT yet locked with `static_assert(offsetof(...))` —
  needs further RE before construct-from-scratch is safe. The
  pieces I'm least confident about are 0x10..0x30 (is it really a
  std::string? what holds the asset path?) and 0x48..0x60 (24 B of
  unknown role).**
- **Generic scene-item factory** discovered at runtime address
  `0xe58af0`. Signature `void create(int type_tag, T* sret)` where
  `sret` is a 16-byte slot for `std::shared_ptr<SceneItemBase>`.
  `case 4` → some other scene type (sizeof 0x70 = 112). `case 15` →
  SceneImageItem. The function does the make_shared inplace alloc
  (232 B = 16 ctrl block + 216 SceneImageItem), writes the
  SceneImageItem vtable, type_tag = 7, default identity QTransform
  from template, zeroes the rest. Returns a fresh empty
  SceneImageItem ready for caller to mutate.
- **Generic factory NOT in `.dynsym`** — cannot resolve by
  `import?` in `.xovi`. Only callable via hardcoded address (pinned
  to firmware 3.26.0.68).
- **Factory prologue signature** at `0xe58af0` (verified, locked):
    - `+0`: `0xd503233f`  (paciasp)
    - `+4`: `0xa9bd7bfd`  (stp x29, x30, [sp, #-48]!)
  Both are read at runtime by `SceneImageItem::checkFactorySignature`
  before each call; mismatch logs the actual bytes and returns null
  shared_ptr (no jump into garbage). When firmware bumps and the
  address moves, journalctl shows the new bytes in one line.
- **Destructor allocator** for the inline-bytes vector at
  offset 0x30: deleting destructor (vt[1] @ 0xe83d60) tail-calls
  `_ZdlPvm` (= `::operator delete(void*, size_t)`), passing
  `(end_of_storage - begin)` as the size. **Match with
  `::operator new(size_t)` for the PNG buffer**, NOT
  `new uint8_t[size]` (which would route through
  `::operator delete[]` and corrupt the heap on document close).
  Codified in `rm_SceneImageItem.cpp::setInlineBytes`.

### Phase 1 progress

### Phase 1 progress

- 1a ✓ — upstream `clipboard-injector/` vendored into
  `src/clipboard-injector/`. Original Unlicense shipped as
  `UPSTREAM-LICENSE`.
- 1b ✓ — `.pro` updated for aarch64: dropped
  `-mfpu=neon -mfloat-abi=hard`; toolchain provides `-mcpu=cortex-a53`
  flags. Per-extension `Dockerfile` removed (top-level Dockerfile is
  the canonical build env).
- 1c **in progress, validation deferred to 1f** — the upstream's
  `SceneLineItem` layout was reverse-engineered against rM2 (armv7,
  4-byte pointers). On rMPP/aarch64 every void* doubles to 8 bytes,
  shifting field offsets. We:
    1. Trust the upstream struct as the best guess for xochitl 3.26.
    2. Added `static_assert(offsetof(...))` for every named field
       on `__aarch64__` to lock the C++ compiler's layout to our
       hand calc (sizeof(SceneItem)=8, line@0x48, etc.). Build
       passes — our struct lays out the way we expect.
    3. Added runtime observability: `setupVtable()` logs
       `sizeof+offsets+vtable`; `tryCast()` logs each invariant
       mismatch with its actual value.
  Confirmed via strings on `xochitl-3.26.0.68`: both `13SceneLineItem`
  and `13ScenePathItem` typeinfo present (the rename is *additive*
  on rMPP, not a replacement). All upstream `SceneController` QML
  factories used by the qmd survive the rename: `addDrawingLine`,
  `selectWithLine`, `clearSelectedItems`, `deleteSelectedItems`,
  `cloneSelectedItems` all in the binary.
  **If the runtime invariants fail at first deploy, escalate to
  Ghidra-RE the constructor.**
- 1d ✓ — `clipboard-injector.qmd` round-trips byte-identically
  through our hashtab (decompile then re-hash → diff -q is empty).
  No unmapped `[[number]]` blocks. Plain-name source committed at
  `src/clipboard-injector/clipboard-injector.qml-diff` for editing.
  TRAVERSE selectors `FocusScope > Item#toolbar > GridLayout`
  + `LOCATE AFTER Repeater[.model='toolbarProvider.editingTools']`
  match rMPP `Toolbar.qml` exactly (saved at
  `reference/qml/Toolbar.qml`, 669 lines).
- 1e ✓ — `ClipboardInjector.cpp` already reads framebuffer
  dimensions from `getFramebufferConfig` at runtime; only an
  out-of-date comment hardcoded `1404×1872`. Updated comment to
  list ferrari (1620×2160), porsche (960×1696), legacy rM2.
- 1f pending — needs a deploy + smoke-test gate from the parent
  before pushing to device.

### Phase 1 build artefacts in hand

```
src/clipboard-injector/clipboard-injector.so   721 360 bytes
  ELF 64-bit LSB shared object, ARM aarch64
  exports: _xovi_construct, _xovi_depconstruct, registerQmldiff
  static_asserts pass: sizeof(SceneItem)==8, line@0x48, etc.
```

### Adjustments to upstream code (Phase 1)

- `ClipboardInjector.cpp`: `%d` → `%lld` on `qsizetype` printf
  args (aarch64 `qsizetype` is `long long int`).
- `rm_SceneLineItem.hpp`: aarch64 `static_assert` block at bottom.
- `rm_SceneLineItem.cpp`: setupVtable now logs sizeof+offsets;
  tryCast logs each invariant mismatch.
- `clipboard-injector.pro`: dropped armv7 NEON flags.
- Comment-only update to `captureArea` doc block.

## Status — Phase 0 COMPLETE (2026-05-03)

All Phase 0 tasks done. Hello-world XOVI extension loads cleanly on
ferrari (USB, `10.11.99.1`).

Confirmed load sequence from `journalctl -u xochitl`:

```
xochitl[51624]: [cropPaste] hello-world extension loaded
xochitl[51624]: [rm-shot]: Extension loaded
xochitl[51624]: [qmldiff]: Set system version to 3.26.0.68
xochitl[51624]: [qmldiff]: Hashtab loaded! Cached 20017 entries
…then 26 .qmd files load cleanly…
```

Post-load device state: PID 51624, `State: S (sleeping)` (normal Qt
event-loop idle), `VmRSS 296 MB`, uptime 58 s at check time, no
new errors in the journal mentioning cropPaste / our extension.
The deploy did NOT introduce any regression visible in the journal.

Pre-existing benign noise: `SceneViewGestures.qml:75 JSON.parse`
parse error fires every 10 s on the previous PID (51082) — that's
from some *other* installed extension's qmd, not ours, and was
present before our deploy.

- 0a ✓ — `~/src/xovi` cloned (HEAD `0c8d526`); `~/src/qmldiff` and
  `~/src/rm-xovi-extensions` already on disk and verified.
- 0b ✓ — XOVI README + external.h + main.c, every file in upstream
  `clipboard-injector/`, freeColour MASTER + qmldiff workflow,
  freeColour Makefile + compile-qmd.sh, framebuffer-spy main.c all read.
- 0c ✓ — `Dockerfile` written. Pinned to
  `eeems/remarkable-toolchain:5.6.75-rmpp` (aarch64 codex SDK for
  ferrari/rMPP); `--platform linux/amd64` to run the x86_64 host
  binaries under Rosetta on Apple Silicon. `XOVI_REPO=/opt/xovi`.
  `docker build` succeeds. Toolchain provides `CC =
  aarch64-remarkable-linux-gcc -mcpu=cortex-a53+crc+crypto
  -mbranch-protection=standard --sysroot=…/cortexa53-crypto-remarkable-linux`.
- 0d **build done, deploy pending** — `src/hello/hello.so`
  (76 KB, `ELF 64-bit LSB shared object, ARM aarch64`,
  exports `_xovi_construct` and `_xovi_depconstruct`).
  `xovigen.py` ran cleanly, `make` succeeds. Zero compile warnings
  apart from a redefinition note for `_GNU_SOURCE` (the toolchain
  pre-defines it; safe to drop our `#define`).
- 0e ✓ — `reference/qml/`:
    - `SelectionContextualMenu.qml` (143 lines) — actual base
      `ArkControls.ContextualMenu` derivative.
    - `SceneSelectionHandler.qml` (452 lines) — **the real qmldiff
      target for Phase 3**: contains the existing Cut/Duplicate/
      ConvertToText buttons assigned into `tools` (a
      `SelectionContextualMenu` instance with id `tools`,
      `objectName: "selectionHandlerMenu"`).
    - `DocumentView.qml` (689 lines) — bonus context.
    - `TextSelectionMenu.qml` (1 921 lines) — bonus context.
  Plus `reference/qml-dump/` holds all 739 zstd-decompressed QML
  frames (`frame-<hexoff>.qml`) for grep when we need other files.
  Extractor script: `reference/extract-qml.py`.

### Phase 0d gate — what I'm asking the parent for

Push plan I propose (do not execute until acknowledged):

1. Confirm device IP:
   `ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    root@10.11.99.1 'echo ok && ls /home/root/xovi/extensions.d/'`
2. SCP push:
   `scp src/hello/hello.so
    root@10.11.99.1:/home/root/xovi/extensions.d/cropPaste.so`
3. Restart xochitl: `ssh root@10.11.99.1 'systemctl restart xochitl'`
4. Verify load:
   `ssh root@10.11.99.1 'journalctl -u xochitl --since "30 sec ago"
    | grep cropPaste'` — expect `[cropPaste] hello-world extension loaded`.

Recovery if xochitl boot-loops:

- `ssh root@10.11.99.1 'rm /home/root/xovi/extensions.d/cropPaste.so &&
   systemctl restart xochitl'`. If SSH itself is broken (xochitl owns
   the touch UI but not the network stack, so SSH should always be
   reachable as long as networking comes up), fall back to USB cable;
   USB-Eth `10.11.99.1` is hosted by a kernel-side gadget, independent
   of xochitl.
- No `restore.sh` on device. The disable-everything path is
  `/home/root/xovi/stock` (unmounts the tmpfs overlay over
  `/etc/systemd/system/<unit>.d/`, drops the `LD_PRELOAD=xovi.so`
  drop-in, and restarts the affected services — xochitl runs vanilla
  thereafter). Inverse is `/home/root/xovi/start`. Foreground/debug
  mode: `/home/root/xovi/debug` (stops xochitl, runs it foreground
  under `LD_PRELOAD=xovi.so`). Existing extensions in `extensions.d/`:
  `appload.so`, `framebuffer-spy.so`, `librarian.so`, `literm.so`,
  `qt-command-executor.so`, `qt-resource-rebuilder.so`, `rm-shot.so`,
  `xovi-message-broker.so`. So `framebuffer-spy.so` and
  `qt-resource-rebuilder.so` are already loaded — our `.xovi`'s
  `depends-on` lines will resolve cleanly.

## Decisions and rationale

- **2026-05-03 (Phase 1 retro)** — User unprompted-noticed the
  upstream's two stroke-trace artifacts at first deploy: text rendered
  as outlined-bold ("thickening") and a perfect rectangle drawn
  around every captured region ("border"). Both are inherent to the
  algorithm: Laplacian-of-Gaussian produces an edge band 1–2 pixels
  wide on each side of every transition (so a 4 px stroke is traced
  as two parallel lines outlining it), and the qmd's
  `cropOverlay.visible = false` is non-synchronous so the
  framebuffer still contains the white selection rectangle and the
  30 % black tint at the moment we read it. **This is empirical
  confirmation of the brief's premise that Phase 2 (image paste) is
  the actual deliverable, not a polish phase.** Do not re-litigate
  this in future sessions: the user has, in their own hands, found
  Phase-1's stroke output insufficient.

- **2026-05-03 (Phase 2 plan)** — The cropOverlay→captureArea race
  survives the Phase 2 swap (the white selection rectangle and 30 %
  tint will still be in the captured PNG even if the output side is
  raster). Fold the fix into Phase 2's capture refactor, **not** as
  a throwaway Phase-1 patch. Preferred fix: connect
  `cropOverlay.visible = false` to a `Window.frameSwapped` signal
  (or `QQuickWindow::afterRendering`), trigger `captureArea` only
  after one paint cycle has confirmed the overlay is gone. Fallback:
  `sleepMs(50)` (acknowledge as a hack here). Stretch: split the
  cropOverlay's visual layer (Rectangle + selectionBox) from its
  MouseArea, so the visual subtree can be `parent: null`'d for one
  frame around the capture — fully unrenders, not just hidden.

- **2026-05-03 (Phase 2 deploy strategy)** — Don't uninstall Phase 1
  build between phases. The two new toolbar buttons are harmless
  when not pressed; user is OK with them present. Push Phase 2
  build over the same `cropPaste.so` filename when ready.

- **2026-05-03 (Phase 2 toolbar button order)** — Three test buttons
  injected at the end of `editingTools`, in this left-to-right order
  per the qmd `INSERT` block:
    1. `clipboardPasteButton` (icon `qrc:/ark/icons/duplicate`) —
       Phase 1 stroke-trace paste; reads `/tmp/clipboard_inject.json`
       and stuffs into `Clipboard.items`.
    2. `areaCaptureButton` (icon `qrc:/ark/icons/crop`) — Phase 1
       crop overlay opener.
    3. `imageTestButton` (icon `qrc:/ark/icons/save`) — Phase 2
       first-deploy entry. Calls `ClipboardInjector.pasteTestImage()`
       which runs the heavy-log SceneImageItem path with a hardcoded
       1×1 black PNG + scale-200 transform. Bypasses framebuffer.
  All three collapse into the Phase 3 selection-menu integration when
  Phase 2 v1 ships.

## Roadmap (from `SLAVE-PORT.md`)

- **Phase 0** ✓ — setup + hello-world XOVI ext loads on device.
- **Phase 1** ✓ — port upstream stroke-trace pipeline to aarch64.
  End-to-end validated on ferrari; outputs line art (v0.5 quality).
- **Phase 2** — replace stroke output with `SceneImageItem` (raster).
  ← active. Includes capture-race fix folded in.
- **Phase 3** — wire into the existing selection tool's contextual
  menu, remove the upstream's separate crop overlay.
- **Phase 4** (stretch) — system-clipboard parity, threading polish.

### Phase 2 gate-check (must precede first device push)

Before SCP'ing the Phase 2 build, this report must include:

1. `SceneImageItem` vtable address (RE'd against `xochitl-3.26.0.68`,
   anchored off the known `SceneLineItem` vtable @ 0x16a4338).
2. Full struct layout written up as `rm_SceneImageItem.hpp` with
   `static_assert(offsetof(...))` lines and `tryCast`-equivalent
   invariants.
3. The QML factory we'll scavenge from — explicitly named. Search
   `reference/qml-dump/` for `addImage` / `insertImage` /
   `importImage` / `attachImage` / `imageFromPath`. If no clean
   QML method exists, pick a C++ ctor mangled symbol and document
   it as an `import?` line in `clipboard-injector.xovi`.
4. Image-data representation decision. Default recommendation:
   tmp-file path (write captured PNG to `/tmp/`, paste references
   that path), revisit only if the RE forces an alternative.

A Phase-2-only ship that still uses the upstream's separate crop
button is shippable as v0.5 if Phase 3 stalls.

## What's known going in (compiled from `SLAVE-PORT.md` + verification)

### Hardware / firmware
- Targets: **ferrari** (rMPP 11.8") and **porsche** (rMPP Move 7.3").
  Both **aarch64**, both **xochitl 3.26.0.68**.
- Device IPs: USB `10.11.99.1`, ferrari WLAN `192.168.1.112`,
  porsche WLAN `192.168.1.115`.
- Framebuffer: rMPP panel format is detected as `type=4` (BGRA, 4-byte
  pixels) by `framebuffer-spy` (`rmppCondition` = 1620×2160 stride 6528;
  `rmppmCondition` = 960×1696 stride 3840). The `bgraToGray` path
  upstream is the one we keep.

### Prior art (upstream `clipboard-injector`)
- Three-layer architecture: QML overlay → C++ `ClipboardInjector`
  Q_INVOKABLE → vtable-stealing trick → Clipboard.items.
- The **vtable trick**: QML calls a real native factory
  (`addDrawingLine` for lines), we clone the resulting clipboard item,
  read its first 8 bytes (the vtable pointer), stash it as a static.
  We then construct our own struct payloads with the stolen vtable.
  See upstream `clipboard-injector.qmd:23-39` (`ensureVtablePtr()`)
  and `rm_SceneLineItem.cpp` for the pattern.
- JSON bridge: capture writes `/tmp/clipboard_inject.json`; QML calls
  `ClipboardInjector.loadFromJSON(path)` to reconstruct items.
- The upstream binds `framebuffer-spy$getFramebufferConfig` and
  `framebuffer-spy$refreshFramebuffer` via xovi's import resolver.

### `SceneImageItem` exists in xochitl 3.26
Confirmed by `strings` on `../ferrari/scratch/xochitl-3.26.0.68`:
- `14SceneImageItem` typeinfo string is present.
- `BoundsChanged` and `SceneImageItem::Bounds` strings are present.
- The parent's note on `make_shared<SceneImageItem>` and the
  `xostd::tagged_union<SceneImageItem::Bounds, std::__cxx11::basic_string<char>>`
  pair is the working hypothesis: a SceneImageItem holds a Bounds
  union plus an asset path.
- We have **no headers** for it. Phase 2 will RE the layout in Ghidra.

### qmldiff / hashtab
- `~/src/qmldiff/target/release/qmldiff` is built (commit
  `533d2b9ceac41d2952d92090eed37298cd627440`, fetched 2026-04-24).
- The rMPP 3.26 hashtab is at
  `../freeColour.plugin/reference/hashtab` (20 017 entries, 1 353
  reverse-resolvable to plain names). We will **reuse this
  directly** — copy or symlink into `reference/`.
- Workflow doc:
  `../freeColour.plugin/reference/qmldiff-workflow.md`. Authoring
  pattern: write `src/foo.qml-diff` plain-name, compile via
  `bin/compile-qmd.sh foo.qml-diff` → `build/foo.qmd`. The compile
  script `cp`s into `build/` first because `qmldiff hash-diffs`
  rewrites in place.

### XOVI framework facts
- A working XOVI extension is a `.so` that exposes a
  `_xovi_construct()` C entry point (called once at xovi load).
- `.xovi` description file declares `version`, `import?` /
  `condition` / `override` / `export` / `resource` lines.
- `python3 $XOVI_REPO/util/xovigen.py -o xovi.c -H xovi.h foo.xovi`
  emits a glue-`.c` (and optionally `.h`) declaring `$strdup` /
  `extension$export` style trampoline pointers and a constructor
  table. Build the extension shared lib by linking your code with
  the generated `xovi.c`.
- Hooks on aarch64 use 5-instr movz/movk/movk/movk/br trampolines;
  see upstream xovi README. We don't need to write hooks; we just
  import `framebuffer-spy$*` and depend on `qt-resource-rebuilder`.
- Drop point on device: `/home/root/xovi/extensions.d/<name>.so`.
  `qt-resource-rebuilder`'s qmd dir is
  `/home/root/xovi/exthome/qt-resource-rebuilder/`.

### Already extracted from xochitl
At `../ferrari/scratch/qml-dump/`:
- `files/Toolbar.qml` (57 lines — actually the *selectionButton*
  PenTool, not the screen-edge toolbar that has `editingTools`).
- `files/WritingTool.qml`, `PrimaryPenMenu.qml`, `SecondaryPenMenu.qml`.
- `all-decompressed.bin` (8.1 MB) — every zstd-decompressed QML
  fragment from the binary, concatenated. Grep this for QML usage.
- The parent's `editingTools` `Repeater` (the row the upstream's
  Phase-1 qmd targets) lives in a *different* QML file in
  `all-decompressed.bin` — found in context with
  `PenInputBlocker`/`hideShowButtonDragPoint`/`ToolbarProxyModel`.
  Likely the actual screen-edge toolbar file, distinct from the
  generic `Toolbar.qml` shown in `files/`. **Phase 1c needs to
  resolve which file** the upstream's `[[11313888899523275277]]`
  hash refers to — decompile their qmd against our hashtab to find
  out.

### NOT yet extracted (Phase 0e)
- `SelectionContextualMenu.qml` — referenced as
  `SelectionContextualMenu 1.0 SelectionContextualMenu.qml` in
  `all-decompressed.bin`. Confirmed to exist; not dumped to
  `qml-dump/files/` yet. Phase 0e job: write a small Python script
  that scans the xochitl binary for zstd frame magic
  `\x28\xb5\x2f\xfd`, decompresses, and saves the frame containing
  `SelectionContextualMenu` (and any related `SelectionPreview` /
  selection floatbar files) into `reference/`.

## Findings from getting here (Proven, don't re-verify)

1. `~/src/xovi` does **not** exist locally. Need to clone in Phase 0a:
   `git clone https://github.com/asivery/xovi ~/src/xovi`. The
   upstream Dockerfile clones to `/tmp/xovi` inside the container —
   same pattern works on the host for local `xovigen.py` access.
2. `~/src/qmldiff/target/release/qmldiff` is built and on disk
   (verified). `~/src/rm-xovi-extensions/` is checked out with
   `framebuffer-spy/`, `qt-resource-rebuilder/`, etc. (verified).
3. `framebuffer-spy` already speaks rMPP and rMPP-Move — see
   `~/src/rm-xovi-extensions/framebuffer-spy/src/main.c:35-46`.
   The export it offers via xovi is `getFramebufferConfig`, returning
   a `FramebufferConfig { void* framebufferAddress; int width, height,
   type, bpl; bool requiresReload; }`. Type 1 = RGB565 (rM1), Type 2
   = RGBA/BGRA (rM2 colour, rMPP, rMPP-Move).
4. **rMPP toolchain image exists**: Docker Hub has
   `eeems/remarkable-toolchain:latest-rmpp` (alias of
   `5.6.75-rmpp`, last updated 2026-04-30) and
   `latest-rmppm` for the Move. Replaces `latest-rm2` in the
   upstream's Dockerfile.
5. Docker is installed locally (v28.5.1, Apple Silicon).
6. Upstream `clipboard-injector.qmd` `TRAVERSE` hash
   `[[8397993708429497603]] > [[6502786168]]#[[233745975898428]] >
   [[8398044571386570029]]` — meaning unknown; resolve by
   `qmldiff hash-diffs -r reference/hashtab clipboard-injector.qmd`
   in Phase 1d. The `editingTools` `Repeater` it inserts into
   matches the rMPP toolbar shape (verified by grepping
   `all-decompressed.bin` — same structure).

## Decision log

- **2026-05-03** — created MASTER.md, mirrored
  `../freeColour.plugin/MASTER.md` shape.
- **2026-05-03** — Toolchain choice for Phase 0c: pin to
  `eeems/remarkable-toolchain:5.6.75-rmpp` (not `latest-rmpp`,
  to keep builds reproducible across firmware bumps; the firmware
  on device is 3.26.0.68 but the codex SDK image label tracks the
  *codex* release independent of xochitl). Build for aarch64,
  drop `-mfpu`/`-mfloat-abi` (they're armv7-only flags). The
  build output is one `.so` — both ferrari and porsche should
  load the same binary; we'll only diverge if/when we hit an
  ABI difference at runtime.
- **2026-05-03** — Vendor strategy: copy the upstream's
  `clipboard-injector/` into `src/` verbatim in Phase 1a, then
  modify in place. Don't fork the upstream; just keep their MIT
  notices intact and their `acknowledgments` in our README.

## Conventions inherited from `../freeColour.plugin/MASTER.md`

- Repo layout: `src/`, `bin/`, `build/`, `reference/`. `build/` is
  gitignored. Hashtab lives at `reference/hashtab` (will copy or
  symlink from freeColour's).
- Plain-name source `src/foo.qml-diff` → hashed device-ready
  `build/foo.qmd` via `bin/compile-qmd.sh`.
- Don't commit large binaries (xochitl-3.26.0.68 is referenced from
  `../ferrari/scratch/`).
- Iteration on the device requires xochitl restarts. Batch deploys.
- SSH host-key warnings are noisy on the rMPPs; commands prefix
  `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`.

## Repo layout (planned)

```
cropPaste.plugin/
├── MASTER.md                 # this file
├── SLAVE-PORT.md             # parent brief
├── README.md                 # user-facing (TBD)
├── Makefile                  # install/restore against device (TBD)
├── Dockerfile                # cross-compile container (Phase 0c)
├── src/                      # vendored upstream + our changes
│   ├── ClipboardInjector.{cpp,hpp}
│   ├── rm_*.{cpp,hpp}
│   ├── main.cpp / entry.c
│   ├── clipboard-injector.{pro,xovi}
│   ├── *.qml-diff            # plain-name qmldiff sources (Phase 1d / 3a)
│   └── hello.{c,xovi}        # Phase 0d hello-world
├── build/                    # gitignored: .qmd, .so, qmake objects
├── bin/
│   └── compile-qmd.sh        # copied from freeColour
└── reference/
    ├── hashtab               # symlink/copy from freeColour
    ├── extract-qml.py        # Phase 0e zstd-frame extractor
    ├── SelectionContextualMenu.qml   # Phase 0e output
    ├── SelectionPreview.qml          # Phase 0e output (if found)
    ├── upstream/             # original clipboard-injector files for diff
    └── …
```

## Risks / things to watch (carried forward from `SLAVE-PORT.md`)

- **Bricking risk**: a bad `.so` in `extensions.d/` can boot-loop
  xochitl. Recovery is `ssh root@<ip>` → delete the offending file →
  `systemctl restart xochitl`. **Confirm this works before pushing
  the first build.** The parent has flagged this as the gate
  before Phase 1.
- **Pin firmware**: every offset, vtable address, hashed identifier
  documented here gets a `(3.26.0.68)` tag. Future firmware bumps
  need a re-RE pass.
- **`SceneImageItem` may be read-only.** If notebook persistence
  refuses user-created instances, Phase 2 dies and we fall back to
  a host-side flow (write a `.rm` page via `drawj2d image`, deposit
  into the doc UUID dir, rescan). Escalate to parent if hit.
- **Selection paste-target compatibility**: if xochitl filters
  scene-item types at paste, Phase 2 needs a C++ hook on the paste
  path itself — meaningful scope expansion. Escalate.

## 2026-05-04 — porsche RE complete

Per-device dispatch for cropPaste's vtable patch + factory call is now
wired. Single `build/cropPaste.so` works on both ferrari and porsche.

**Porsche addresses (firmware 3.26.0.68, BuildID `12313e7b…f2829`)**

| Constant | porsche VA | how confirmed |
| --- | --- | --- |
| Image factory entry | `0xc77740` | most recent `paciasp` before the case-15 body at `0xc777ec` (which has the matching `mov w5, #0x7` type-tag, `mov x0, #0xe8` 232-byte alloc, identity-QTransform load — same shape as ferrari's `0xe58af0`) |
| Factory prologue | `0xd503233f, 0xa9bd7bfd` | raw bytes at file offset `0x877740`. **Identical to ferrari** — `checkFactorySignature()` constants need no per-device split. |
| `SceneImageItem` vtable | `0x1572900` | typeinfo header at `0x15728f0` (offset_to_top=0, typeinfo ptr `0x15728d8`); name string `14SceneImageItem` at `0x15728c0`; vt[0]=`0xca2880` is in .text range |
| `SceneImageItem` typeinfo | `0x15728d8` | `__si_class_type_info` struct: name → `0x15728c0`, parent → `0x00fca8d8` (= SceneItem, same as ferrari) |
| `SceneImageItem::paint` (vt[3]) | `0xca2180` | disasm shows the exact UUID-cache-lookup pattern as ferrari's `0xe83630`: `add x0, x19, #0x48; bl _Z5qHashRK5QUuidm@plt` plus the same hashtable-walk magic constants. cropPaste's `OFF_IMAGE_UUID = 0x48` carries over. |
| ctx → QPainter helper | `0xce4550` | first `bl` inside porsche vt[3] (between `mov x21, x1` ctx-stash and the `_ZN6QImageC1Ev@plt` call) — same instruction position as ferrari's first `bl 0xec5ad0` inside `0xe83630` |
| `SceneLineItem` vtable | `0x1572e48` | name string `13SceneLineItem` at `0x1572e0f`, typeinfo at `0x1572e20` (parent → SceneItem, same as ferrari's), vt[0]=`0xcc7cb0` in .text. **Surfaced for Slave A's eyeDropper porsche port.** |

**Disassembly approach (objdump-only, no Ghidra/IDA/r2)**

Single full `.text` disasm to `/tmp/porsche-text.dis` (162 MB, 5.3 s).
For the factory: awk-grep for `adrp xN, 0x1572000` paired with `add xN,
xN, #0x900` within 6 instructions — yielded 2 candidates (`0xc77810` factory body, `0xc7280` likely a copy/clone). Function entry =
nearest preceding `paciasp`. `.rodata`/typeinfo were dumped via `xxd -s
<file_offset>` (LOAD seg base 0x400000, so file_offset = VA - 0x400000).
A first naïve search for the literal `0x1572900` as 8-byte LE returned
zero hits across the whole binary — the toolchain materialises the
vtable address inline via adrp+add, never as a 64-bit pointer in any
GOT/literal-pool slot, which is why the address has to be found via
the adrp+add walk and not via a relocation scan.

**Per-device dispatch implementation**

- `src/clipboard-injector/firmware_addrs.{hpp,cpp}` — new files. Define `Device { Ferrari, Porsche }`, `struct FirmwareAddrs`, and `firmware()` accessor (Meyers singleton: lazy `static const FirmwareAddrs&` initialised on first call).
- `firmware()` parses `/etc/hostname` once and chooses one of two static `kFerrari` / `kPorsche` tables. Match logic: `imx93` or `chiappa` → Porsche; `imx8mm` or `ferrari` → Ferrari; anything else → Ferrari (defensive default — ferrari is the in-field build). Logs the choice once at process startup.
- `rm_SceneImageItem.{hpp,cpp}`: stripped the `FIRMWARE_VTABLE_ADDR / FIRMWARE_FACTORY_ADDR / FIRMWARE_FACTORY_SIG0/SIG1` constexprs from the header. `checkFactorySignature()` and `factoryCreate()` now read `cropPaste::firmware().imgFactory` / `.factorySig0` / `.factorySig1`. Type tag (`FIRMWARE_FACTORY_TYPE_TAG_IMAGE = 15`) and struct offsets (`OFF_*`) are kept in the header — they're device-invariant per Itanium-ABI / same-source guarantee.
- `customVtable.cpp`: removed the four `constexpr uintptr_t kStock*Addr / kPainterHelperAddr` constants. Each callsite now reads from `cropPaste::firmware()`. `kSlotPaint = 3` stays — slot ordering is device-invariant.
- `clipboard-injector.pro`: added `firmware_addrs.cpp` / `.hpp` to SOURCES / HEADERS.
- `SceneLineItem` vtable is **not** in the address table — `SceneLineItem::setupVtable()` already gets it at runtime from a scavenged item via `cloneSelectedItems`. No change needed there.

**Verification on host**

`cropPaste.so` post-link: 1.0 MB, aarch64, valid ELF. `nm -D` shows `cropPaste::firmware()` exported. `strings` shows `imx8mm`, `imx93`, `chiappa`, `ferrari`, `porsche`, `/etc/hostname` embedded. 8-byte-aligned literal scan confirms each per-device address (ferrari's `0x16a3df0`, `0xe58af0`, `0xec5ad0`; porsche's `0x1572900`, `0xc77740`, `0xca2180`, `0xce4550`) appears exactly once in the .so — i.e., both tables are linked, dispatched at runtime by hostname.

**Known unknowns / runtime-pending checks**

- `SceneImageItem::paint` vt[3] semantics on porsche were verified by disassembly (UUID hash + hashtable walk + QImage default ctor — same shape as ferrari) but **not yet runtime-confirmed**. If porsche compiles inserted an extra slot before paint, vt[3] would be the wrong function and the install-site sanity check `*slot == fw.imgPaint` will catch it (refuses to patch, logs `slot[3] mismatch on porsche: got=… want=…`). Caller logs but doesn't crash; cropPaste degrades to the cache-miss placeholder instead of the color rendering.
- `kPainterHelperAddr` (porsche `0xce4550`) is also disassembly-only. Wrong helper would call into garbage and likely SIGSEGV inside the paint hook. Mitigation: the install path's typeinfo + vt[3] sanity checks fail-closed before any paint occurs, so a wrong vtable patches don't get installed; if those pass and the helper VA is still wrong, we'd see a paint-time crash on porsche only.
- Hand-off: `build/cropPaste.so` is the deployable. Parent should ferrari-smoke-test before porsche deploy — ferrari's addresses are unchanged, so a regression there means the dispatch refactor itself is broken (not the new porsche RE).

## Current task list

See live tasks via `TaskList`. Initial set:
1. Phase 0a — verify/clone xovi/qmldiff/rm-xovi-extensions.
2. Phase 0b — read all references. (done)
3. Phase 0c — pick toolchain + write Dockerfile.
4. Phase 0d — hello-world XOVI extension on device.
5. Phase 0e — extract `SelectionContextualMenu.qml` from xochitl.
6. Write MASTER.md. (done)
