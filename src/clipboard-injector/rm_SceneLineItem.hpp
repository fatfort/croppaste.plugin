#pragma once

#include <cstddef>
#include <memory>
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"

// Layout was reverse-engineered by the upstream against rM2 (armv7,
// 4-byte pointers). Field names encode armv7 byte offsets and are
// kept verbatim for tracking against upstream commits — on aarch64
// the actual offsets shift because vtable + every void* doubles
// from 4 to 8 bytes.
struct SceneLineItem : public SceneItem {
    unsigned char unk_x4;
    int pageIndex;
    short unk_xc;
    short unk_xe;
    int sourceLayerId;
    short unk_x14;
    short unk_x16;
    int unk_x18;
    void* unk_x1c;
    unsigned char unk_x20;
    unsigned char unk_x21;
    void* unk_x24[3];
    Line line;
    int unk_x78;
    int unk_x7c[3];

    static void* vtable_ptr;
    static void setupVtable(void* vtable);

    static SceneLineItem fromLine(Line &&line) {
        SceneLineItem item = {};
        item.vtable = vtable_ptr;
        item.unk_x4 = 3;
        item.pageIndex = 0xE;
        item.unk_xe = 1;
        item.sourceLayerId = 0xB;
        item.line = line;
        item.unk_x78 = 1;
        return item;
    }

    static SceneLineItem* tryCast(SceneItem* item);
};

// Catch our own miscompiles. These offsets do NOT prove xochitl uses
// the same layout — only that the C++ compiler arranges *our* struct
// the way we expect. If xochitl's SceneLineItem disagrees, the
// vtable trick will fail at runtime; tryCast()'s invariant checks
// (logged on entry) will surface the mismatch.
#ifdef __aarch64__
static_assert(sizeof(SceneItem) == 8, "SceneItem aarch64 size");
static_assert(offsetof(SceneLineItem, unk_x4) == 0x08, "unk_x4 offset");
static_assert(offsetof(SceneLineItem, pageIndex) == 0x0c, "pageIndex offset");
static_assert(offsetof(SceneLineItem, unk_xc) == 0x10, "unk_xc offset");
static_assert(offsetof(SceneLineItem, unk_xe) == 0x12, "unk_xe offset");
static_assert(offsetof(SceneLineItem, sourceLayerId) == 0x14, "sourceLayerId offset");
static_assert(offsetof(SceneLineItem, unk_x18) == 0x1c, "unk_x18 offset");
static_assert(offsetof(SceneLineItem, unk_x1c) == 0x20, "unk_x1c offset (void*, 8B)");
static_assert(offsetof(SceneLineItem, unk_x20) == 0x28, "unk_x20 offset");
static_assert(offsetof(SceneLineItem, unk_x21) == 0x29, "unk_x21 offset");
static_assert(offsetof(SceneLineItem, unk_x24) == 0x30, "unk_x24[3] offset (void*, 8B align)");
static_assert(offsetof(SceneLineItem, line) == 0x48, "line offset");
#endif
