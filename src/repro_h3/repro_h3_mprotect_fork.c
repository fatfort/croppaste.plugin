/*
 * repro_h3_mprotect_fork.c — H3 isolator for cropPaste v1.6 renderer crash.
 *
 * BACKGROUND
 * ----------
 * cropPaste v1.6e installs a single 8-byte vtable-slot patch on xochitl's
 * SceneImageItem vtable (mprotect file-backed page R -> RW, write 8 bytes,
 * mprotect back to R). Within ~60 s of this patch landing, xochitl's
 * separately-spawned PDF renderer subprocess starts dying with
 * "renderer exited while waiting for a response" 7-14 times.
 *
 * Investigation 2026-05-04 ruled out direct vtable corruption of the
 * renderer:
 *   - xochitl_pdf_renderer is a separate ELF (266 KB), not a fork-only
 *     descendant of xochitl. It is spawned via fork()+execve().
 *   - Its DT_NEEDED list is libpdfium / libQt6Gui / libQt6Core / libstdc++
 *     / libm / libgcc_s / libc — no libxochitl-*.
 *   - It contains zero `Scene*` strings or symbols; the SceneImageItem
 *     vtable address (0x16a3df0) lives inside the xochitl binary's
 *     .rodata, which the renderer never maps.
 *
 * That leaves H3 as the leading candidate:
 *
 *   H3: mprotect(file-backed-page, R -> RW -> R) on the parent process
 *       triggers a kernel-level state change (page-cache flush, TLB
 *       shootdown, inode-mapping invalidation, COW corruption, dirty-bit
 *       fan-out, etc.) that destabilizes subsequent fork()+execve()'d
 *       children, *independently of what code runs in the child*.
 *
 * If H3 is true, the workaround space is:
 *   - delay the mprotect until after the renderer has been spawned at
 *     least once
 *   - use an alternate hooking mechanism (PLT/GOT redirection, PAC-aware
 *     trampoline, etc.) that doesn't touch file-backed pages
 *   - ship the patched vtable as a separate file the dynamic linker maps
 *     RW from the start
 *
 * If H3 is false, the renderer-crash mechanism is something xochitl- or
 * Qt-specific (signal/slot infrastructure across processes, PDFium's
 * interaction with mmap'd state, font cache, etc.) and we widen H4+.
 *
 * TEST DESIGN
 * -----------
 * 1. Open /proc/self/exe (this binary's own ELF on disk).
 * 2. mmap a 4 KB page MAP_PRIVATE | PROT_READ from offset 0 of that ELF.
 *    This mirrors how the dynamic linker maps xochitl's .rodata.
 * 3. mprotect(page, PROT_READ | PROT_WRITE).
 * 4. Write 8 bytes (sentinel value, doesn't matter what — only the
 *    page-table entry transition matters; the file is never touched
 *    because MAP_PRIVATE creates an anonymous COW page on first write).
 * 5. mprotect(page, PROT_READ).  Page-table now reflects the post-patch
 *    state matching production v1.6e.
 * 6. fork() + execl("/bin/echo", ...) ITERATIONS times. Track exit
 *    status. ITERATIONS = 32 to catch flaky/probabilistic crashes.
 * 7. As a CONTROL, also run a second pass of ITERATIONS fork+execs
 *    *without* doing the mprotect dance, to confirm the baseline rate
 *    of (presumably zero) failures.
 *
 * INTERPRETATION
 * --------------
 *   patched-pass crashes > 0, control-pass crashes == 0:  H3 CONFIRMED.
 *   both passes crash:                                    test bug or
 *                                                         pre-existing
 *                                                         kernel issue
 *                                                         unrelated to
 *                                                         our patch.
 *   neither passes crash:                                 H3 DENIED;
 *                                                         widen search to
 *                                                         xochitl-specific
 *                                                         mechanisms.
 *
 * BUILD (host, Docker, aarch64 cross to ferrari sysroot)
 * ------------------------------------------------------
 *   docker run --rm -v "$PWD/src/repro_h3:/work" -w /work croppaste-build \
 *     bash -lc '. /opt/codex/ferrari/5.6.75/environment-setup-cortexa53-crypto-remarkable-linux \
 *               && $CC -O0 -g -static-libgcc \
 *                  -o repro_h3_mprotect_fork repro_h3_mprotect_fork.c'
 *
 * RUN ON DEVICE (next session — this is a deploy, do NOT run tonight)
 * --------------------------------------------------------------------
 *   scp repro_h3_mprotect_fork root@10.11.99.1:/tmp/
 *   ssh root@10.11.99.1 chmod +x /tmp/repro_h3_mprotect_fork
 *   ssh root@10.11.99.1 /tmp/repro_h3_mprotect_fork
 *
 * Expected runtime: < 1 s. Output goes to stderr.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ITERATIONS 32
#define PAGE_SIZE  4096

typedef struct { int ok; int nonzero; int crashes; } pass_result_t;

static int do_mprotect_dance(void) {
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) { perror("open /proc/self/exe"); return -1; }

    void *map = mmap(NULL, PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    fprintf(stderr, "[repro] mmap'd /proc/self/exe page @ %p (fd=%d)\n",
            map, fd);

    if (mprotect(map, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "[repro] mprotect RW failed: %s\n", strerror(errno));
        munmap(map, PAGE_SIZE); close(fd); return -1;
    }

    /* 8-byte write into the page — same shape as our real vtable patch.
     * Offset 16 is past the ELF magic so we don't break anything that
     * might be subsequently consulted (the ELF is already loaded; this
     * write only affects the COW copy). */
    unsigned long sentinel = 0xdeadbeefcafef00dUL;
    memcpy((unsigned char *)map + 16, &sentinel, sizeof sentinel);

    if (mprotect(map, PAGE_SIZE, PROT_READ) != 0) {
        fprintf(stderr, "[repro] mprotect R failed: %s\n", strerror(errno));
        munmap(map, PAGE_SIZE); close(fd); return -1;
    }

    fprintf(stderr, "[repro] mprotect+write+restore done.\n");
    /* Intentionally do NOT munmap — production v1.6e leaves the patched
     * page mapped for the entire xochitl lifetime. */
    close(fd);
    return 0;
}

