#include "uart.h"
#include "cpu.h"

enum {
    UART_RXBUF = 0, UART_TXBUF = 0,
    UART_DLL = 0, UART_DLH = 1,
    UART_IER = 1, UART_IIR = 2, UART_FCR = 2,
    UART_LCR = 3, UART_MCR = 4, UART_LSR = 5, UART_MSR = 6, UART_SCR = 7
};

enum { UART_LSR_DATA_READY = 0x1, UART_LSR_TX_EMPTY = 0x20, UART_LSR_TRANSMITTER_EMPTY = 0x40 };
enum { UART_IER_MSI = 0x08, UART_IER_BRK = 0x04, UART_IER_THRI = 0x02, UART_IER_RDI = 0x01 };
enum { UART_IIR_MSI = 0x00, UART_IIR_NO_INT = 0x01, UART_IIR_THRI = 0x02, UART_IIR_RDI = 0x04, UART_IIR_RLSI = 0x06, UART_IIR_CTI = 0x0C };
enum { UART_LCR_DLAB = 0x80 };
enum { UART_MSR_DCD = 0x80, UART_MSR_DSR = 0x20, UART_MSR_CTS = 0x10 };

enum { NUNITS = 2 };
int uart_LCR[NUNITS], uart_LSR[NUNITS], uart_MSR[NUNITS], uart_ints[NUNITS];
int uart_IIR[NUNITS], uart_IER[NUNITS], uart_DLL[NUNITS], uart_DLH[NUNITS], uart_FCR[NUNITS], uart_MCR[NUNITS];

enum { RXBUF_CAP = 256 };
int rxbuf[NUNITS * RXBUF_CAP];
int rx_head[NUNITS], rx_len[NUNITS];

int uart_intno(int unit) { return unit == 0 ? UART_INTNO : UART1_INTNO; }

void uart_reset() {
    int u;
    for (u = 0; u < NUNITS; ++u) {
        uart_LCR[u] = 0x3;
        uart_LSR[u] = UART_LSR_TRANSMITTER_EMPTY | UART_LSR_TX_EMPTY;
        uart_MSR[u] = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS;
        uart_ints[u] = 0;
        uart_IIR[u] = UART_IIR_NO_INT;
        uart_IER[u] = 0;
        uart_DLL[u] = 0; uart_DLH[u] = 0; uart_FCR[u] = 0; uart_MCR[u] = 0;
        rx_head[u] = 0; rx_len[u] = 0;
    }
}

// Priority order (CTI, then THRI, then MSI) matches
// UARTDev.prototype.CheckInterrupt exactly.
void uart_check_interrupt(int unit) {
    if ((uart_ints[unit] & (1 << UART_IIR_CTI)) && (uart_IER[unit] & UART_IER_RDI)) {
        uart_IIR[unit] = UART_IIR_CTI;
        cpu_raise_interrupt(uart_intno(unit));
    } else if ((uart_ints[unit] & (1 << UART_IIR_THRI)) && (uart_IER[unit] & UART_IER_THRI)) {
        uart_IIR[unit] = UART_IIR_THRI;
        cpu_raise_interrupt(uart_intno(unit));
    } else if ((uart_ints[unit] & (1 << UART_IIR_MSI)) && (uart_IER[unit] & UART_IER_MSI)) {
        uart_IIR[unit] = UART_IIR_MSI;
        cpu_raise_interrupt(uart_intno(unit));
    } else {
        uart_IIR[unit] = UART_IIR_NO_INT;
        cpu_clear_interrupt(uart_intno(unit));
    }
}

void uart_throw_interrupt(int unit, int line) {
    uart_ints[unit] = uart_ints[unit] | (1 << line);
    uart_check_interrupt(unit);
}

void uart_ack_interrupt(int unit, int line) {
    uart_ints[unit] = uart_ints[unit] & ~(1 << line);
    uart_check_interrupt(unit);
}

