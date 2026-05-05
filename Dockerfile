# cropPaste — cross-compile container.
#
# Base: eeems/remarkable-toolchain:5.6.75-rmpp.
#   - aarch64 Yocto/codex SDK image for the reMarkable Paper Pro.
#   - Tag pinned (not 'latest-rmpp') for reproducibility across firmware bumps.
#   - The rMPP-Move (porsche) is also aarch64 and binary-compatible for our
#     purposes; if a runtime ABI difference shows up we'll add a -rmppm build.
#   - Versus the upstream rM2 image, we drop the armv7 NEON flags
#     (-mfpu=neon -mfloat-abi=hard) — they're invalid on aarch64.
#     Advanced SIMD is mandatory in armv8-a, so no extra flag is needed
#     beyond <arm_neon.h>.
#
# Build:   docker build --platform linux/amd64 -t croppaste-build .
#          (the codex SDK installer ships x86_64 host binaries; on Apple
#           Silicon hosts Docker Desktop runs the container under Rosetta.)
# Run:     docker run --rm -v "$PWD:/work" -w /work croppaste-build make

FROM --platform=linux/amd64 eeems/remarkable-toolchain:5.6.75-rmpp

WORKDIR /src

RUN apt-get update && apt-get install -y --no-install-recommends \
        git python3 \
    && rm -rf /var/lib/apt/lists/* \
    && git clone --depth 1 https://github.com/asivery/xovi /opt/xovi

ENV XOVI_REPO=/opt/xovi

# Default command: build whatever's mounted at /src against the codex SDK.
# The toolchain's environment-setup-* must be sourced inside a bash login
# shell before invoking qmake6/make, hence the chained command.
CMD ["bash", "-lc", ". /opt/codex/*/*/environment-setup-* && qmake6 && make"]
