#include "rm_SceneImageItem.hpp"
#include "firmware_addrs.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace {

constexpr int kHexBytesPerRow = 16;

void hexdumpRow(const uint8_t* p, size_t off, size_t row_bytes) {
    char hex[kHexBytesPerRow * 3 + 1];
    char ascii[kHexBytesPerRow + 1];
    int hp = 0;
    for (size_t i = 0; i < row_bytes; ++i) {
        hp += std::snprintf(hex + hp, sizeof(hex) - hp, "%02x ", p[i]);
        ascii[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    ascii[row_bytes] = 0;
    fprintf(stderr, "[cropPaste:img] %04zx  %-48s |%s|\n",
            off, hex, ascii);
}

} // namespace

bool SceneImageItem::checkFactorySignature() {
    const auto& fw = cropPaste::firmware();
    auto* prologue = reinterpret_cast<const uint32_t*>(fw.imgFactory);
    uint32_t got0 = prologue[0];
    uint32_t got1 = prologue[1];
    bool ok = got0 == fw.factorySig0 && got1 == fw.factorySig1;
    fprintf(stderr,
        "[cropPaste:img] factory(%s) @ 0x%lx prologue check: got=(0x%08x, 0x%08x) "
        "want=(0x%08x, 0x%08x) -> %s\n",
        fw.deviceName, (unsigned long)fw.imgFactory, got0, got1,
        fw.factorySig0, fw.factorySig1,
        ok ? "MATCH" : "MISMATCH (refusing to call)");
    return ok;
}

std::shared_ptr<SceneItem> SceneImageItem::factoryCreate() {
    if (!checkFactorySignature()) {
        return std::shared_ptr<SceneItem>{};
    }

    // Signature: std::shared_ptr<SceneItemBase> create(int type_tag).
    // AArch64 C++ PCS: result types with non-trivial destructor or
    // copy ctor (std::shared_ptr has both) are returned via an
    // indirect pointer passed in x8 — NOT x0/x1, even when the type
    // fits in 16 bytes. The disasm confirms this: `mov x19, x8`
    // at e58b08, then `stp x3, x0, [x19]` at e58c28 writing the
    // 16-byte {ptr, control_block} pair to *x8.
    //
    // First version of this code declared the factory as
    // `void(int, std::shared_ptr<...>*)` — that puts the
    // out-pointer in x1, leaving x8 uninitialized; the function
    // wrote 16 bytes to a garbage address and SIGSEGV'd xochitl.
    using FactoryFn = std::shared_ptr<SceneItem>(*)(int);
    auto factory = reinterpret_cast<FactoryFn>(cropPaste::firmware().imgFactory);
    std::shared_ptr<SceneItem> sp = factory(FIRMWARE_FACTORY_TYPE_TAG_IMAGE);
    fprintf(stderr,
        "[cropPaste:img] factory returned shared_ptr: ptr=%p use_count=%ld\n",
        (void*)sp.get(), (long)sp.use_count());
    return sp;
}

void SceneImageItem::hexdumpDefaultInit(SceneItem* item) {
    if (!item) {
        fprintf(stderr, "[cropPaste:img] hexdump: null item\n");
        return;
    }
    auto* p = reinterpret_cast<const uint8_t*>(item);
    fprintf(stderr,
        "[cropPaste:img] === default-init SceneImageItem hexdump (%zu bytes) ===\n",
        OBJECT_SIZE);
    for (size_t off = 0; off < OBJECT_SIZE; off += kHexBytesPerRow) {
        size_t row = OBJECT_SIZE - off;
        if (row > kHexBytesPerRow) row = kHexBytesPerRow;
        hexdumpRow(p + off, off, row);
    }
    // Annotate the four well-understood fields right inline for easy reading.
    uint8_t  type_tag = p[OFF_TYPE_TAG];
    uint64_t bytes_b  = *reinterpret_cast<const uint64_t*>(p + OFF_BYTES_BEGIN);
    uint64_t bytes_e  = *reinterpret_cast<const uint64_t*>(p + OFF_BYTES_END);
    uint64_t bytes_es = *reinterpret_cast<const uint64_t*>(p + OFF_BYTES_END_STORAGE);
    const double* sb  = reinterpret_cast<const double*>(p + OFF_SOURCE_BOUNDS);
    const double* tx  = reinterpret_cast<const double*>(p + OFF_TRANSFORM);
    fprintf(stderr,
        "[cropPaste:img]   type_tag=%u (want 7)\n"
        "[cropPaste:img]   bytes_vec={begin=0x%lx end=0x%lx end_storage=0x%lx}\n"
        "[cropPaste:img]   source_bounds=(%g,%g,%g,%g)\n"
        "[cropPaste:img]   transform=[%g %g %g; %g %g %g; %g %g %g]\n",
        (unsigned)type_tag,
        (unsigned long)bytes_b, (unsigned long)bytes_e, (unsigned long)bytes_es,
        sb[0], sb[1], sb[2], sb[3],
        tx[0], tx[1], tx[2], tx[3], tx[4], tx[5], tx[6], tx[7], tx[8]);
}

bool SceneImageItem::setSourceBoundsF(SceneItem* item,
                                      double x, double y,
                                      double w, double h,
                                      bool heavyLog) {
    if (!item) return false;
    auto* dst = reinterpret_cast<double*>(
        reinterpret_cast<uint8_t*>(item) + OFF_SOURCE_BOUNDS);
    if (heavyLog) {
        fprintf(stderr,
            "[cropPaste:img] setSourceBoundsF @ 0x%zx: before=(%g,%g,%g,%g) "
            "after=(%g,%g,%g,%g)\n",
            OFF_SOURCE_BOUNDS, dst[0], dst[1], dst[2], dst[3], x, y, w, h);
    }
    dst[0] = x; dst[1] = y; dst[2] = w; dst[3] = h;
    return true;
}

bool SceneImageItem::setTransformScale(SceneItem* item, double scale,
                                       bool heavyLog) {
    if (!item) return false;
    auto* base = reinterpret_cast<uint8_t*>(item);
    auto* m = reinterpret_cast<double*>(base + OFF_TRANSFORM);
    auto* mtype = reinterpret_cast<uint16_t*>(base + OFF_TRANSFORM_M_TYPE);

    if (heavyLog) {
        fprintf(stderr,
            "[cropPaste:img] setTransformScale @ 0x%zx: scale=%g\n"
            "[cropPaste:img]   before: [%g %g %g; %g %g %g; %g %g %g] "
            "m_type@0x%zx=0x%04x\n",
            OFF_TRANSFORM, scale,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
            OFF_TRANSFORM_M_TYPE, (unsigned)*mtype);
    }

    m[0] = scale; m[1] = 0;     m[2] = 0;
    m[3] = 0;     m[4] = scale; m[5] = 0;
    m[6] = 0;     m[7] = 0;     m[8] = 1.0;

    uint16_t prev = *mtype;
    *mtype = static_cast<uint16_t>((prev & M_TYPE_MASK_BITS_KEEP) | M_TYPE_TX_SCALE);

    if (heavyLog) {
        fprintf(stderr,
            "[cropPaste:img]   after:  [%g %g %g; %g %g %g; %g %g %g] "
            "m_type=0x%04x (TxScale; bits 10-15 preserved from 0x%04x)\n",
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
            (unsigned)*mtype, (unsigned)(prev & M_TYPE_MASK_BITS_KEEP));
    }
    return true;
}

bool SceneImageItem::setInlineBytes(SceneItem* item,
                                    const uint8_t* bytes, size_t size,
                                    bool heavyLog) {
    if (!item || !bytes || size == 0) return false;

    // Match the destructor's deallocator: it tail-calls
    // _ZdlPvm = ::operator delete(void*, size_t). So allocate
    // with ::operator new(size_t) — NOT new uint8_t[size]
    // (which would mismatch and corrupt the heap on document
    // close).
    void* buf = ::operator new(size);
    std::memcpy(buf, bytes, size);

    auto* base = reinterpret_cast<uint8_t*>(item);
    auto* slot_b  = reinterpret_cast<uint8_t**>(base + OFF_BYTES_BEGIN);
    auto* slot_e  = reinterpret_cast<uint8_t**>(base + OFF_BYTES_END);
    auto* slot_es = reinterpret_cast<uint8_t**>(base + OFF_BYTES_END_STORAGE);

    if (heavyLog) {
        fprintf(stderr,
            "[cropPaste:img] setInlineBytes: alloc %zu via ::operator new -> %p\n",
            size, buf);
        fprintf(stderr,
            "[cropPaste:img]   before: begin=%p end=%p end_storage=%p\n",
            (void*)*slot_b, (void*)*slot_e, (void*)*slot_es);
    }

    auto* p = static_cast<uint8_t*>(buf);
    *slot_b  = p;
    *slot_e  = p + size;
    *slot_es = p + size;

    if (heavyLog) {
        fprintf(stderr,
            "[cropPaste:img]   after:  begin=%p end=%p end_storage=%p\n",
            (void*)*slot_b, (void*)*slot_e, (void*)*slot_es);
    }
    return true;
}

bool SceneImageItem::setImageUuid(SceneItem* item,
                                  const uint8_t uuid[16],
                                  bool heavyLog) {
    if (!item || !uuid) return false;
    auto* dst = reinterpret_cast<uint8_t*>(item) + OFF_IMAGE_UUID;
    if (heavyLog) {
        fprintf(stderr, "[cropPaste:img] setImageUuid @ 0x%zx:\n",
                OFF_IMAGE_UUID);
        fprintf(stderr, "  before: ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", dst[i]);
        fprintf(stderr, "\n  after:  ");
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", uuid[i]);
        fprintf(stderr, "\n");
    }
    std::memcpy(dst, uuid, 16);
    return true;
}

bool SceneImageItem::setImageKind(SceneItem* item,
                                  uint8_t kind, uint8_t subkind,
                                  bool heavyLog) {
    if (!item) return false;
    auto* base = reinterpret_cast<uint8_t*>(item);
    uint8_t before_k = base[0x28];
    uint8_t before_s = base[0x29];
    base[0x28] = kind;
    base[0x29] = subkind;
    if (heavyLog) {
        fprintf(stderr, "[cropPaste:img] setImageKind @ 0x28/0x29: "
                "before=(%u,%u) after=(%u,%u)\n",
                before_k, before_s, kind, subkind);
    }
    return true;
}
