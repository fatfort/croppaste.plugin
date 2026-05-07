#include "firmware_addrs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace cropPaste {
namespace {

constexpr FirmwareAddrs kFerrari = {
    Device::Ferrari, "ferrari",
    /*imgVtable      */ 0x16a3df0,
    /*imgTypeinfo    */ 0x16a3dc8,
    /*imgPaint       */ 0xe83630,
    /*painterHelper  */ 0xec5ad0,
    /*imgFactory     */ 0xe58af0,
    /*factorySig0    */ 0xd503233fu,
    /*factorySig1    */ 0xa9bd7bfdu,
};

constexpr FirmwareAddrs kPorsche = {
    Device::Porsche, "porsche",
    /*imgVtable      */ 0x1572900,
    /*imgTypeinfo    */ 0x15728d8,
    /*imgPaint       */ 0xca2180,
    /*painterHelper  */ 0xce4550,
    /*imgFactory     */ 0xc77740,
    /*factorySig0    */ 0xd503233fu,
    /*factorySig1    */ 0xa9bd7bfdu,
};

Device parseHostname() {
    int fd = open("/etc/hostname", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr,
            "[cropPaste:firmware] /etc/hostname open failed (errno=%d) — "
            "defaulting to ferrari\n", errno);
        return Device::Ferrari;
    }
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return Device::Ferrari;
    buf[n] = 0;

    // Match on the chip codename — same SoC family is the most stable
    // signal across firmware bumps. "imx8mm" = ferrari (rMPP 11.8"),
    // "imx93"  = porsche (rMPP Move 7.3").
    if (std::strstr(buf, "imx93")  != nullptr) return Device::Porsche;
    if (std::strstr(buf, "chiappa")!= nullptr) return Device::Porsche;
    if (std::strstr(buf, "imx8mm") != nullptr) return Device::Ferrari;
    if (std::strstr(buf, "ferrari")!= nullptr) return Device::Ferrari;

    fprintf(stderr,
        "[cropPaste:firmware] /etc/hostname='%.*s' matched neither "
        "ferrari nor porsche — defaulting to ferrari\n",
        (int)n, buf);
    return Device::Ferrari;
}

const FirmwareAddrs& selectAddrs() {
    Device d = parseHostname();
    const FirmwareAddrs& fw = (d == Device::Porsche) ? kPorsche : kFerrari;
    fprintf(stderr,
        "[cropPaste:firmware] device=%s vtable=0x%lx typeinfo=0x%lx "
        "paint=0x%lx painterHelper=0x%lx factory=0x%lx sig=(0x%08x,0x%08x)\n",
        fw.deviceName,
        (unsigned long)fw.imgVtable, (unsigned long)fw.imgTypeinfo,
        (unsigned long)fw.imgPaint, (unsigned long)fw.painterHelper,
        (unsigned long)fw.imgFactory,
        fw.factorySig0, fw.factorySig1);
    return fw;
}

}  // namespace

const FirmwareAddrs& firmware() {
    static const FirmwareAddrs& cached = selectAddrs();
    return cached;
}

}  // namespace cropPaste

// C-linkage wrapper so entry.c can dispatch the bundled-qmd registration
// by device without pulling in C++. Caches via firmware()'s static.
extern "C" int cropPaste_isPorsche(void) {
    return cropPaste::firmware().device == cropPaste::Device::Porsche ? 1 : 0;
}
