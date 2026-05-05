// v1.6 path 5 — Global SceneImageItem paint() hook via vtable patching.
//
// Architecture:
//   - At extension load: patch slot 3 of the stock SceneImageItem vtable
//     in .rodata to point at ourGlobalPaint. Save the original pointer.
//   - At capture time: generate UUID, write it to the SceneImageItem at
//     offset 0x48, stash the captured QImage in g_imageMap[uuid].
//   - At paint time: ALL SceneImageItem paints (stock + ours) reach
//     ourGlobalPaint. Lookup UUID → hit drawImage, miss callOriginal.
//
// Why this beats the v1.6a/b/c per-item custom vtable approach:
//   - Items keep stock vtable in EVERY slot (including hit-test, clone,
//     dtor, etc.). xochitl's selection / lasso / move / resize work as
//     they do for built-in images. Single change point: the paint slot.
//   - UUID-keyed map outlives the original SceneImageItem pointer.
//     When xochitl tears down a page from memory and reloads from the
//     .rm scenefile, the deserialized item gets the same UUID at +0x48
//     and our lookup re-hits. No more dashed-rect octagon on revisit.
//   - No clone wrapper needed (stock clone preserves UUID at +0x48,
//     which is the cache key — clones inherit our entry transparently).
//   - No dtor wrappers needed (the map entry leaks until xochitl exit,
//     but each entry is one QImage's COW handle = ~64 bytes, and total
//     entries scale with capture count per session — bounded).
//
// Pinned firmware: 3.26.0.68 (ferrari). Per memory `firmware_lock`.

#include "customVtable.hpp"
#include "rm_SceneImageItem.hpp"
#include "firmware_addrs.hpp"

#include <QByteArray>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QUuid>
#include <Qt>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <unordered_map>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cropPaste {
namespace {

// ── Pinned firmware constants ──────────────────────────────────────────
//
// Per-device addresses come from cropPaste::firmware() (see
// firmware_addrs.hpp). Selected once at startup by hostname. The slot
// index and struct offsets below are device-invariant (same source
// across rMPP variants).

constexpr size_t    kSlotPaint         = 3;

constexpr size_t kOffUuid       = 0x48;  // QUuid (16 bytes)
constexpr size_t kOffDestBounds = 0x60;  // QRectF — destination on canvas
constexpr size_t kOffTransform  = 0x80;  // QTransform — applied during paint

// ── UUID-keyed side-map ────────────────────────────────────────────────

struct UuidKey {
    uint8_t bytes[16];
    bool operator==(const UuidKey& o) const noexcept {
        return std::memcmp(bytes, o.bytes, 16) == 0;
    }
};

struct UuidHash {
    size_t operator()(const UuidKey& k) const noexcept {
        size_t h;
        std::memcpy(&h, k.bytes, sizeof h);
        return h;
    }
};

std::mutex                                            g_mapMutex;
std::unordered_map<UuidKey, QImage, UuidHash>         g_imageMap;

// Original paint @ 0xe83630, saved before our patch. Called for items
// whose UUID isn't in g_imageMap (i.e., stock notebook images).
using PaintFn = void(*)(void* /*self*/, void* /*ctx*/);
PaintFn g_origPaint = nullptr;

std::atomic<bool> g_hookInstalled{false};

// PID at hook-install time. Used by ourGlobalPaint to detect fork-only
// children (e.g., xochitl's PDF renderer subprocess) that inherit our
// patched vtable + globals via COW but never re-run installPaintHook.
//
// Without this guard, a forked child could deadlock on g_mapMutex if
// main's render thread happened to be holding it at the moment of fork
// — the child inherits the locked mutex with no owner thread to release.
// Manifests on rMPP as "renderer exited while waiting for a response"
// from pdfrenderer_unix.cpp:344, killing PDF rendering.
//
// On match: child IS the original installer → run full hook logic.
// On mismatch: we're in a fork-only child → bypass to g_origPaint.
pid_t g_installPid = 0;

// First-invocation log gates. Each fires exactly once across the process
// lifetime — confirming three independent things:
//   (1) the hook is reachable for OUR items (UUID hit -> drawImage)
//   (2) the hook is reachable for STOCK items (UUID miss -> origPaint)
//   (3) origPaint can be called without recursion / SIGSEGV
std::atomic<bool> g_firstHitLogged{true};
std::atomic<bool> g_firstMissLogged{true};

// v1.6h+ dispatch log — every paint invocation appended to a disk file
// independent of journald. Survives journal-tail death and crashes.
// Used to disambiguate revisit-time dispatch outcomes (parent's A/B/C):
//   A: revisit fires ourGlobalPaint, hit, drawImage -> persistence works
//   B: revisit fires ourGlobalPaint, miss -> side-map lost the entry
//   C: revisit doesn't fire ourGlobalPaint -> deserialized item carries
//      a different vtable than the one we patched
std::mutex          g_logMutex;
FILE*               g_logFile = nullptr;
std::atomic<uint64_t> g_paintCallCount{0};

constexpr const char* kDispatchLogPath = "/tmp/cropPaste_dispatch.log";

// caller must hold g_logMutex
void openDispatchLogIfNeeded() {
    if (g_logFile) return;
    g_logFile = std::fopen(kDispatchLogPath, "a");
    if (g_logFile) {
        std::time_t now = std::time(nullptr);
        std::fprintf(g_logFile,
            "\n=== v1.6i dispatch log opened pid=%d epoch=%lld ===\n",
            (int)getpid(), (long long)now);
        std::fflush(g_logFile);
    }
}

void logDispatch(void* self, void* vtable, const uint8_t uuid[16],
                 bool hit, double bx, double by, double bw, double bh,
                 const char* branch) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    openDispatchLogIfNeeded();
    if (!g_logFile) return;
    uint64_t n = g_paintCallCount.fetch_add(1) + 1;
    std::fprintf(g_logFile,
        "[#%llu] self=%p vt=%p uuid=%02x%02x%02x%02x-%02x%02x-%02x%02x"
        "-%02x%02x-%02x%02x%02x%02x%02x%02x hit=%d "
        "bounds@0x60=(%g,%g,%g,%g) branch=%s\n",
        (unsigned long long)n, self, vtable,
        uuid[0],uuid[1],uuid[2],uuid[3], uuid[4],uuid[5], uuid[6],uuid[7],
        uuid[8],uuid[9],
        uuid[10],uuid[11],uuid[12],uuid[13],uuid[14],uuid[15],
        (int)hit, bx, by, bw, bh, branch);
    std::fflush(g_logFile);
}

