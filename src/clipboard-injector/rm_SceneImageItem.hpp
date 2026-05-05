#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "rm_SceneItem.hpp"

// Layout RE'd against ferrari xochitl 3.26.0.68. See MASTER.md
// "Known firmware constants" for the anchor methodology. Only the
// fields below are confirmed; the rest of the 216-byte object is
// initialized by xochitl's own factory at 0xe58af0 (case 15) and
// we leave it untouched.
//
//   0x00  void*      vtable                = 0x16a3df0  (Phase 2a)
//   0x08  uint8_t    type_tag              = 7
//   0x10..0x30  unknown (probably std::__cxx11::basic_string<char>
//                       per the brief's "tagged_union<Bounds, string>")
//   0x30..0x48  std::vector<uint8_t> bytes — destructor frees
//               (end_of_storage - begin) via _ZdlPvm
//               (operator delete(void*, size_t)). Match alloc with
//               ::operator new(size_t).
//   0x48..0x60  unknown
//   0x60..0x80  QRectF source_bounds       (input)
//   0x80..0xc8  QTransform transform       (default = 3x3 identity
//                                           loaded from .rodata
//                                           template @ 0x16a3a00)
//   0xc8        uint16_t QTransform m_type cache
//   0xd0        8 B (zeroed)
//   total       0xd8 = 216 bytes

struct SceneImageItem : public SceneItem {
    // Per-device addresses live in firmware_addrs.{hpp,cpp}; the
    // selector parses /etc/hostname once at startup. Type tag and
    // struct offsets below are device-invariant (same source compiled
    // for both rMPP variants — Itanium ABI guarantees layout match).
    static constexpr int       FIRMWARE_FACTORY_TYPE_TAG_IMAGE = 15;

    static constexpr size_t    OBJECT_SIZE            = 0xd8; // 216
    static constexpr size_t    OFF_VTABLE             = 0x00;
    static constexpr size_t    OFF_TYPE_TAG           = 0x08;
    static constexpr size_t    OFF_BYTES_BEGIN        = 0x30;
    static constexpr size_t    OFF_BYTES_END          = 0x38;
    static constexpr size_t    OFF_BYTES_END_STORAGE  = 0x40;
    static constexpr size_t    OFF_SOURCE_BOUNDS      = 0x60;
    static constexpr size_t    OFF_TRANSFORM          = 0x80;

    // v1.5b discovery: paint() at vt[3] (0xe83630) does an image-cache lookup
    // keyed by a QUuid at this+0x48..0x58. Cache miss → dotted-rect placeholder
    // (= the "octagon" the user was seeing). Real images set a UUID here that
    // resolves to a cached QImage. We populate this with a generated UUID and
    // write the PNG to a path xochitl can find on cache-miss, hoping for an
    // auto-load.
    static constexpr size_t    OFF_IMAGE_UUID         = 0x48;  // 16 bytes
    // QTransform internal m_type/m_dirty cache. Bits 0-4 = m_type
    // (TxNone=0, TxTranslate=1, TxScale=2, TxRotate=4, TxShear=8,
    // TxProject=0x10). Bits 5-9 = m_dirty. Default-init clears
    // bits 0-9 by `(*p &= 0xfc00)` — verified in factory disasm.
    static constexpr size_t    OFF_TRANSFORM_M_TYPE   = 0xc8;
    static constexpr uint16_t  M_TYPE_MASK_BITS_KEEP  = 0xfc00;
    static constexpr uint16_t  M_TYPE_TX_SCALE        = 0x0002;

    // Returns true if the bytes at FIRMWARE_FACTORY_ADDR match the
    // expected prologue. Logs the actual bytes verbatim either way
    // so a journal grep tells the next session what to repin.
    static bool checkFactorySignature();

    // Calls xochitl's case-15 factory by hardcoded address. Returns
    // an empty std::shared_ptr on signature-check failure (will not
    // jump into garbage).
    static std::shared_ptr<SceneItem> factoryCreate();

    // Hexdump a freshly-returned SceneImageItem to the journal.
    // Used once on first deploy to validate our believed layout
    // against ground truth.
    static void hexdumpDefaultInit(SceneItem* item);

    // Mutate the four well-understood fields. Each writes one or
    // two scalars; logs offset + before/after in heavy mode.
    // Returns false on any mismatch we can detect without reading
    // outside the 216-byte struct.
    static bool setSourceBoundsF(SceneItem* item,
                                 double x, double y,
                                 double w, double h,
                                 bool heavyLog);

    // Allocate a buffer with ::operator new(size) (matches the
    // destructor's _ZdlPvm), copy `bytes` into it, populate the
    // vector triplet at 0x30/0x38/0x40. Caller transfers ownership.
    static bool setInlineBytes(SceneItem* item,
                               const uint8_t* bytes, size_t size,
                               bool heavyLog);

    // Write a 16-byte UUID at OFF_IMAGE_UUID. Caller generates the bytes;
    // the same bytes are used to derive the on-disk path xochitl will look up.
    static bool setImageUuid(SceneItem* item,
                             const uint8_t uuid[16],
                             bool heavyLog);

    // Experimental: write the "image kind" bytes at offsets 0x28/0x29.
    // Default is zero; vt[6] @ 0xe83900 returns this[0x28] (or 2 if zero),
    // and vt[7] @ 0xe83910 returns this[0x28] then this[0x29]. These look
    // like a 2-byte format/source discriminator. Toggling them may flip
    // rendering between cache lookup and inline-bytes paths. Used by
    // captureAreaAsImage's experimental cycling (see kImageKindCycle).
    static bool setImageKind(SceneItem* item,
                             uint8_t kind, uint8_t subkind,
                             bool heavyLog);

    // Write a pure-scale 3x3 transform into OFF_TRANSFORM and update
    // the m_type cache at OFF_TRANSFORM_M_TYPE to TxScale, preserving
    // bits 10-15 (which the factory left untouched). Whether xochitl
    // honors the cache or recomputes it from the matrix is the
    // ground-truth question for the first-deploy log.
    static bool setTransformScale(SceneItem* item, double scale,
                                  bool heavyLog);
};
