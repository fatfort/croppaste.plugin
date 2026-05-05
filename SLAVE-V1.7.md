---
slave: cropPaste / v1.7 (ship the feature)
parent: aayush, via Opus
written: 2026-05-04 morning
target: rMPP/Move 3.26.0.68 aarch64
prerequisite: v1.5 in source, v1.6f-diagnostic on device, v1.6g + H3 reproducer staged in build/ from prior sessions
---

# SLAVE — cropPaste v1.7 (ship the feature)

You are picking up after a multi-session investigation. **Read MASTER.md
end-to-end first** — it has the full architectural history, the live
hypotheses, the staged artifacts, and the "Session 2026-05-04 (late) —
host-side triage results" subsection that is the freshest state.

This brief is not a substitute for MASTER.md; it sets the new constraints
and sequences the next session's work.

## Goal in one line

Ship cropPaste with all four user-required properties: color rendering +
persistence-across-navigation + selectability/manipulation + no-PDF-regression.
The user has explicitly stated they want this **shipped ASAP** and that
LOC count is not a constraint.

## What changed since the prior SLAVE-V1.6.md

You should ignore SLAVE-V1.6.md's investigation framing in favor of this
one. The relevant deltas:

1. **Three pivot paths exhausted** (do NOT re-investigate, do NOT
   re-propose):
   - xochitl source on GitHub: closed-source, confirmed
   - Official SDK image-add API: doesn't exist
   - depixelator vector path: 1-bit only, no color, dead
   - H3 (kernel mprotect+fork mechanism): denied via C reproducer
   See MASTER.md "Architectures tried and exhausted" for citations.

2. **Two artifacts staged from prior sessions, ready to use:**
   - `build/v1.6g/cropPaste.so` — v1.6f-diagnostic + QUuid::createUuid()
     RFC 4122 v4 conformance fix. installPaintHook still stubbed
     (no PDF crash risk). Cheap candidate for solving Problem B (c).
   - `src/repro_h3/repro_h3_mprotect_fork` — already executed, result
     in `reference/h3_reproducer_result.txt`. Don't re-run.

3. **User reinforcements** (also in your local memory file
   `feedback_cropPaste_no_compromise.md`):
   - No LOC budget. Don't propose architectures on the basis of "less
     code." Engineering scope is not the constraint.
   - Don't give up. Architecture pivots are acceptable; abandonment
     is not.
   - Ship ASAP framing — fast iteration over thorough understanding,
     where they conflict.

## The two open problems (recap, see MASTER.md for detail)

- **Problem A:** v1.6e's global vtable patch (mprotect+write on xochitl's
  .rodata) crashes the PDF renderer subprocess. Mechanism unknown after
  H3 denied. H4+ hypothesis space includes Qt6 IPC, PDFium-specific
  shared state, SysV/memfd shared memory, atfork/signal handlers.

- **Problem B:** persistence broken even with global patch — deserialized
  items show octagon-on-revisit. Hypothesis (c) live: deserialize/dispatch
  bypasses our patched vtable. UUID conformance fix in v1.6g is the
  cheap candidate; deeper instrumentation is the fallback.

## Sequencing for this session — ship-ASAP framing

The prior session's framing was "investigate to understand." This
session's framing is "ship as fast as the empirical results allow."
Try the cheapest wins first; only deepen if cheap wins fail.

### Step 1 — Deploy v1.6g, test Problem B (c) collapse

`build/v1.6g/cropPaste.so` is staged. Same deploy discipline as always:
stash current device .so to `build/v1.6f-rollback/`, SCP, restart xochitl,
journal-tail in second SSH. Then have the user paste, navigate away,
navigate back. Two outcomes:

- **Octagon-on-revisit goes away** (color or placeholder, doesn't matter
  for this test — what matters is whether deserialized items render
  *something* via our dispatch path). Problem B (c) is collapsed by the
  UUID fix. Proceed to step 2 immediately.
- **Octagon persists**. Problem B (c) is something else. Build an
  instrumented v1.6h that logs deserialize-time vtable pointer and side-map
  lookup attempts on revisit. This is iteration territory — you have full
  token budget for it.

Note: with v1.6g the patch is still stubbed, so paste itself shows the
placeholder, not color. The test is *only* about deserialize behavior —
does revisit render the same placeholder as fresh-paste, or the octagon?
If same as fresh-paste → dispatch works on revisit → Problem B's persistence
is unblocked the moment we re-enable the patch (which is step 2).