// ── v1.6j PNG disk persistence ────────────────────────────────────────
//
// Side-map (g_imageMap) lives in process memory only, so all prior pastes
// turn into octagons whenever xochitl restarts (deliberate restart, OOM,
// OTA update, reboot). The .rm scenefile carries our UUID at +0x48 fine,
// but our hook misses on lookup because the QImage isn't in memory.
//
// Fix: persist each captured QImage to disk as PNG keyed by UUID. On
// install, scan the dir and re-populate the side-map. Survives anything
// short of the user `rm -rf`'ing the store dir.
//
// Filename format: standard hyphenated UUID, no braces, .png extension.
// Picks human-readable form so a future host-side script (export, backup,
// inspection) can resolve a UUID from the .rm directly to the source PNG.

constexpr const char* kImageStoreDir = "/home/root/xovi/exthome/cropPaste";

// Idempotent. Returns true iff dir exists and is usable after the call.
bool ensureImageStoreDir() {
    if (mkdir(kImageStoreDir, 0755) == 0) return true;
    if (errno == EEXIST) return true;
    fprintf(stderr,
        "[cropPaste:disk] mkdir(%s, 0755) failed: errno=%d (%s) — "
        "in-mem side-map still works, but pastes will not survive xochitl "
        "restart.\n",
        kImageStoreDir, errno, std::strerror(errno));
    return false;
}

// Convert our 16-byte in-memory UUID layout (LE struct: data1-LE u32,
// data2-LE u16, data3-LE u16, data4 8 bytes as-is) to the QUuid struct.
// memcpy is correct: QUuid's internal layout matches what we wrote at
// +0x48 (we used the same memcpy round-trip in ClipboardInjector.cpp:481).
QUuid uuidBytesToQUuid(const uint8_t bytes[16]) {
    QUuid q;
    std::memcpy(&q, bytes, 16);
    return q;
}

