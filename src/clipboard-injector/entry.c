#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include "xovi.h"

void registerQmldiff();
extern char *program_invocation_short_name;

// v1.6f diagnostic: log at .so load time, before any XOVI machinery, so
// we know which processes the .so is being loaded into. Fires for main
// xochitl AND for any forked-then-exec'd children that inherit
// LD_PRELOAD (PDF renderer subprocess being the case under investigation).
__attribute__((constructor))
static void cropPaste_so_loaded(void) {
    fprintf(stderr,
        "[clipboard-injector:diag] .so loaded: program=%s pid=%d ppid=%d\n",
        program_invocation_short_name, getpid(), getppid());
}

// C-linkage wrapper around cropPaste::installPaintHook() (in customVtable.cpp).
// Patches slot 3 of xochitl's stock SceneImageItem vtable @ 0x16a3df0 to
// route paint through our hook. Returns 1 on success, 0 on failure;
// failure leaves all SceneImageItem paints dispatching to stock — we just
// don't get color rendering for our captures.
extern int cropPaste_installPaintHook(void);

// C wrapper around cropPaste::firmware() — returns 1 on porsche
// (imx93/chiappa hostname), 0 on ferrari (imx8mm/ferrari) or any
// unmatched hostname (firmware() defaults the unknown case to ferrari).
// Drives the bundled-qmd dispatch in _xovi_construct(). Defined in
// firmware_addrs.cpp.
extern int cropPaste_isPorsche(void);

/* FramebufferConfig matches framebuffer-spy.h */
struct FramebufferConfig {
    void *framebufferAddress;
    int width, height, type, bpl;
    bool requiresReload;
};

#define FBSPY_TYPE_RGB565 1
#define FBSPY_TYPE_RGBA   2

/*
 * C bridge: framebuffer-spy symbols use $ in identifiers (GCC extension).
 * C++ cannot use $, so we wrap them here for ClipboardInjector.cpp to call.
 */
void ci_getFramebufferInfo(void **addr, int *width, int *height,
                           int *type, int *bpl) {
    if (!framebuffer_spy$getFramebufferConfig) {
        fprintf(stderr, "[clipboard-injector] framebuffer-spy not available\n");
        *addr = NULL;
        return;
    }

    struct FramebufferConfig cfg =
        ((struct FramebufferConfig (*)()) framebuffer_spy$getFramebufferConfig)();

    /* Refresh if the device requires it (rM1 mmap'd framebuffer) */
    if (cfg.requiresReload && framebuffer_spy$refreshFramebuffer) {
        ((void (*)()) framebuffer_spy$refreshFramebuffer)();
    }

    *addr  = cfg.framebufferAddress;
    *width = cfg.width;
    *height= cfg.height;
    *type  = cfg.type;
    *bpl   = cfg.bpl;

    fprintf(stderr, "[clipboard-injector] FB: %p %dx%d type=%d bpl=%d\n",
            *addr, *width, *height, *type, *bpl);
}

void _xovi_construct() {
    fprintf(stderr,
        "[clipboard-injector:diag] _xovi_construct entered: program=%s pid=%d ppid=%d\n",
        program_invocation_short_name, getpid(), getppid());
    if (strstr(program_invocation_short_name, "worker") != NULL) {
        fprintf(stderr,
            "[clipboard-injector:diag] _xovi_construct skipping (worker process)\n");
        return;
    }
	printf("[clipboard-injector] Registering ClipboardInjector\n");
    Environment->requireExtension("qt-resource-rebuilder", 0, 2, 0);
	registerQmldiff();

	// Per-device qmd dispatch. The two bundled qmds differ only in the
	// SettingsMenu ("more tools" 3-dots foldout) inject:
	//   ferrari: long-axis Toolbar inject + cropOverlay + foldout entry
	//            for "Capture region" (the only cropPaste access point on
	//            ferrari's short-axis toolbar).
	//   porsche: long-axis Toolbar inject + cropOverlay only. The foldout
	//            entry is omitted because penSlots-porsche.qml-diff
	//            already injects a 3-button RowLayout (penSlots,
	//            eyeDropper, cropPaste) into the same SettingsMenu slot —
	//            having both produced a duplicate "Capture region"
	//            alongside the row.
	if (cropPaste_isPorsche()) {
		fprintf(stderr, "[clipboard-injector] device=porsche, registering porsche qmd\n");
		qt_resource_rebuilder$qmldiff_add_external_diff(r$clipboard_injector_porsche, "Clipboard injector (porsche)");
	} else {
		fprintf(stderr, "[clipboard-injector] device=ferrari, registering ferrari qmd\n");
		qt_resource_rebuilder$qmldiff_add_external_diff(r$clipboard_injector_ferrari, "Clipboard injector (ferrari)");
	}

	// Install the global SceneImageItem paint() hook. Must happen before
	// any rendering. Patches one 8-byte slot in .rodata — atomic write,
	// no synchronization needed with in-flight render threads.
	int hook_ok = cropPaste_installPaintHook();
	fprintf(stderr,
	    "[clipboard-injector] cropPaste_installPaintHook -> %s\n",
	    hook_ok ? "OK" : "FAILED (color rendering disabled, items will paint as cache-miss placeholder)");
}
