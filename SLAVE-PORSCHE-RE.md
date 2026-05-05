# SLAVE-PORSCHE-RE — cropPaste.plugin (porsche binary RE for factory address)

Date opened: 2026-05-04.
Author: parent Claude session.
Continuation of: v1.5+ ferrari-shipping work. Read MASTER.md
end-to-end before doing anything; the relevant section is
"Per-device pinned constants" (~line 815) and the v1.5 entry's
porsche-RE-needed list (~line 130).

Target: extend cropPaste's per-device dispatch table to include
porsche addresses, so a single .so binary loads cleanly on both
ferrari and porsche.

**This slave does host-side RE only.** No SSH, no device touching.
Porsche is unreachable today; ferrari is owned by other slaves.

---

## 0. Read first (mandatory)

- `MASTER.md` lines ~800-840 (the "Per-device pinned constants" table
  + the porsche-TBD note about ADRP+ADD vtable load).
- `MASTER.md` lines ~830-895 (factory address discovery for ferrari
  at `0xe58af0`, factory prologue, vtable patching pattern).
- `src/clipboard-injector/rm_SceneImageItem.{hpp,cpp}` — ferrari's
  vtable hook + factory call.
- `src/clipboard-injector/rm_SceneLineItem.{hpp,cpp}` — same pattern,
  smaller scope.
- The aarch64 ADRP+ADD addressing primer (any reference). Key:
  `adrp xN, page` loads page-aligned base; `add xN, xN, #offset`
  fills in the low 12 bits. Together they form a PC-relative
  full address.

Binaries available locally:

```
~/Documents/remarkable/ferrari/scratch/xochitl-3.26.0.68   # 23 MB
~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68   # 22 MB, BuildID 12313e7b...
```

Both are stripped aarch64 ELFs.

---

## 1. Goal (concrete deliverables)

Append a porsche row to MASTER's per-device constants table with
real values for:

| Constant                       | porsche value         |
| ------------------------------ | --------------------- |
| Image factory address          | TBD → **find it**     |
| Factory prologue (4-byte LE)   | TBD → **dump it**     |
| `SceneImageItem` vtable        | `0x1572900` (already known) |
| `SceneLineItem` vtable         | TBD → **find it**     |
| Paint vtable slot (vt[3])      | TBD → **verify offset** |

Plus: extend cropPaste's C++ dispatch logic to branch on device
identity (BuildID hash, panel dimensions from framebuffer-spy,
or `/etc/hostname` parse → ferrari/porsche). Existing
program-name guard is the pattern; add the address-table branch on
top.

Update `MASTER.md`'s constants table with the dug-out values + a
short note on the disassembly approach used.

---

## 2. Approach

### Phase 0 — Confirm vtable address still applies

```bash
# Verify SceneImageItem vtable @ 0x1572900 on porsche
objdump -d ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 \
  --start-address=0x1572900 --stop-address=0x1572a00 2>/dev/null | head -40
```

Expect: a vtable-shaped layout (function pointers, sometimes
preceded by RTTI). Confirm `SceneImageItem` typeinfo string
nearby (per MASTER §"strings identical: `14SceneImageItem`...").

### Phase 1 — Find the image factory via ADRP+ADD walk

The factory loads its vtable via ADRP+ADD on porsche (vs. literal
on ferrari). Strategy: search for the ADRP+ADD instruction pair
that materialises `0x1572900`:

```bash
# Disassemble entire .text and grep for adrp pages near 0x1572000
objdump -d ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 \
  | grep -E '\sadrp\s' | grep -E '0x15720|0x15721|0x15722|0x15723' \
  | head -20
```

For each candidate `adrp xN, 0x1572000` instruction, look at the
next few instructions for `add xN, xN, #0x900` — that's the pair
that builds `0x1572900`. The function containing that pair is the
factory candidate.

```bash
# Once you have a candidate address PC for the adrp instruction,
# dump 32 instructions around it for context
objdump -d ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 \
  --start-address=<PC-128> --stop-address=<PC+128>
```

Cross-reference against ferrari's factory at `0xe58af0`: the
function shape (prologue, vtable load, RTTI store, return) should
look very similar.

### Phase 2 — Dump factory prologue

Once factory address known, the prologue is the first 8 bytes
(little-endian, 4-byte instructions):

```bash
# Read first 16 bytes of the factory function as raw hex
objcopy -O binary --only-section=.text \
  --change-section-address .text=0 \
  ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 /tmp/text.bin

# Compute file offset from VA: VA - .text base + .text file offset
# Then xxd /tmp/text.bin | grep -A1 <offset>
```