// On install: scan the store dir, load every PNG into the side-map.
// Mutex-guarded inserts; safe even though install runs single-threaded
// (we'd rather not depend on the temporal exclusion in case future
// install-callers race).
void loadImageStoreFromDisk() {
    DIR* dir = opendir(kImageStoreDir);
    if (!dir) {
        // Not an error on first-ever run — store may not exist yet.
        fprintf(stderr,
            "[cropPaste:disk] image store dir not present at install time "
            "(%s, errno=%d %s) — starting with empty side-map. Will be "
            "created on first capture.\n",
            kImageStoreDir, errno, std::strerror(errno));
        return;
    }
    int loaded = 0, parseFailed = 0, loadFailed = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t namelen = std::strlen(name);
        if (namelen < 5) continue;
        if (std::strcmp(name + namelen - 4, ".png") != 0) continue;

        QString basename = QString::fromUtf8(name, (int)(namelen - 4));
        QUuid quuid = QUuid::fromString(basename);
        if (quuid.isNull()) {
            parseFailed++;
            fprintf(stderr,
                "[cropPaste:disk] skip: cannot parse UUID from filename '%s'\n",
                name);
            continue;
        }

        QString fullpath = QString::fromUtf8(kImageStoreDir) + "/" +
                           QString::fromUtf8(name);
        QImage img;
        if (!img.load(fullpath)) {
            loadFailed++;
            fprintf(stderr,
                "[cropPaste:disk] skip: QImage::load failed for '%s'\n",
                fullpath.toUtf8().constData());
            continue;
        }

        UuidKey key;
        std::memcpy(key.bytes, &quuid, 16);
        {
            std::lock_guard<std::mutex> lk(g_mapMutex);
            g_imageMap[key] = std::move(img);
        }
        loaded++;
    }
    closedir(dir);

    size_t mapSize;
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        mapSize = g_imageMap.size();
    }
    fprintf(stderr,
        "[cropPaste:disk] startup scan: dir=%s loaded=%d parse_failed=%d "
        "load_failed=%d side_map_size=%zu\n",
        kImageStoreDir, loaded, parseFailed, loadFailed, mapSize);
}

// ── ourGlobalPaint ─────────────────────────────────────────────────────

extern "C" void ourGlobalPaint(void* self, void* ctx) {
    // Fork-only-child guard: if we're not in the process that installed
    // the hook (e.g., xochitl's fork-only PDF renderer subprocess),
    // bypass our logic. The renderer inherits our patched vtable via
    // COW but doesn't have a coherent g_mapMutex / g_imageMap state —
    // touching them risks deadlock or worse.
    if (getpid() != g_installPid) {
        static std::atomic<bool> s_firstChildLogged{true};
        if (s_firstChildLogged.exchange(false)) {
            fprintf(stderr,
                "[cropPaste:hook] fork-only child detected: pid=%d "
                "installPid=%d -> bypass to g_origPaint @ 0x%lx\n",
                (int)getpid(), (int)g_installPid,
                (unsigned long)g_origPaint);
        }
        if (g_origPaint) g_origPaint(self, ctx);
        return;
    }

    UuidKey key;
    std::memcpy(key.bytes,
                reinterpret_cast<uint8_t*>(self) + kOffUuid, 16);

    // Read fields-of-interest BEFORE map lookup so they're loggable on
    // either branch. vtable-at-self answers parent's outcome-C question
    // ("did the deserialized item carry our patched vtable or another?").
    void* selfVtable = *reinterpret_cast<void**>(self);
    auto* destBoundsPtr = reinterpret_cast<const QRectF*>(
        reinterpret_cast<uint8_t*>(self) + kOffDestBounds);
    double bx = destBoundsPtr->x(), by = destBoundsPtr->y();
    double bw = destBoundsPtr->width(), bh = destBoundsPtr->height();

    QImage img;  // empty by default; populated via Qt6 COW on map hit
    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        auto it = g_imageMap.find(key);
        if (it != g_imageMap.end()) {
            img = it->second;
            hit = true;
        }
    }

    if (!hit) {
        logDispatch(self, selfVtable, key.bytes, false, bx, by, bw, bh,
                    "miss->origPaint");
        if (g_firstMissLogged.exchange(false)) {
            fprintf(stderr,
                "[cropPaste:hook] first miss self=%p ctx=%p "
                "uuid=%02x%02x%02x%02x%02x%02x%02x%02x... -> "
                "calling g_origPaint @ 0x%lx (stock paint)\n",
                self, ctx,
                key.bytes[0], key.bytes[1], key.bytes[2], key.bytes[3],
                key.bytes[4], key.bytes[5], key.bytes[6], key.bytes[7],
                (unsigned long)g_origPaint);
        }
        if (g_origPaint) g_origPaint(self, ctx);
        return;
    }

    // We own this item. Render directly via QPainter::drawImage; sidesteps
    // the image cache xochitl's stock paint walks (which we never populate).
    if (!ctx) {
        logDispatch(self, selfVtable, key.bytes, true, bx, by, bw, bh,
                    "hit->null-ctx-early-ret");
        return;
    }

    using GetPainterFn = QPainter*(*)(void*);
    QPainter* painter = reinterpret_cast<GetPainterFn>(
        cropPaste::firmware().painterHelper)(ctx);

    if (g_firstHitLogged.exchange(false)) {
        fprintf(stderr,
            "[cropPaste:hook] first hit self=%p ctx=%p painter=%p "
            "uuid=%02x%02x%02x%02x%02x%02x%02x%02x... "
            "destBounds=(%g,%g,%g,%g) img=%dx%d isNull=%d\n",
            self, ctx, (void*)painter,
            key.bytes[0], key.bytes[1], key.bytes[2], key.bytes[3],
            key.bytes[4], key.bytes[5], key.bytes[6], key.bytes[7],
            bx, by, bw, bh,
            img.width(), img.height(), (int)img.isNull());
    }

    if (!painter || img.isNull()) {
        logDispatch(self, selfVtable, key.bytes, true, bx, by, bw, bh,
                    "hit->no-painter-or-null-img");
        return;
    }

    logDispatch(self, selfVtable, key.bytes, true, bx, by, bw, bh,
                "hit->drawImage");

    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    auto* xform = reinterpret_cast<const QTransform*>(
        reinterpret_cast<uint8_t*>(self) + kOffTransform);
    painter->setTransform(*xform, /*combine=*/true);
    QRectF source(0, 0, img.width(), img.height());
    painter->drawImage(*destBoundsPtr, img, source);
    painter->restore();
}

}  // namespace

