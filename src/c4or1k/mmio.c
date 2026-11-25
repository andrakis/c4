#include "mmio.h"
#include "uart.h"
#include "virtio.h"
#include "eth.h"

// Top-byte device selector, matching jor1k's own `devices[(addr>>24)&0xFF]`
// switch-like dispatch table -- see mmio.h. Case labels must be plain
// literals under c4lc (`case (UART_MMIO_BASE >> 24) & 0xFF:` doesn't
// parse as a constant expression), so these mirror UART_MMIO_BASE/
// VIRTIO_MMIO_BASE's top bytes by hand rather than computing them.
enum { UART_TOP = 0x90, UART1_TOP = 0x96, VIRTIO_TOP = 0x97, ETH_TOP = 0x92 };

// Devices this project doesn't implement (ethernet, framebuffer,
// touchscreen, keyboard, sound, RTC, ATA, ...) probe-and-fail
// gracefully against these unhandled-address fallbacks by design --
// see M4's README notes. Once real Linux is actually running (M5/M6),
// some of those probes recur constantly (an ethernet link-status poll
// timer fires forever in the background), so logging every single
// access -- as M4 did, when this only ever fired a handful of times
// during early boot -- both floods the log and burns real time on
// printf. warned[] reports each *unique* top byte once instead.
int warned[256];

void warn_once(char *what, int top, int addr) {
    if (warned[top]) return;
    warned[top] = 1;
    printf("%s: no device at top byte 0x%x (addr=0x%x) -- further accesses to this device are not logged\n", what, top, addr);
}

int mmio_read8(int addr) {
    int top;
    top = (addr >> 24) & 0xFF;
    switch (top) {
    case UART_TOP: return uart_read8(0, addr & 0xFFFFFF);
    case UART1_TOP: return uart_read8(1, addr & 0xFFFFFF);
    case VIRTIO_TOP: return virtio_read8(addr & 0xFFFFFF);
    }
    warn_once("mmio_read8", top, addr);
    return 0;
}

void mmio_write8(int addr, int val) {
    int top;
    top = (addr >> 24) & 0xFF;
    switch (top) {
    case UART_TOP: uart_write8(0, addr & 0xFFFFFF, val); return;
    case UART1_TOP: uart_write8(1, addr & 0xFFFFFF, val); return;
    case VIRTIO_TOP: virtio_write8(addr & 0xFFFFFF, val); return;
    }
    warn_once("mmio_write8", top, addr);
}

int mmio_read32(int addr) {
    int top;
    top = (addr >> 24) & 0xFF;
    switch (top) {
    case VIRTIO_TOP: return virtio_read32(addr & 0xFFFFFF);
    case ETH_TOP: return eth_read32(addr & 0xFFF);
    }
    warn_once("mmio_read32", top, addr);
    return 0;
}

void mmio_write32(int addr, int val) {
    int top;
    top = (addr >> 24) & 0xFF;
    switch (top) {
    case VIRTIO_TOP: virtio_write32(addr & 0xFFFFFF, val); return;
    case ETH_TOP: eth_write32(addr & 0xFFF, val); return;
    }
    warn_once("mmio_write32", top, addr);
}