Or simpler:

```bash
# Read straight from the ELF at the factory VA
objdump -d ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 \
  --start-address=<factory-VA> --stop-address=<factory-VA+16> \
  | head -8
```

Convert to 4-byte LE words. Document in MASTER alongside ferrari's
`0xd503233f, 0xa9bd7bfd`.

### Phase 3 — Repeat for SceneLineItem vtable

Same pattern: search for `13SceneLineItem` typeinfo string,
backtrack to the vtable that references it, note the address.

```bash
# Find the typeinfo string
strings -t d ~/Documents/remarkable/porsche/scratch/xochitl-3.26.0.68 \
  | grep -E '13SceneLineItem'
# That gives the file offset; add ELF base for VA.
```

### Phase 4 — Verify paint vt[3]

Vt[3] is typically the third virtual function in the slot order
(after RTTI/dtor/dtor2). cropPaste's ferrari `MASTER.md` describes
which vt slot is `paint`. Load porsche's vtable (Phase 0 + 3) and
confirm vt[3] points to a function that disassembles like
ferrari's paint method (or use a string xref to confirm — paint
methods often reference QPainter strings).

### Phase 5 — Wire per-device dispatch in C++

In `src/clipboard-injector/...`:

- Add a `Device` enum (Ferrari, Porsche).
- Add a `detect_device()` function. Options:
  - Read `/etc/hostname` (`imx8mm-ferrari` vs `imx93-chiappa`).
  - Read framebuffer dimensions (1620×2160 vs 960×1696) via
    framebuffer-spy if it's already linked.
  - Hash the running `xochitl` BuildID (parse `/proc/self/exe`'s
    `.note.gnu.build-id`).
  - Cheapest is hostname parse — adopt that unless there's a
    reason not to.
- Replace each hardcoded address constant with a lookup in a
  `static const struct AddressTable[]` indexed by `Device`.
- Recompile against `rmpp-sdk:5.6.75` (Docker image already built,
  see MASTER §"Salvageables from V1" + Dockerfile).

### Phase 6 — Smoke test on ferrari

The new dispatching binary must still work on ferrari (ferrari
branch unchanged in addresses). Hand off to the parent for actual
ferrari smoke test — slave does NOT deploy.

Hand-off deliverable: a single `build/cropPaste.so` that the
parent can scp to either device.

---

## 3. Hazards

- **ADRP page alignment**: `adrp` materialises a 4 KB page, so
  the immediate is `0x1572000` (page) and `add` provides `0x900`
  (offset within page). Don't search for `adrp ..., 0x1572900`
  — that won't appear.
- **Multiple ADRP+ADD pairs** may materialise the same address.
  The factory is the one whose surrounding function shape matches
  ferrari's `0xe58af0` (creation pattern: param `int type_tag`,
  16-byte sret slot for `shared_ptr<SceneItemBase>`, vtable
  store). Other matches might be RTTI fixups elsewhere.
- **Stripped binary**: no symbol names. Cross-reference via
  string xrefs, vtable shape, RTTI layout. Ghidra/IDA/radare2
  would be ideal here but objdump+grep+careful reading is enough.
- **Don't break ferrari**: the per-device dispatch must default
  to ferrari constants when the device check fails. Defensive
  fallback.
- **No SSH, no porsche touching**: Porsche unreachable today.
  All work is host-side disassembly + C++ edit + compile.
- **Sibling slaves**: don't restart the parent's ferrari xochitl
  — other slaves are using it.

---

## 4. Hand-off back to parent

When complete:

- Append a dated entry to `MASTER.md` under
  "## 2026-05-04 — porsche RE complete" listing:
  - Porsche factory address (with the disassembly walk that found
    it — 1-2 sentences).
  - Porsche factory prologue (4-byte LE words).
  - Porsche `SceneLineItem` vtable address.
  - Verified paint vt[3] offset (and how confirmed).
  - Per-device dispatch implementation note (how `detect_device()`
    works, where the address table lives).
- Compiled `build/cropPaste.so` ready for ferrari smoke test.
- Note in MASTER: known unknowns (e.g., if paint vt[3] couldn't be
  confirmed without runtime verification on porsche, document the
  assumption + a runtime assert that catches a wrong offset).

---

## 5. Time budget

Target: complete by 16:30 AEST so parent has 30min for ferrari
smoke test before 17:00 porsche deploy. If you blow past 16:30,
ping parent — cropPaste defers to next porsche window and tonight's
bundle ships without it.