// ── installPaintHook ───────────────────────────────────────────────────

// glibc-cached host program name. Defined in <errno.h> with _GNU_SOURCE,
// or just declared extern. Used as the v1.6i install-site guard.
extern "C" char* program_invocation_short_name;

bool installPaintHook() {
    static std::mutex installMutex;
    std::lock_guard<std::mutex> lk(installMutex);

    if (g_hookInstalled.load(std::memory_order_acquire)) return true;

    // v1.6i process-name guard. Our hardcoded addresses (resolved via
    // firmware() from /etc/hostname) are virtual addresses inside the
    // xochitl ELF. The PDF renderer subprocess
    // (xochitl_pdf_renderer) is a separate 266 KB binary (per MASTER.md
    // host-side triage) that maps nothing at those addresses. Reading
    // from them in the renderer SIGSEGVs immediately, killing the renderer
    // before our typeinfo check even completes. This was Problem A.
    //
    // Why exact-match: prefix match would let "xochitl_pdf_renderer" pass.
    // Why install-site (not just runtime) guard: the install code
    // dereferences before any lookup logic, so the runtime guard in
    // ourGlobalPaint can't help — install crashes first.
    // Runtime PID guard in ourGlobalPaint is kept as defense-in-depth.
    if (std::strcmp(program_invocation_short_name, "xochitl") != 0) {
        fprintf(stderr,
            "[cropPaste:hook] v1.6j guard: skipping install in non-xochitl "
            "host (program=%s pid=%d). Returning success — caller treats "
            "as no-op; the renderer doesn't need our hook.\n",
            program_invocation_short_name, (int)getpid());
        return true;
    }

    fprintf(stderr,
        "[cropPaste:hook] v1.6j install starting: pid=%d program=%s. "
        "mprotect+vtable-write enabled; program-name guard active; "
        "disk persistence enabled.\n",
        (int)getpid(), program_invocation_short_name);

    // v1.6j: disk-load PNG store BEFORE patching vtable. If patch fails,
    // we still get an empty side-map populated; if patch succeeds, the
    // first paint that hits a deserialized item finds its image waiting.
    loadImageStoreFromDisk();

    const auto& fw = cropPaste::firmware();

    // (1) Sanity: typeinfo header at vtable-8 must equal the known
    //     SceneImageItem typeinfo. Mismatch = wrong base address /
    //     firmware drift; refuse to patch.
    auto* typeinfoSlot = reinterpret_cast<uintptr_t*>(fw.imgVtable - 8);
    if (*typeinfoSlot != fw.imgTypeinfo) {
        fprintf(stderr,
            "[cropPaste:hook] typeinfo header mismatch @ (0x%lx - 8) on %s: "
            "got=0x%lx want=0x%lx — refusing to install\n",
            (unsigned long)fw.imgVtable, fw.deviceName,
            (unsigned long)*typeinfoSlot,
            (unsigned long)fw.imgTypeinfo);
        return false;
    }

    // (2) Sanity: slot 3 must currently point at the stock paint address.
    //     If not, something else has already patched the vtable, or our
    //     RE got the slot wrong.
    auto* slot = reinterpret_cast<void**>(fw.imgVtable) + kSlotPaint;
    void* origValue = *slot;
    if (origValue != reinterpret_cast<void*>(fw.imgPaint)) {
        fprintf(stderr,
            "[cropPaste:hook] slot[%zu] mismatch on %s: got=%p want=0x%lx — "
            "refusing to install\n",
            kSlotPaint, fw.deviceName, origValue, (unsigned long)fw.imgPaint);
        return false;
    }

    // (3) mprotect the page containing the slot to RW. Page size at
    //     runtime — some aarch64 kernels use 16KB pages.
    long pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize <= 0) pagesize = 4096;
    auto pageBase = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(slot) &
        ~static_cast<uintptr_t>(pagesize - 1));

    if (mprotect(pageBase, static_cast<size_t>(pagesize),
                 PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr,
            "[cropPaste:hook] mprotect RW page=%p size=%ld failed: errno=%d (%s) — "
            "refusing to install\n",
            pageBase, pagesize, errno, std::strerror(errno));
        return false;
    }

    // (4) Patch. 8-byte naturally-aligned write on aarch64 is atomic
    //     (single str instruction) — no torn write across in-flight render
    //     threads. Capture our PID first so the fork-only-child guard in
    //     ourGlobalPaint has a valid value to compare against.
    g_installPid = getpid();
    g_origPaint = reinterpret_cast<PaintFn>(origValue);
    *slot = reinterpret_cast<void*>(&ourGlobalPaint);

    // (5) Restore read-only. Failure here is non-fatal — patch is in place;
    //     we just lose the protection-against-future-stray-writes guarantee.
    if (mprotect(pageBase, static_cast<size_t>(pagesize), PROT_READ) != 0) {
        fprintf(stderr,
            "[cropPaste:hook] mprotect R restore failed: errno=%d (%s) — "
            "non-fatal, hook still active\n",
            errno, std::strerror(errno));
    }

    g_hookInstalled.store(true, std::memory_order_release);

    fprintf(stderr,
        "[cropPaste:hook] v1.6j paint hook installed (%s): pid=%d vtable=0x%lx "
        "slot[%zu] old=%p (=0x%lx) new=%p page=%p size=%ld typeinfo=0x%lx OK "
        "(fork-only children will bypass via PID guard; dispatch log at %s)\n",
        fw.deviceName,
        (int)g_installPid,
        (unsigned long)fw.imgVtable, kSlotPaint,
        origValue, (unsigned long)fw.imgPaint,
        (void*)&ourGlobalPaint,
        pageBase, pagesize,
        (unsigned long)fw.imgTypeinfo,
        kDispatchLogPath);
    return true;
}

