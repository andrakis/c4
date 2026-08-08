// c4or1k MMIO dispatch: routes a physical address to a device by its
// top byte, matching jor1k's RAM.AddDevice/Read8Big convention
// (`devices[(addr>>24)&0xFF]`) -- see mem.c for why the "is this RAM
// or a device" check itself has to be an explicit bit-31 mask rather
// than "addr < 0" the way jor1k's JS int32 arithmetic gets it for
// free.
//
// Byte-width only for now: the one device that exists (uart.c) is
// byte-width only too, matching jor1k's UARTDev (no ReadReg16/32).
// A 16/32-bit MMIO access has no real target to route to yet and
// isn't expected from any guest driver this project runs.

int mmio_read8(int addr);
void mmio_write8(int addr, int val);
