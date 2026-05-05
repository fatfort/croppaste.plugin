#include "rm_SceneLineItem.hpp"
#include <cstddef>
#include <cstdio>

void* SceneLineItem::vtable_ptr = nullptr;

void SceneLineItem::setupVtable(void* vtable) {
	fprintf(stderr,
		"[cropPaste] SceneLineItem layout: sizeof=%zu line@0x%zx unk_xc@0x%zx unk_x78@0x%zx — vtable=%p\n",
		sizeof(SceneLineItem),
		offsetof(SceneLineItem, line),
		offsetof(SceneLineItem, unk_xc),
		offsetof(SceneLineItem, unk_x78),
		vtable);
	SceneLineItem::vtable_ptr = vtable;
}

SceneLineItem* SceneLineItem::tryCast(SceneItem* item) {
    if (!item) return nullptr;
    auto* lineItem = reinterpret_cast<SceneLineItem*>(item);
    if (lineItem->unk_xc != 0) {
        fprintf(stderr, "[cropPaste] tryCast: unk_xc=%d (expected 0) — struct layout mismatch on this firmware\n",
                (int)lineItem->unk_xc);
        return nullptr;
    }
    if (lineItem->unk_xe != 0) {
        fprintf(stderr, "[cropPaste] tryCast: unk_xe=%d (expected 0)\n", (int)lineItem->unk_xe);
        return nullptr;
    }
    if (lineItem->unk_x78 != 1) {
        fprintf(stderr, "[cropPaste] tryCast: unk_x78=%d (expected 1)\n", lineItem->unk_x78);
        return nullptr;
    }
    if (!(lineItem->unk_x20 == 0x0 || (lineItem->unk_x20 == 0x2 && lineItem->unk_x21 == 0x2))) {
        fprintf(stderr, "[cropPaste] tryCast: unk_x20=%d unk_x21=%d (expected (0,*) or (2,2))\n",
                (int)lineItem->unk_x20, (int)lineItem->unk_x21);
        return nullptr;
    }
    return lineItem;
}