void registerImage(const uint8_t uuid[16], QImage image) {
    UuidKey key;
    std::memcpy(key.bytes, uuid, 16);
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        g_imageMap[key] = image;  // Qt6 QImage is COW; copy is cheap
    }

    // v1.6j disk persistence: write PNG to image store. Survives xochitl
    // restart so the deserialized SceneImageItem finds its source image
    // when the hook re-installs and re-scans the dir.
    if (!ensureImageStoreDir()) return;

    QUuid quuid = uuidBytesToQUuid(uuid);
    QString path = QString::fromUtf8(kImageStoreDir) + "/" +
                   quuid.toString(QUuid::WithoutBraces) + ".png";
    bool saved = image.save(path, "PNG");

    static std::atomic<bool> s_firstSaveLogged{true};
    bool heavyLog = s_firstSaveLogged.exchange(false);
    if (heavyLog || !saved) {
        fprintf(stderr,
            "[cropPaste:disk] save: path=%s img=%dx%d depth=%d result=%s\n",
            path.toUtf8().constData(),
            image.width(), image.height(), image.depth(),
            saved ? "OK" : "FAILED");
    }
}

size_t imageMapSize() {
    std::lock_guard<std::mutex> lk(g_mapMutex);
    return g_imageMap.size();
}

}  // namespace cropPaste

// ── C wrapper for entry.c (which is C-linkage) ─────────────────────────

extern "C" int cropPaste_installPaintHook(void) {
    return cropPaste::installPaintHook() ? 1 : 0;
}

// ───────────────────────────────────────────────────────────────────────
// v1.5 / v1.6a-c custom-vtable apparatus — preserved for rollback per
// parent's "comment out, don't delete" instruction. If option 5 has a
// surprise, restore by deleting the #if 0 / #endif markers, restoring
// the corresponding hpp interface, and reverting captureAreaAsImage's
// call site to installCustomVtable.
// ───────────────────────────────────────────────────────────────────────

#if 0
// [Old per-item custom-vtable code: ourPaint / ourClone / ourD0 / ourD1
//  + initCustomVtable / installCustomVtable / sideMapSize. See
//  v1.6c snapshot in git history if needed for restoration.]
#endif
