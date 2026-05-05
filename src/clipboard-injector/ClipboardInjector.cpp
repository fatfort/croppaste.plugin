#include "ClipboardInjector.hpp"
#include "rm_SceneLineItem.hpp"
#include "rm_SceneImageItem.hpp"
#include "customVtable.hpp"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QImage>
#include <QByteArray>
#include <QUuid>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <vector>
#include <algorithm>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

/* ── C bridge to framebuffer-spy (defined in entry.c) ──────────────── */
extern "C" {
    void ci_getFramebufferInfo(void **addr, int *width, int *height,
                               int *type, int *bpl);
}

#define FBSPY_TYPE_RGB565 1
#define FBSPY_TYPE_RGBA   2

/* ── NEON-optimized image processing ──────────────────────────────── */

/*
 * BGRA → Grayscale (NEON: 8 pixels/cycle)
 * Fixed-point: gray = (29·B + 150·G + 77·R) >> 8
 * ≈ 0.114·B + 0.587·G + 0.299·R
 */
static void bgraToGray(const uint8_t *bgra, uint8_t *gray,
                       int pixelCount) {
#ifdef __ARM_NEON
    const uint8x8_t wB = vdup_n_u8(29);
    const uint8x8_t wG = vdup_n_u8(150);
    const uint8x8_t wR = vdup_n_u8(77);

    int i = 0;
    for (; i + 8 <= pixelCount; i += 8) {
        /* vld4 deinterleaves 8 BGRA quads into 4 × uint8x8 */
        uint8x8x4_t px = vld4_u8(bgra + i * 4);
        uint16x8_t acc = vmull_u8(px.val[0], wB);   /* B */
        acc = vmlal_u8(acc, px.val[1], wG);           /* G */
        acc = vmlal_u8(acc, px.val[2], wR);           /* R */
        vst1_u8(gray + i, vshrn_n_u16(acc, 8));
    }
    for (; i < pixelCount; i++) {
        gray[i] = (uint8_t)((29*bgra[i*4] + 150*bgra[i*4+1]
                              + 77*bgra[i*4+2]) >> 8);
    }
#else
    for (int i = 0; i < pixelCount; i++) {
        gray[i] = (uint8_t)((29*bgra[i*4] + 150*bgra[i*4+1]
                              + 77*bgra[i*4+2]) >> 8);
    }
#endif
}

/*
 * RGB565 → Grayscale (NEON: 8 pixels/cycle)
 */
