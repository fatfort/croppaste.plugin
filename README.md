# cropPaste

A region-capture / paste extension for xochitl on the reMarkable Paper
Pro (rMPP, ferrari, 11.8") and the rMPP Move (porsche, 7.3"), firmware
3.26.0.68. Lets you draw a rectangle on a notebook or PDF page and
clone the pixels (and any strokes inside) into the system clipboard so
you can paste them anywhere else as a `SceneImageItem`.

The plugin is an `xovi` extension (`.so`) that bundles its own
qmldiff. It hooks `SceneImageItem::paint`, builds clones via xochitl's
own factory, and inserts them through `Clipboard.items`. There is no
binary patch on `/usr/bin/xochitl` — only LD_PRELOAD plus per-firmware
vtable + factory addresses (see `src/clipboard-injector/firmware_addrs.cpp`).

## What it does

A new `Capture region` button (crop icon) appears in the editing
toolbar — directly on the long-axis main toolbar, and also in the
right-most three-dots `SettingsMenu` foldout when the toolbar is on
the screen's short axis.

| step | effect |
|---|---|
| **tap the crop icon** | a translucent overlay covers the document. |
| **drag a rectangle** | a white outline tracks the selection. |
| **release** | xochitl re-renders without the overlay (≥50 ms defer), the framebuffer is sampled, and the resulting `SceneImageItem` is pushed onto `Clipboard.items`. |
| **paste anywhere** | the standard xochitl paste gesture clones the captured region as a selectable image. |

## Build (Docker)

```sh
docker build --platform linux/amd64 -t croppaste-build .
docker run --rm --platform linux/amd64 \
    -v "$PWD/src/clipboard-injector:/src" \
    croppaste-build
```

Toolchain image is `eeems/remarkable-toolchain:5.6.75-rmpp` — the
codex SDK matching firmware 3.26.0.68. The container runs `qmake6 +
make` against `clipboard-injector.pro` and writes
`src/clipboard-injector/clipboard-injector.so`.

The qmldiff source is `src/clipboard-injector/clipboard-injector.qml-diff`.
`bin/compile-qmd.sh` hashes it against `reference/hashtab` to
`build/clipboard-injector.qmd`. Copy that into
`src/clipboard-injector/` so xovigen embeds it as a resource on the
next `.so` rebuild.

## Install

The plugin lives at `/home/root/xovi/extensions.d/cropPaste.so`.
Backup, push, restart:

```sh
ssh root@192.168.1.112 \
    'cp /home/root/xovi/extensions.d/cropPaste.so \
        /home/root/cropPaste.so.pre-$(date +%H%M)'
scp build/cropPaste.so root@192.168.1.112:/home/root/xovi/extensions.d/cropPaste.so
ssh root@192.168.1.112 'systemctl restart xochitl'
```

Verify in the journal:

```
[cropPaste:hook] v1.6j paint hook installed (ferrari): pid=... \
    vtable=0x16a3df0 slot[3] old=0xe83630 new=... OK
[clipboard-injector] Registering ClipboardInjector
```

NEVER place backup `.so` or other non-extension files inside
`/home/root/xovi/extensions.d/` — the xovi loader treats every file in
that directory as an active extension and will detach if there's a
duplicate or a `.bak`. See the project memory `feedback_xovi_extensionsd_no_bak`.

## Per-device dispatch

`firmware_addrs.cpp` reads `/etc/hostname` and dispatches to the right
vtable / paint / factory addresses for `imx8mm-ferrari` (rMPP) vs
`imx93-chiappa` (rMPP Move / porsche). Both addresses were RE'd by
binary disassembly during the v1.6 push; ferrari is currently
production, porsche is staged but not always deployed.

## Version history

| version | what changed |
|---|---|
| **v1.5** | Monochrome stroke-trace pipeline. Built captured rectangle as a `SceneLineItem` traced by sampling pixels — preserved selectability + persistence but lost colour. |
| **v1.6** | Colour raster path. Hook `SceneImageItem::paint`, sample the freshly-rendered framebuffer, build a `SceneImageItem` via xochitl's own factory, push to `Clipboard.items`. Dropped stroke-trace fallback in v1.6c after the conditional kept firing the wrong branch. v1.6j is the current production build. |
| **v1.7** | Slave dir not yet integrated; see `SLAVE-V1.7.md`. |

## Repo layout

```
.
├── Dockerfile                 cross-compile toolchain image
├── MASTER.md                  master tracker for parent-supervised slave sessions
├── SLAVE-*.md                 individual slave delegation briefs
├── bin/
│   └── compile-qmd.sh         qml-diff → hashed .qmd via qmldiff
├── reference/
│   ├── extract-qml.py         pulls QML out of xochitl's zstd-frame resources
│   ├── hashtab                qmldiff hashtab for firmware 3.26.0.68
│   ├── qml/                   extracted xochitl QML for reference
│   └── v1.6*-deploy-journal*  past deploy logs
└── src/clipboard-injector/    C++ source for the xovi extension
    ├── ClipboardInjector.cpp  QML-exposed singleton
    ├── customVtable.cpp       per-stroke vtable swap helpers
    ├── firmware_addrs.cpp     ferrari ↔ porsche dispatch
    ├── rm_*.cpp               local re-declarations of xochitl scene types
    ├── clipboard-injector.pro qmake project
    ├── clipboard-injector.xovi xovi extension manifest
    └── clipboard-injector.qml-diff qmldiff source (Toolbar.qml inject)
```

`build/` and the qmake-generated `Makefile` / `build/` subdirs are
git-ignored. Rebuild via Docker.

## Status

ferrari (11.8" rMPP): production v1.6j, attaches via xovi-autostart on
every boot.
porsche (rMPP Move): vtable + factory addresses RE'd, build path
shipped, deploy not yet routine.
