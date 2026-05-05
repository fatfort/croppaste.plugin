#pragma once

#include <memory>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QVariant>
#include "rm_Line.hpp"
#include "rm_SceneItem.hpp"

class ClipboardInjector : public QObject
{
    Q_OBJECT
    public:
        explicit ClipboardInjector(QObject *parent = nullptr) : QObject(parent) {}

        Q_INVOKABLE void sleepMs(int ms);

        // Load scene items from a JSON file (xovi-stickers format)
        Q_INVOKABLE QList<std::shared_ptr<SceneItem>> loadFromJSON(const QString& path);

        // Setup vtable pointer from existing clipboard items
        Q_INVOKABLE bool setupVtablePtr(const QList<std::shared_ptr<SceneItem>>& items);

        // Create a helper line for vtable extraction
        Q_INVOKABLE Line createLine(const QPointF& start, const QPointF& end);
        Q_INVOKABLE Line createCircle(const QPointF& center, float radius);

        // Capture a region of the live framebuffer, edge-detect, and write JSON
        Q_INVOKABLE bool captureArea(int x, int y, int w, int h);

        // Phase 2 first-deploy entry point. Calls xochitl's case-15 factory
        // (after signature check), hexdumps the returned default-init,
        // mutates source bounds + inline PNG bytes (1x1 black, hardcoded),
        // logs every field write, and returns the shared_ptr in a QList
        // ready for `Clipboard.items = ...`. No framebuffer involvement.
        Q_INVOKABLE QList<std::shared_ptr<SceneItem>> pasteTestImage();

        // v1.5: real-capture raster paste. Reads the framebuffer rect via
        // ci_getFramebufferInfo, wraps it as a QImage, copies the cropped
        // sub-rect, encodes PNG, then runs the same SceneImageItem
        // factory + mutation path as pasteTestImage. Returns a one-item
        // list ready for `Clipboard.items = ...`. Heavy-logs only the
        // first invocation per process lifetime; subsequent calls are quiet.
        Q_INVOKABLE QList<std::shared_ptr<SceneItem>> captureAreaAsImage(
                int rx, int ry, int rw, int rh);
};