static void rgb565ToGray(const uint16_t *rgb565, uint8_t *gray,
                         int pixelCount) {
#ifdef __ARM_NEON
    int i = 0;
    for (; i + 8 <= pixelCount; i += 8) {
        uint16x8_t px = vld1q_u16(rgb565 + i);
        /* Extract channels */
        uint16x8_t r5 = vshrq_n_u16(px, 11);
        uint16x8_t g6 = vandq_u16(vshrq_n_u16(px, 5), vdupq_n_u16(0x3F));
        uint16x8_t b5 = vandq_u16(px, vdupq_n_u16(0x1F));
        /* Scale to 8-bit and compute luminance (fixed-point) */
        uint16x8_t r8 = vmulq_n_u16(r5, 255/31);
        uint16x8_t g8 = vmulq_n_u16(g6, 255/63);
        uint16x8_t b8 = vmulq_n_u16(b5, 255/31);
        /* gray = (77*R + 150*G + 29*B) >> 8 */
        uint16x8_t lum = vmulq_n_u16(r8, 77);
        lum = vmlaq_n_u16(lum, g8, 150);
        lum = vmlaq_n_u16(lum, b8, 29);
        lum = vshrq_n_u16(lum, 8);
        vst1_u8(gray + i, vmovn_u16(lum));
    }
    for (; i < pixelCount; i++) {
        uint16_t p = rgb565[i];
        uint8_t r = ((p >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((p >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (p & 0x1F) * 255 / 31;
        gray[i] = (uint8_t)((77*r + 150*g + 29*b) >> 8);
    }
#else
    for (int i = 0; i < pixelCount; i++) {
        uint16_t p = rgb565[i];
        uint8_t r = ((p >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((p >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (p & 0x1F) * 255 / 31;
        gray[i] = (uint8_t)((77*r + 150*g + 29*b) >> 8);
    }
#endif
}

/*
 * Gaussian 3×3 blur  —  kernel {1,2,1; 2,4,2; 1,2,1} / 16
 * NEON: processes 8 output pixels per iteration
 */
[[maybe_unused]] static void gaussianBlur3x3(uint8_t *img, int w, int h) {
    uint8_t *tmp = (uint8_t *)malloc(w * h);
    if (!tmp) return;
    memcpy(tmp, img, w * h);

    for (int y = 1; y < h - 1; y++) {
        const uint8_t *r0 = tmp + (y - 1) * w;
        const uint8_t *r1 = tmp + y * w;
        const uint8_t *r2 = tmp + (y + 1) * w;
        int x = 1;
#ifdef __ARM_NEON
        for (; x + 8 <= w - 1; x += 8) {
            /* Load 10-pixel spans for the three rows (x-1 .. x+8) */
            uint8x8_t a0 = vld1_u8(r0 + x - 1);
            uint8x8_t a1 = vld1_u8(r0 + x);
            uint8x8_t a2 = vld1_u8(r0 + x + 1);
            uint8x8_t b0 = vld1_u8(r1 + x - 1);
            uint8x8_t b1 = vld1_u8(r1 + x);
            uint8x8_t b2 = vld1_u8(r1 + x + 1);
            uint8x8_t c0 = vld1_u8(r2 + x - 1);
            uint8x8_t c1 = vld1_u8(r2 + x);
            uint8x8_t c2 = vld1_u8(r2 + x + 1);

            /* Widen to 16-bit and apply kernel weights */
            uint16x8_t sum = vaddl_u8(a0, a2);       /* 1*tl + 1*tr */
            sum = vmlal_u8(sum, a1, vdup_n_u8(2));    /* 2*tc */
            sum = vaddw_u8(sum, c0);                  /* 1*bl */
            sum = vaddw_u8(sum, c2);                  /* 1*br */
            sum = vmlal_u8(sum, c1, vdup_n_u8(2));    /* 2*bc */
            sum = vmlal_u8(sum, b0, vdup_n_u8(2));    /* 2*ml */
            sum = vmlal_u8(sum, b2, vdup_n_u8(2));    /* 2*mr */
            sum = vmlal_u8(sum, b1, vdup_n_u8(4));    /* 4*mc */

            vst1_u8(img + y * w + x, vshrn_n_u16(sum, 4));
        }
#endif
        for (; x < w - 1; x++) {
            int s = r0[x-1] + 2*r0[x] + r0[x+1]
                  + 2*r1[x-1] + 4*r1[x] + 2*r1[x+1]
                  + r2[x-1] + 2*r2[x] + r2[x+1];
            img[y * w + x] = (uint8_t)(s >> 4);
        }
    }
    free(tmp);
}

/*
 * Laplacian 3×3 edge detection  —  kernel {1,4,1; 4,-20,4; 1,4,1}
 * NEON: processes 8 output pixels per iteration using 16-bit signed math
 */
static void laplacianFilter(const uint8_t *in, uint8_t *out, int w, int h) {
    memset(out, 0, w * h);
    for (int y = 1; y < h - 1; y++) {
        const uint8_t *r0 = in + (y - 1) * w;
        const uint8_t *r1 = in + y * w;
        const uint8_t *r2 = in + (y + 1) * w;
        int x = 1;
#ifdef __ARM_NEON
        for (; x + 8 <= w - 1; x += 8) {
            /* Load shifted spans */
            int16x8_t a0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0+x-1)));
            int16x8_t a1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0+x)));
            int16x8_t a2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0+x+1)));
            int16x8_t b0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1+x-1)));
            int16x8_t b1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1+x)));
            int16x8_t b2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1+x+1)));
            int16x8_t c0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r2+x-1)));
            int16x8_t c1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r2+x)));
            int16x8_t c2 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r2+x+1)));

            /* kernel: 1*corners + 4*edges − 20*center */
            int16x8_t sum = vaddq_s16(a0, a2);
            sum = vaddq_s16(sum, c0);
            sum = vaddq_s16(sum, c2);
            sum = vmlaq_n_s16(sum, a1, 4);
            sum = vmlaq_n_s16(sum, b0, 4);
            sum = vmlaq_n_s16(sum, b2, 4);
            sum = vmlaq_n_s16(sum, c1, 4);
            sum = vmlaq_n_s16(sum, b1, -20);

            /* Clamp to [0,255] */
            sum = vmaxq_s16(sum, vdupq_n_s16(0));
            sum = vminq_s16(sum, vdupq_n_s16(255));
            vst1_u8(out + y*w + x, vmovn_u16(vreinterpretq_u16_s16(sum)));
        }
