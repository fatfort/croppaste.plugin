#pragma once

#include <cstdint>

namespace cropPaste {

enum class Device { Ferrari, Porsche };

// Pinned addresses for a single firmware build. All VAs are inside
// xochitl's text/rodata. Reset on every firmware bump (3.26.0.68 today).
struct FirmwareAddrs {
    Device      device;
    const char* deviceName;

    // SceneImageItem-related (Phase 0..4 RE).
    uintptr_t   imgVtable;        // first vfunc slot (objects store this)
    uintptr_t   imgTypeinfo;      // = imgVtable - 0x28
    uintptr_t   imgPaint;         // = imgVtable[3]
    uintptr_t   painterHelper;    // ctx -> QPainter*
    uintptr_t   imgFactory;       // generic factory entry, dispatches case 15

    // Factory prologue first 8 bytes (paciasp; stp x29,x30,[sp,#-0x30]!).
    // checkFactorySignature reads these before each call.
    uint32_t    factorySig0;
    uint32_t    factorySig1;
};

// Lazily detects on first call by parsing /etc/hostname.
// "imx8mm-ferrari" -> Ferrari, "imx93-chiappa" -> Porsche.
// Anything else: defaults to Ferrari (defensive — ferrari is the
// build that's been in field use). Logs the choice once.
const FirmwareAddrs& firmware();

}  // namespace cropPaste