void uart_receive_char(int c) {
    if (rx_len[0] < RXBUF_CAP) {
        rxbuf[(rx_head[0] + rx_len[0]) % RXBUF_CAP] = c & 0xFF;
        rx_len[0] = rx_len[0] + 1;
    }
    if (rx_len[0] > 0) {
        uart_LSR[0] = uart_LSR[0] | UART_LSR_DATA_READY;
        uart_throw_interrupt(0, UART_IIR_CTI);
    }
}

int uart_read8(int unit, int addr) {
    int ret;

    if (uart_LCR[unit] & UART_LCR_DLAB) {
        if (addr == UART_DLL) return uart_DLL[unit];
        if (addr == UART_DLH) return uart_DLH[unit];
    }

    switch (addr) {
    case UART_RXBUF:
        ret = 0;
        if (rx_len[unit] > 0) {
            ret = rxbuf[unit * RXBUF_CAP + rx_head[unit]];
            rx_head[unit] = (rx_head[unit] + 1) % RXBUF_CAP;
            rx_len[unit] = rx_len[unit] - 1;
        }
        if (rx_len[unit] == 0) {
            uart_LSR[unit] = uart_LSR[unit] & ~UART_LSR_DATA_READY;
            uart_ack_interrupt(unit, UART_IIR_CTI);
        }
        return ret & 0xFF;
    case UART_IER:
        return uart_IER[unit] & 0x0F;
    case UART_MSR:
        ret = uart_MSR[unit];
        uart_MSR[unit] = uart_MSR[unit] & 0xF0; // reset the "delta" bits, matches uart.js
        return ret;
    case UART_IIR:
        ret = (uart_IIR[unit] & 0x0F) | 0xC0; // top two bits (fifo enabled) always set
        if (uart_IIR[unit] == UART_IIR_THRI) uart_ack_interrupt(unit, UART_IIR_THRI);
        return ret;
    case UART_LCR:
        return uart_LCR[unit];
    case UART_LSR:
        return uart_LSR[unit];
    }
    printf("uart_read8: unsupported register %d (unit %d)\n", addr, unit);
    return 0;
}

void uart_write8(int unit, int addr, int x) {
    x = x & 0xFF;

    if (uart_LCR[unit] & UART_LCR_DLAB) {
        if (addr == UART_DLL) { uart_DLL[unit] = x; return; }
        if (addr == UART_DLH) { uart_DLH[unit] = x; return; }
    }

    switch (addr) {
    case UART_TXBUF:
        // No real TX buffer: putchar() is the "sent immediately" jor1k
        // itself does (uart.js: "the data is sent immediately"). Unit
        // 1 has no real host terminal backing it -- writes to it are
        // still accepted (a real serial port would happily transmit
        // into the void too), just not echoed to our own stdout,
        // since that's the ttyS0/unit-0 console's job alone.
        uart_LSR[unit] = uart_LSR[unit] & ~UART_LSR_TRANSMITTER_EMPTY;
        if (unit == 0) putchar(x);
        uart_LSR[unit] = uart_LSR[unit] | UART_LSR_TRANSMITTER_EMPTY | UART_LSR_TX_EMPTY;
        uart_throw_interrupt(unit, UART_IIR_THRI);
        break;
    case UART_IER:
        uart_IER[unit] = x & 0x0F;
        uart_check_interrupt(unit);
        break;
    case UART_FCR:
        uart_FCR[unit] = x & 0xC9;
        if (uart_FCR[unit] & 2) {
            uart_ack_interrupt(unit, UART_IIR_CTI);
            rx_head[unit] = 0; rx_len[unit] = 0;
        }
        // FCR&4 (clear TX fifo): no-op, there's no TX buffer to clear.
        break;
    case UART_LCR:
        uart_LCR[unit] = x;
        break;
    case UART_MCR:
        uart_MCR[unit] = x;
        break;
    default:
        printf("uart_write8: unsupported register %d (unit %d)\n", addr, unit);
    }
}