#endif
        for (; x < w - 1; x++) {
            int s = r0[x-1] + 4*r0[x] + r0[x+1]
                  + 4*r1[x-1] - 20*r1[x] + 4*r1[x+1]
                  + r2[x-1] + 4*r2[x] + r2[x+1];
            out[y*w + x] = (uint8_t)(s > 255 ? 255 : (s < 0 ? 0 : s));
        }
    }
}

/*
 * Horizontal line extraction from binary edge image.
 * Returns a flat float array: [x1,y1,x2,y2, ...] and sets *outCount.
 */
static float *extractHorizontalLines(const uint8_t *edge,
                                      int w, int h, int *outCount) {
    /* Worst case: every pixel is a 1px line */
    int cap = w * h;
    float *lines = (float *)malloc(cap * 4 * sizeof(float));
    int n = 0;

    for (int y = 0; y < h; y++) {
        bool inLine = false;
        int from = 0, to = 0;
        for (int x = 0; x < w; x++) {
            bool px = edge[y * w + x] > 0;
            if (inLine) {
                if (px) {
                    to = x;
                    if (x + 1 == w) {
                        lines[n++] = (float)from;
                        lines[n++] = (float)y;
                        lines[n++] = (float)to;
                        lines[n++] = (float)y;
                        inLine = false;
                    }
                } else {
                    lines[n++] = (float)from;
                    lines[n++] = (float)y;
                    lines[n++] = (float)to;
                    lines[n++] = (float)y;
                    inLine = false;
                }
            } else if (px) {
                from = x;
                to = x;
                if (x + 1 == w) {
                    lines[n++] = (float)from;
                    lines[n++] = (float)y;
                    lines[n++] = (float)to;
                    lines[n++] = (float)y;
                } else {
                    inLine = true;
                }
            }
        }
    }
    *outCount = n / 4;
    return lines;
}

/*
 * Serialize lines to the clipboard JSON format and write to disk.
 */
