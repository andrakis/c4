// c4or1k UART -- 16550-compatible, matching
// jorconsole/jor1k/js/worker/dev/uart.js exactly (register offsets,
// LSR/IER/IIR/FCR bit meanings, the DLAB-aliased DLL/DLH registers,
// the CTI/THRI/MSI interrupt-cause priority order in
// uart_check_interrupt). MMIO base 0x90000000, IRQ line 2 -- both
// confirmed against jorconsole/jor1k/js/worker/system.js
// (`new UARTDev(0, this, 0x2)`, `ram.AddDevice(uartdev0, 0x90000000, 0x7)`).
//
// Byte-width registers only (l.lbz/l.sb from the guest): jor1k's
// UARTDev never defines ReadReg16/ReadReg32, so a 16/32-bit guest
// access to this device would crash real jor1k too -- mmio.c treats
// it the same way this project treats anything else unimplemented.

enum { UART_MMIO_BASE = 0x90000000 };
enum { UART_INTNO = 2 };

void uart_reset();
int uart_read8(int addr);
void uart_write8(int addr, int val);

// Feeds one received byte in from the host (main.c's stdin poll,
// wired in this milestone's launch-script integration). Matches
// UARTDev.prototype.ReceiveChar: sets LSR's data-ready bit and raises
// the CTI interrupt cause if IER.RDI is enabled.
void uart_receive_char(int c);