### Step 2 — Re-enable installPaintHook with lazy-install workaround for Problem A

H3 is denied so we don't have a mechanism explanation, but we have a
plausible workaround that doesn't require understanding the mechanism:
**patch only during capture, unpatch immediately after.**

The renderer fork happens at PDF-page-load time. If our patch window is
short (microseconds, between mprotect-RW → write → mprotect-R), and we
ensure no PDF page load is in flight during that window, the renderer
shouldn't see the poisoned state.

Implementation (rough):
- New v1.6h build: `installPaintHook` and a paired `uninstallPaintHook`.
- captureAreaAsImage() entry: install patch (quick mprotect+write+mprotect).
- captureAreaAsImage() exit / paste-acknowledged: uninstall patch (restore
  original byte, mprotect again).
- Heavy-log first invocation: timestamps before/during/after the install,
  so we can measure window duration.
- Trip-wire on deploy: if renderer crashes within 60s of any capture,
  rollback. Otherwise iterate on tightening the window or scoping it
  differently.

If lazy-install works empirically → ship v1.7. The user has accepted
"ship a workaround that works" earlier in this project's pattern.

If lazy-install crashes the renderer → the install/uninstall window
itself is enough to poison renderer state. Then we need either (a) longer
install window with fork-blocking via prctl/seccomp, (b) deeper Problem
A investigation (strace correlation, shared memory enumeration, etc.).

### Step 3 — H4+ Problem A investigation, only if lazy-install fails

If step 2 doesn't yield a shippable workaround, expand investigation
methods:

- `strace -ff -e trace=mprotect,fork,clone,execve,mmap` on main during
  patch + capture, correlate with renderer behavior in journal
- Enumerate shared memory between main and renderer:
  `lsof -p <main-pid> | grep -E '(SHM|memfd|/dev/shm)'`
  same for renderer
- Inspect `/proc/<main-pid>/smaps` for any shared mappings the renderer
  also has
- Look at whether xochitl uses `pthread_atfork` (`nm`/`strings` on
  xochitl binary)
- Look at whether the renderer reads any state from main's address
  space via `process_vm_readv` (`strace` on renderer)

Each diagnostic is a separate sub-step; sequence them, don't batch.

## Constraints (carry forward from prior sessions)

- **Recovery primitive proven and required:** every device-touching
  deploy stashes the current .so first. Recovery is `ssh root@<ip> 'rm
  /home/root/xovi/extensions.d/cropPaste.so && systemctl restart xochitl'`.

- **No xochitl restart during normal use** (memory file
  `no_restart_during_use.md`). Lazy install/uninstall must complete
  without forcing a restart.

- **Color + persistence + selectability + no-PDF-regression are ALL
  required.** Don't propose ship-with-known-limitations.

- **Same deploy discipline as prior phases:**
  signature-check-before-call, heavy-log first invocation (disable
  after success), allocator pairing, second-SSH journal-tail.

- **Trip-wire:** any renderer-exit in the 60s post-deploy window =
  immediate rollback. We re-established this is non-negotiable.

## Definition of done

User can, on ferrari (porsche pending Kiyomi's return):
1. Activate the existing crop button.
2. Drag a selection over PDF content (any color, including highlights).
3. Release; switch to selection tool; tap paste in destination notebook.
4. See the captured region appear as a faithful color raster image
   that visually matches the source.
5. Navigate away from the destination, navigate back — image still
   renders correctly (NOT octagon).
6. Lasso the image with selection tool, move/resize/delete it.
7. Open a PDF in any notebook — no renderer crashes.
8. All this without restarting xochitl.

That's v1.7 ship. Anything less is not done.

## Pickup list for this session

1. Read MASTER.md end-to-end. Especially the late-2026-05-04 triage
   subsection.
2. Read `reference/h3_reproducer_result.txt` so you know what was
   ruled out.
3. Read this brief.
4. Step 1: deploy v1.6g, test (c). Report verbatim journal block +
   user observation of revisit behavior before doing anything else.
5. Step 2 (gated on step 1 outcome): build v1.6h with lazy-install,
   deploy, test color + persistence + PDF-stability.
6. Step 3 (gated on step 2 outcome): H4+ investigation if needed.

Begin by reading MASTER.md. Don't deploy anything until you've reported
your read-through summary and proposed plan back to me.
