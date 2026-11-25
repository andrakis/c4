// c4or1k UART -- 16550-compatible, matching
// jorconsole/jor1k/js/worker/dev/uart.js exactly (register offsets,
// LSR/IER/IIR/FCR bit meanings, the DLAB-aliased DLL/DLH registers,
// the CTI/THRI/MSI interrupt-cause priority order in
// uart_check_interrupt). Two units exist, matching jor1k's
// `uartdev0`/`uartdev1` exactly (system.js: `new UARTDev(0, this,
// 0x2)` @ 0x90000000, `new UARTDev(1, this, 0x3)` @ 0x96000000) --
// unit 1 was added in M5/M6 once basefs.json's /etc/inittab turned
// out to spawn a second getty on ttyS1 (`ttyS1::respawn:-login -f
// root`); without a real, idle-but-correctly-behaved UART1, that
// getty's driver never sees a sane "no data yet, go to sleep" signal
// and instead hot-loops polling registers forever, burning the whole
// instruction budget before ttyS0's own getty/login ever gets a
// chance to run. Unit 1 never receives real host input (there's only
// one real terminal, wired to unit 0 via console.c) -- its getty just
// blocks forever, exactly like a real unconnected serial port would.
//
// Byte-width registers only (l.lbz/l.sb from the guest): jor1k's
// UARTDev never defines ReadReg16/ReadReg32, so a 16/32-bit guest
// access to this device would crash real jor1k too -- mmio.c treats
// it the same way this project treats anything else unimplemented.

enum { UART_MMIO_BASE = 0x90000000, UART1_MMIO_BASE = 0x96000000 };
enum { UART_INTNO = 2, UART1_INTNO = 3 };

void uart_reset();
int uart_read8(int unit, int addr);
void uart_write8(int unit, int addr, int val);

// Feeds one received byte in from the host (main.c's stdin poll,
// wired in this milestone's launch-script integration) to unit 0 --
// the only unit with a real byte source. Matches
// UARTDev.prototype.ReceiveChar: sets LSR's data-ready bit and raises
// the CTI interrupt cause if IER.RDI is enabled.
void uart_receive_char(int c);
