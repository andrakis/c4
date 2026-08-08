// c4or1k MMIO dispatch: routes a physical address to a device by its
// top byte, matching jor1k's RAM.AddDevice/Read8Big convention
// (`devices[(addr>>24)&0xFF]`) -- see mem.c for why the "is this RAM
// or a device" check itself has to be an explicit bit-31 mask rather
// than "addr < 0" the way jor1k's JS int32 arithmetic gets it for
// free.
//
// Byte-width for uart.c (matching jor1k's UARTDev -- no ReadReg16/32
// there either, so a 16/32-bit UART access has no real target, same
// as real jor1k) and for virtio.c's config space (byte-width covers
// everything the 9p transport's config-space reads need -- including
// its one 16-bit-sized field, the mount-tag length, which mem.c
// composes from two 8-bit reads rather than this dispatching a
// dedicated 16-bit call; see mem.c's ram_lh). 32-bit also routes to
// virtio.c: the control registers and ring-notify path are genuinely
// 32-bit (Linux's virtio_mmio driver uses readl/writel) -- see
// virtio.c's header comment for the endianness this implies.

int mmio_read8(int addr);
void mmio_write8(int addr, int val);
int mmio_read32(int addr);
void mmio_write32(int addr, int val);