static pass_result_t fork_exec_pass(const char *label) {
    pass_result_t r = {0, 0, 0};
    fprintf(stderr, "[repro] === %s pass: %d fork+exec iterations ===\n",
            label, ITERATIONS);

    for (int i = 0; i < ITERATIONS; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); r.crashes++; continue; }
        if (pid == 0) {
            char argbuf[64];
            snprintf(argbuf, sizeof argbuf, "[%s child %d] alive", label, i);
            execl("/bin/echo", "echo", argbuf, (char *)NULL);
            perror("execl");
            _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid"); r.crashes++; continue;
        }
        if (WIFSIGNALED(status)) {
            fprintf(stderr,
                    "[repro] %s iter %d: CHILD KILLED by signal %d "
                    "(%s%s)\n",
                    label, i, WTERMSIG(status), strsignal(WTERMSIG(status)),
                    WCOREDUMP(status) ? ", core dumped" : "");
            r.crashes++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr,
                    "[repro] %s iter %d: child exit=%d (non-zero)\n",
                    label, i, WEXITSTATUS(status));
            r.nonzero++;
        } else {
            r.ok++;
        }
    }

    fprintf(stderr, "[repro] %s SUMMARY: ok=%d nonzero=%d crashes=%d\n",
            label, r.ok, r.nonzero, r.crashes);
    return r;
}

int main(void) {
    fprintf(stderr,
        "[repro] H3 isolator: mprotect(file-backed page, R->RW->R) "
        "+ fork+execve.\n[repro] pid=%d page_size=%d\n",
        (int)getpid(), PAGE_SIZE);

    /* Control pass first — establish baseline of fork+exec reliability
     * with no patch dance applied. */
    pass_result_t control = fork_exec_pass("control");

    /* Apply the patch dance. */
    if (do_mprotect_dance() != 0) {
        fprintf(stderr, "[repro] FATAL: mprotect dance failed\n");
        return 1;
    }

    /* Patched pass — same fork+exec loop, but now after the parent has
     * touched a file-backed page via mprotect+write+restore. */
    pass_result_t patched = fork_exec_pass("patched");

    /* Verdict. */
    fprintf(stderr,
        "\n[repro] === H3 VERDICT ===\n"
        "[repro] control:  ok=%d nonzero=%d crashes=%d\n"
        "[repro] patched:  ok=%d nonzero=%d crashes=%d\n",
        control.ok, control.nonzero, control.crashes,
        patched.ok, patched.nonzero, patched.crashes);

    if (control.crashes == 0 && patched.crashes > 0) {
        fprintf(stderr,
            "[repro] H3 CONFIRMED: mprotect dance breaks fork+exec'd "
            "children.\n");
        return 2;
    }
    if (control.crashes == 0 && patched.crashes == 0
        && control.nonzero == 0 && patched.nonzero == 0) {
        fprintf(stderr,
            "[repro] H3 DENIED: fork+exec works identically before and "
            "after mprotect dance. Renderer-crash mechanism is something "
            "else (xochitl- or Qt-specific). Widen search.\n");
        return 0;
    }
    fprintf(stderr,
        "[repro] AMBIGUOUS: re-run, or examine individual iter logs above. "
        "Possible: pre-existing kernel issue, /bin/echo unavailable, etc.\n");
    return 3;
}
