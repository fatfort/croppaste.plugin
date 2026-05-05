# cropPaste hello-world: minimum XOVI extension that logs at load.
#
# Built inside the cross-compile container (../../Dockerfile). The
# container sources /opt/codex/.../environment-setup-* which sets CC,
# CFLAGS, and friends to point at the aarch64-poky-linux toolchain.

NAME      = hello
XOVI_REPO ?= /opt/xovi
OBJ       = main.o xovi.o
OUT       = $(NAME).so

# -fPIC: shared object code. -D_GNU_SOURCE: program_invocation_short_name
# would need it; xovi.h itself doesn't, but we keep the convention
# established by framebuffer-spy / clipboard-injector.
CFLAGS   += -fPIC -D_GNU_SOURCE -O2 -Wall

.PHONY: all clean
all: $(OUT)

$(OUT): $(OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^

# xovigen.py turns hello.xovi into xovi.c (extension descriptor table)
# and xovi.h (trampoline declarations). The `&:` grouped-target syntax
# tells make a single recipe produces both files.
xovi.c xovi.h &: hello.xovi
	python3 $(XOVI_REPO)/util/xovigen.py -o xovi.c -H xovi.h hello.xovi

main.o: main.c xovi.h
xovi.o: xovi.c xovi.h

clean:
	rm -f $(OBJ) xovi.c xovi.h $(OUT)
