#pragma once

#include <cstddef>
#include <cstdint>

class QImage;

// v1.6 path 5 — Global SceneImageItem paint() hook via vtable patching.
//
// Replaces v1.5/v1.6c per-item custom vtable swap with a single global patch
// on the stock SceneImageItem vtable. After installation, every paint
// dispatch (ours + stock notebook images) flows through ourGlobalPaint,
// which:
//   - Reads UUID at this+0x48
//   - Hits g_imageMap (UUID-keyed) → drawImage from the stashed QImage
//   - Misses → calls saved-original paint @ 0xe83630 (stock behavior)
//
// UUID-keyed map outlives any individual SceneImageItem pointer, so it
// survives navigate-away-and-back: the deserializer reconstructs items
// with the same UUID at +0x48 from the .rm scenefile, and the lookup
// re-hits.
//
// Map dies on xochitl restart. The user accepted this constraint
// explicitly — restart-bounded persistence, not navigate-bounded.

namespace cropPaste {

// One-time install of the global paint hook. Idempotent; safe to call
// multiple times. Returns false on any sanity-check failure (typeinfo
// header mismatch, slot 3 not pointing at the expected stock paint
// address, mprotect failure). On failure: hook is not installed, all
// SceneImageItem paints continue dispatching to stock — extension is
// effectively a no-op for color rendering. Should be called once from
// _xovi_construct.
bool installPaintHook();

// Stash a QImage under the given 16-byte UUID. Caller is responsible for
// also writing the same UUID into the SceneImageItem at offset 0x48
// (via SceneImageItem::setImageUuid) so the paint hook's lookup hits.
//
// QImage is held by value — Qt6 implicit sharing makes this a refcount
// bump, not a pixel copy.
void registerImage(const uint8_t uuid[16], QImage image);

// Diagnostic.
size_t imageMapSize();

}  // namespace cropPaste