static bool writeClipboardJSON(const float *lines, int lineCount,
                                const char *path) {
    QJsonArray arr;
    for (int i = 0; i < lineCount; i++) {
        float x1 = lines[i*4], y1 = lines[i*4+1];
        float x2 = lines[i*4+2], y2 = lines[i*4+3];

        QJsonArray p1, p2;
        p1 << x1 << y1 << 25 << 8 << 0 << 255;
        p2 << x2 << y2 << 25 << 8 << 0 << 255;

        QJsonArray pts;
        pts << p1 << p2;

        float minX = std::min(x1, x2), minY = std::min(y1, y2);
        float w = std::abs(x2 - x1), h = std::abs(y2 - y1);
        if (w < 1) w = 1;
        if (h < 1) h = 1;

        QJsonArray bounds;
        bounds << minX << minY << w << h;

        QJsonObject obj;
        obj["points"] = pts;
        obj["rgba"] = (double)0xFF000000u;
        obj["color"] = 0;
        obj["bounds"] = bounds;
        obj["tool"] = 17; // FINELINER_2 (from rmscene, fixed-width non-aliased)
        obj["maskScale"] = 1.0;
        obj["thickness"] = 0.1; // Extremely thin to avoid blurring
        arr << obj;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    f.close();
    return true;
}


/* ── Phase 2 image-paste path ─────────────────────────────────────── */

// Minimal 1x1 black PNG (67 bytes), generated by zlib compress of a single
// 0x00 pixel byte preceded by a 0x00 filter byte.
static const uint8_t kTestPng1x1Black[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x3a, 0x7e, 0x9b, 0x55, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x01, 0xe5, 0x27, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
static constexpr size_t kTestPng1x1BlackSize = 67;

QList<std::shared_ptr<SceneItem>> ClipboardInjector::pasteTestImage() {
    QList<std::shared_ptr<SceneItem>> items;

    fprintf(stderr, "[cropPaste:img] === pasteTestImage: phase 2 first deploy ===\n");

    auto sp = SceneImageItem::factoryCreate();
    if (!sp) {
        fprintf(stderr, "[cropPaste:img] factoryCreate returned null — aborting paste\n");
        return items;
    }

    SceneItem* obj = sp.get();
    SceneImageItem::hexdumpDefaultInit(obj);

    // Mutate the four well-understood fields. Heavy-log every write.
    // Source bounds = the SOURCE-pixel rect of the image (0,0,1,1 for
    // a 1x1 PNG). Transform scales source-pixel-space → scene units;
    // 200 makes the visible square 200x200 scene units. Together they
    // give an unambiguous block in any pasted notebook.
    SceneImageItem::setSourceBoundsF(obj, 0.0, 0.0, 1.0, 1.0,
                                     /*heavyLog=*/true);
    SceneImageItem::setTransformScale(obj, 200.0, /*heavyLog=*/true);
    SceneImageItem::setInlineBytes(obj, kTestPng1x1Black, kTestPng1x1BlackSize,
                                   /*heavyLog=*/true);

    fprintf(stderr, "[cropPaste:img] === post-mutation hexdump ===\n");
    SceneImageItem::hexdumpDefaultInit(obj);

    items.append(sp);
    fprintf(stderr, "[cropPaste:img] returning %lld scene item(s)\n",
            (long long)items.size());
    return items;
}


/* ── v1.6: color-raster paste via custom-vtable paint() override ───────
 *
 * Captures the framebuffer rect as a color QImage, constructs a
 * SceneImageItem via xochitl's factory, then swaps slots 0/1/2/3 of the
 * vtable to our overrides (see customVtable.cpp). Our paint() draws the
 * QImage directly onto the QPainter handed in by xochitl's render loop —
 * no image cache lookup, no on-disk asset round-trip, no xochitl restart.
 *
 * The QImage is stashed in a pointer-keyed side map. Clone duplicates the
 * entry under the cloned pointer; destructors erase. Lifetime tied to
 * the SceneImageItem's shared_ptr refcount via the complete destructor
 * (vt[0]).
 *
 * v1.5 dead-end paths (UUID at this+0x48, .thumbnails shotgun, inline
 * bytes vector at 0x30, kind-byte cycling) are stripped — none of them
 * affect what stock paint draws. Path 2 sidesteps the cache entirely. */

QList<std::shared_ptr<SceneItem>> ClipboardInjector::captureAreaAsImage(
        int rx, int ry, int rw, int rh) {
    static bool s_firstCall = true;
    bool heavyLog = s_firstCall;
    s_firstCall = false;

    QList<std::shared_ptr<SceneItem>> items;

    fprintf(stderr,
        "[cropPaste:img] === captureAreaAsImage(%d,%d,%d,%d) heavy=%d ===\n",
        rx, ry, rw, rh, (int)heavyLog);

    // 1. Framebuffer.
    void *fbAddr = nullptr;
    int   fbW = 0, fbH = 0, fbType = 0, fbBpl = 0;
    ci_getFramebufferInfo(&fbAddr, &fbW, &fbH, &fbType, &fbBpl);
    if (!fbAddr || fbW <= 0 || fbH <= 0) {
        fprintf(stderr, "[cropPaste:img] no framebuffer!\n");
        return items;
    }
    if (fbType != FBSPY_TYPE_RGBA) {
        fprintf(stderr,
            "[cropPaste:img] FB type %d unsupported (need %d=BGRA on rMPP/Move)\n",
            fbType, FBSPY_TYPE_RGBA);
        return items;
    }

    // 2. Clamp the requested rect to FB bounds.
    int x0 = std::max(0, rx);
    int y0 = std::max(0, ry);
    int x1 = std::min(fbW, rx + rw);
    int y1 = std::min(fbH, ry + rh);
    int cw = x1 - x0;
    int ch = y1 - y0;
    if (cw <= 2 || ch <= 2) {
        fprintf(stderr, "[cropPaste:img] region too small after clamp (%dx%d)\n",
                cw, ch);
        return items;
    }
    fprintf(stderr,
        "[cropPaste:img] crop %dx%d @ (%d,%d) from FB %dx%d bpl=%d\n",
        cw, ch, x0, y0, fbW, fbH, fbBpl);

    // 3. Wrap the FB and detach the cropped subregion as a real color QImage.
    //    Format_RGB32 has the same memory layout as ARGB32 (BGRA bytes on
    //    little-endian aarch64) but with alpha-ignored on draw, so the
    //    panel's alpha byte (whatever it is) doesn't bleed through.
    //    .copy() detaches from the FB memory so subsequent FB writes can't
    //    race our paint thread.
    QImage fbImage(reinterpret_cast<const uchar*>(fbAddr),
                   fbW, fbH, fbBpl, QImage::Format_RGB32);
    QImage captured = fbImage.copy(x0, y0, cw, ch);
    if (captured.isNull()) {
        fprintf(stderr, "[cropPaste:img] QImage::copy failed\n");
        return items;
    }

    // 4. Diagnostic: persist the captured image to /tmp so we can SCP it back
    //    and visually verify what the FB actually contained at capture time.
    //    Color is preserved end-to-end; v1.5's grayscale conversion is dropped
    //    (we want the user's highlights/colors to render as they were).
    if (heavyLog) {
        captured.save("/tmp/cropPaste_dbg_capture.png", "PNG");
        fprintf(stderr, "[cropPaste:img] dbg PNG written to /tmp/cropPaste_dbg_capture.png\n");
    }

    // 5. Construct SceneImageItem via xochitl's case-15 factory.
    auto sp = SceneImageItem::factoryCreate();
    if (!sp) {
        fprintf(stderr, "[cropPaste:img] factoryCreate returned null\n");
        return items;
    }
    SceneItem* obj = sp.get();

    if (heavyLog) {
        SceneImageItem::hexdumpDefaultInit(obj);
    }

    // 6. Set the canvas-side rect and identity transform. Stock paint reads
    //    the QRectF at this+0x60 as the destination on the page (NOT a
    //    sub-rect of the source image — older comments called it
    //    "source_bounds" misleadingly; renamed to destBounds in MASTER.md
    //    update for v1.6).
    SceneImageItem::setSourceBoundsF(obj, 0.0, 0.0,
                                     (double)cw, (double)ch, heavyLog);
    SceneImageItem::setTransformScale(obj, 1.0, heavyLog);

    // 7. Generate a Qt-conformant v4 UUID via QUuid::createUuid() and copy
    //    its in-memory struct (16 bytes: u32 data1 + u16 data2 + u16 data3
    //    + u8[8] data4) directly into the SceneImageItem at offset 0x48.
    //
    //    v1.6f-and-earlier bug (fixed in v1.6g): we wrote raw /dev/urandom
    //    bytes with version/variant bits set in ASCII byte order. xochitl
    //    interpreted those bytes via QUuid's LE struct fields, producing a
    //    logical UUID with version nibble `f` (read out of data3's BE high
    //    nibble = high nibble of byte[7], not byte[6]) — non-conforming v4.
    //    Empirically observed in the .rm round-trip: the disk bytes match
    //    `QUuid::toRfc4122()` byte-reversed exactly, confirming xochitl's
    //    QUuid interpretation. Version validation may have rejected the
    //    item silently on deserialize (one of several candidates for the
    //    persistence-fails-on-revisit bug per MASTER.md "v1.6 EXPLORED").
    //
    //    Fix: ask Qt to generate the UUID natively, then memcpy the 16-byte
    //    struct verbatim. Round-trip is guaranteed by definition: xochitl
    //    reads +0x48 as the same QUuid we constructed.
    //
    //    UUID-keyed (not pointer-keyed) side-map so the lookup survives
    //    xochitl's page-unload-and-reload-from-disk cycle: deserializer
    //    reconstructs a fresh SceneImageItem with the same UUID at +0x48
    //    from the .rm scenefile, and our hook re-hits.
    QUuid quuid = QUuid::createUuid();
    uint8_t uuid[16];
    std::memcpy(uuid, &quuid, 16);
    SceneImageItem::setImageUuid(obj, uuid, heavyLog);
    cropPaste::registerImage(uuid, captured);

    if (heavyLog) {
        QByteArray uuidStr = quuid.toString(QUuid::WithoutBraces).toUtf8();
        fprintf(stderr,
            "[cropPaste:img] image registered with QUuid=%s "
            "(in-mem bytes: %02x%02x%02x%02x %02x%02x %02x%02x %02x%02x"
            " %02x%02x%02x%02x%02x%02x); version=%d variant=%d map_size=%zu\n",
            uuidStr.constData(),
            uuid[0],uuid[1],uuid[2],uuid[3],
            uuid[4],uuid[5], uuid[6],uuid[7],
            uuid[8],uuid[9],
            uuid[10],uuid[11],uuid[12],uuid[13],uuid[14],uuid[15],
            (int)quuid.version(), (int)quuid.variant(),
            cropPaste::imageMapSize());
    }

    items.append(sp);
    fprintf(stderr, "[cropPaste:img] returning %lld scene item(s)\n",
            (long long)items.size());
    return items;
}


/* ── Existing methods (unchanged) ─────────────────────────────────── */

void ClipboardInjector::sleepMs(int ms) {
    ::usleep(ms * 1000);
}

QList<std::shared_ptr<SceneItem>> ClipboardInjector::loadFromJSON(const QString& path) {
    QList<std::shared_ptr<SceneItem>> items;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "[clipboard-injector] Error: cannot open %s\n",
                path.toUtf8().constData());
        return items;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonArray jsonArray = doc.array();

    fprintf(stderr, "[clipboard-injector] Loading %lld lines from %s\n",
           (long long)jsonArray.size(), path.toUtf8().constData());

    for (int i = 0; i < jsonArray.size(); i++) {
        QJsonObject obj = jsonArray[i].toObject();
        QJsonArray pointsArray = obj["points"].toArray();
        QList<LinePoint> linePoints;

        for (int j = 0; j < pointsArray.size(); j++) {
            QJsonArray ptArr = pointsArray[j].toArray();
            LinePoint pt;
            pt.x = static_cast<float>(ptArr[0].toDouble());
            pt.y = static_cast<float>(ptArr[1].toDouble());
            pt.speed = static_cast<unsigned short>(ptArr[2].toInt());
            pt.width = static_cast<unsigned short>(ptArr[3].toInt());
            pt.direction = static_cast<unsigned char>(ptArr[4].toInt());
            pt.pressure = static_cast<unsigned char>(ptArr[5].toInt());
            linePoints.append(pt);
        }

        QJsonArray boundsArr = obj["bounds"].toArray();
        QRectF bounds(boundsArr[0].toDouble(), boundsArr[1].toDouble(),
                      boundsArr[2].toDouble(), boundsArr[3].toDouble());

        Line line;
        line.points = linePoints;
        line.bounds = bounds;
        line.rgba = static_cast<unsigned int>(obj["rgba"].toDouble());
        line.color = obj["color"].toInt(0);
        line.tool = obj["tool"].toInt(0x17);
        line.maskScale = obj["maskScale"].toDouble(1.0);
        line.thickness = static_cast<float>(obj["thickness"].toDouble(0.0));

        auto item = std::make_shared<SceneLineItem>(
            SceneLineItem::fromLine(std::move(line)));
        items.push_back(item);
    }

    fprintf(stderr, "[clipboard-injector] Loaded %lld scene items\n", (long long)items.size());
    return items;
}

bool ClipboardInjector::setupVtablePtr(
        const QList<std::shared_ptr<SceneItem>>& items) {
    if (items.empty()) return false;
    auto* item = reinterpret_cast<SceneLineItem*>(items.first().get());
    SceneLineItem::setupVtable(item->vtable);
    return true;
}

Line ClipboardInjector::createLine(
        const QPointF& start, const QPointF& end) {
    QList<LinePoint> linePoints;
    LinePoint p1;
    p1.x = static_cast<float>(start.x());
    p1.y = static_cast<float>(start.y());
    p1.speed = 25;
    p1.width = 25;
    p1.direction = 0;
    p1.pressure = 255;
    linePoints.append(p1);

    LinePoint p2;
    p2.x = static_cast<float>(end.x());
    p2.y = static_cast<float>(end.y());
    p2.speed = 25;
    p2.width = 25;
    p2.direction = 0;
    p2.pressure = 255;
    linePoints.append(p2);

    QRectF bounds(
        std::min(start.x(), end.x()), std::min(start.y(), end.y()),
        std::abs(end.x() - start.x()), std::abs(end.y() - start.y()));
    return Line::fromPoints(std::move(linePoints), bounds);
}

Line ClipboardInjector::createCircle(const QPointF& center, float radius) {
    QList<LinePoint> linePoints;
    int numPoints = 8;
    for (int i = 0; i <= numPoints; ++i) {
        float angle = i * 2.0f * 3.14159f / numPoints;
        LinePoint pt;
        pt.x = static_cast<float>(center.x() + radius * cos(angle));
        pt.y = static_cast<float>(center.y() + radius * sin(angle));
        pt.speed = 25;
        pt.width = 25;
        pt.direction = 0;
        pt.pressure = 255;
        linePoints.append(pt);
    }

    QRectF bounds(center.x() - radius, center.y() - radius,
                  radius * 2, radius * 2);
    return Line::fromPoints(std::move(linePoints), bounds);
}


/* ══════════════════════════════════════════════════════════════════════
 *  captureArea  — read framebuffer region → edge detect → clipboard JSON
 *
 *  Coordinates are in framebuffer pixels (not scene-view coordinates).
 *  Panel dimensions are read from framebuffer-spy at runtime; current
 *  expected values:
 *    rMPP 11.8" (ferrari): 1620 × 2160 BGRA (type=2, bpl=6528)
 *    rMPP Move (porsche):   960 × 1696 BGRA (type=2, bpl=3840)
 *    RM2 (legacy upstream): 1404 × 1872 BGRA (type=2, bpl=5616)
 * ══════════════════════════════════════════════════════════════════════ */
bool ClipboardInjector::captureArea(int rx, int ry, int rw, int rh) {
    /* ── 1. Get framebuffer ─────────────────────────────────────────── */
    void  *fbAddr = nullptr;
    int    fbW = 0, fbH = 0, fbType = 0, fbBpl = 0;
    ci_getFramebufferInfo(&fbAddr, &fbW, &fbH, &fbType, &fbBpl);

    if (!fbAddr || fbW <= 0 || fbH <= 0) {
        fprintf(stderr, "[clipboard-injector] captureArea: no framebuffer!\n");
        return false;
    }

    /* Clamp the requested rectangle to framebuffer bounds */
    int x0, y0, x1, y1;
    if (rw <= 0 || rh <= 0) {
        /* Full framebuffer capture */
        x0 = 0; y0 = 0;
        x1 = fbW; y1 = fbH;
    } else {
        x0 = std::max(0, rx);
        y0 = std::max(0, ry);
        x1 = std::min(fbW, rx + rw);
        y1 = std::min(fbH, ry + rh);
    }
    int cw = x1 - x0;
    int ch = y1 - y0;
    if (cw <= 2 || ch <= 2) {
        fprintf(stderr, "[clipboard-injector] captureArea: region too small\n");
        return false;
    }

    fprintf(stderr, "[clipboard-injector] captureArea: crop %d×%d @ (%d,%d) "
            "from FB %d×%d type=%d\n", cw, ch, x0, y0, fbW, fbH, fbType);

    /* ── 2. Crop + convert to grayscale ─────────────────────────────── */
    uint8_t *gray = (uint8_t *)malloc(cw * ch);
    if (!gray) return false;

    if (fbType == FBSPY_TYPE_RGBA) {
        /* Each row: fbBpl bytes = fbW * 4 bytes of BGRA */
        uint8_t *rowBuf = (uint8_t *)malloc(cw * 4);
        for (int y = 0; y < ch; y++) {
            const uint8_t *src = (const uint8_t *)fbAddr
                                 + (y0 + y) * fbBpl + x0 * 4;
            memcpy(rowBuf, src, cw * 4);
            bgraToGray(rowBuf, gray + y * cw, cw);
        }
        free(rowBuf);
    } else {
        /* RGB565: each pixel = 2 bytes */
        int stride = fbBpl / 2; /* pixels per row in FB */
        uint16_t *rowBuf = (uint16_t *)malloc(cw * 2);
        for (int y = 0; y < ch; y++) {
            const uint16_t *src = (const uint16_t *)fbAddr
                                  + (y0 + y) * stride + x0;
            memcpy(rowBuf, src, cw * 2);
            rgb565ToGray(rowBuf, gray + y * cw, cw);
        }
        free(rowBuf);
    }

    /* ── 3. Edge detection  (Laplacian, no blur, thresholded) ───────── */
    /* v1.5c thinning pass: skip the 3x3 Gaussian blur (it was the source
     * of the "blurry" perception — softens input then Laplacian
     * over-responds to the spread). Threshold the Laplacian output to
     * keep only strong edges, so we don't extract faint anti-alias halos
     * as separate strokes. Result: noticeably thinner traced text. */
    uint8_t *edges = (uint8_t *)malloc(cw * ch);
    if (!edges) { free(gray); return false; }

    laplacianFilter(gray, edges, cw, ch);
    free(gray);

    const uint8_t kEdgeThreshold = 96;
    int n = cw * ch;
    for (int i = 0; i < n; i++) {
        if (edges[i] < kEdgeThreshold) edges[i] = 0;
    }

    /* ── 4. Extract horizontal lines ────────────────────────────────── */
    int lineCount = 0;
    float *lines = extractHorizontalLines(edges, cw, ch, &lineCount);
    free(edges);

    fprintf(stderr, "[clipboard-injector] captureArea: %d lines extracted\n",
            lineCount);

    if (lineCount == 0) {
        free(lines);
        return false;
    }

    /* ── 5. Write clipboard JSON ────────────────────────────────────── */
    bool ok = writeClipboardJSON(lines, lineCount, "/tmp/clipboard_inject.json");
    free(lines);

    fprintf(stderr, "[clipboard-injector] captureArea: JSON written, ok=%d\n", ok);
    return ok;
}
