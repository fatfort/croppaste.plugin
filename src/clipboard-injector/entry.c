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
	qt_resource_rebuilder$qmldiff_add_external_diff(r$clipboard_injector, "Clipboard injector");

	// Install the global SceneImageItem paint() hook. Must happen before
	// any rendering. Patches one 8-byte slot in .rodata — atomic write,
	// no synchronization needed with in-flight render threads.
	int hook_ok = cropPaste_installPaintHook();
	fprintf(stderr,
	    "[clipboard-injector] cropPaste_installPaintHook -> %s\n",
	    hook_ok ? "OK" : "FAILED (color rendering disabled, items will paint as cache-miss placeholder)");
}
