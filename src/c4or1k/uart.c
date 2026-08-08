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

int uart_LCR, uart_LSR, uart_MSR, uart_ints, uart_IIR, uart_IER, uart_DLL, uart_DLH, uart_FCR, uart_MCR;

enum { RXBUF_CAP = 256 };
int rxbuf[RXBUF_CAP];
int rx_head, rx_len;

void uart_reset() {
    uart_LCR = 0x3;
    uart_LSR = UART_LSR_TRANSMITTER_EMPTY | UART_LSR_TX_EMPTY;
    uart_MSR = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS;
    uart_ints = 0;
    uart_IIR = UART_IIR_NO_INT;
    uart_IER = 0;
    uart_DLL = 0; uart_DLH = 0; uart_FCR = 0; uart_MCR = 0;
    rx_head = 0; rx_len = 0;
}

// Priority order (CTI, then THRI, then MSI) matches
// UARTDev.prototype.CheckInterrupt exactly.
void uart_check_interrupt() {
    if ((uart_ints & (1 << UART_IIR_CTI)) && (uart_IER & UART_IER_RDI)) {
        uart_IIR = UART_IIR_CTI;
        cpu_raise_interrupt(UART_INTNO);
    } else if ((uart_ints & (1 << UART_IIR_THRI)) && (uart_IER & UART_IER_THRI)) {
        uart_IIR = UART_IIR_THRI;
        cpu_raise_interrupt(UART_INTNO);
    } else if ((uart_ints & (1 << UART_IIR_MSI)) && (uart_IER & UART_IER_MSI)) {
        uart_IIR = UART_IIR_MSI;
        cpu_raise_interrupt(UART_INTNO);
    } else {
        uart_IIR = UART_IIR_NO_INT;
        cpu_clear_interrupt(UART_INTNO);
    }
}

void uart_throw_interrupt(int line) {
    uart_ints = uart_ints | (1 << line);
    uart_check_interrupt();
}

void uart_ack_interrupt(int line) {
    uart_ints = uart_ints & ~(1 << line);
    uart_check_interrupt();
}

void uart_receive_char(int c) {
    if (rx_len < RXBUF_CAP) {
        rxbuf[(rx_head + rx_len) % RXBUF_CAP] = c & 0xFF;
        rx_len = rx_len + 1;
    }
    if (rx_len > 0) {
        uart_LSR = uart_LSR | UART_LSR_DATA_READY;
        uart_throw_interrupt(UART_IIR_CTI);
    }
}

int uart_read8(int addr) {
    int ret;

    if (uart_LCR & UART_LCR_DLAB) {
        if (addr == UART_DLL) return uart_DLL;
        if (addr == UART_DLH) return uart_DLH;
    }

    if (addr == UART_RXBUF) {
        ret = 0;
        if (rx_len > 0) {
            ret = rxbuf[rx_head];
            rx_head = (rx_head + 1) % RXBUF_CAP;
            rx_len = rx_len - 1;
        }
        if (rx_len == 0) {
            uart_LSR = uart_LSR & ~UART_LSR_DATA_READY;
            uart_ack_interrupt(UART_IIR_CTI);
        }
        return ret & 0xFF;
    } else if (addr == UART_IER) {
        return uart_IER & 0x0F;
    } else if (addr == UART_MSR) {
        ret = uart_MSR;
        uart_MSR = uart_MSR & 0xF0; // reset the "delta" bits, matches uart.js
        return ret;
    } else if (addr == UART_IIR) {
        ret = (uart_IIR & 0x0F) | 0xC0; // top two bits (fifo enabled) always set
        if (uart_IIR == UART_IIR_THRI) uart_ack_interrupt(UART_IIR_THRI);
        return ret;
    } else if (addr == UART_LCR) {
        return uart_LCR;
    } else if (addr == UART_LSR) {
        return uart_LSR;
    }
    printf("uart_read8: unsupported register %d\n", addr);
    return 0;
}

void uart_write8(int addr, int x) {
    x = x & 0xFF;

    if (uart_LCR & UART_LCR_DLAB) {
        if (addr == UART_DLL) { uart_DLL = x; return; }
        if (addr == UART_DLH) { uart_DLH = x; return; }
    }

    if (addr == UART_TXBUF) {
        // No real TX buffer: putchar() is the "sent immediately" jor1k
        // itself does (uart.js: "the data is sent immediately").
        uart_LSR = uart_LSR & ~UART_LSR_TRANSMITTER_EMPTY;
        putchar(x);
        uart_LSR = uart_LSR | UART_LSR_TRANSMITTER_EMPTY | UART_LSR_TX_EMPTY;
        uart_throw_interrupt(UART_IIR_THRI);
    } else if (addr == UART_IER) {
        uart_IER = x & 0x0F;
        uart_check_interrupt();
    } else if (addr == UART_FCR) {
        uart_FCR = x & 0xC9;
        if (uart_FCR & 2) {
            uart_ack_interrupt(UART_IIR_CTI);
            rx_head = 0; rx_len = 0;
        }
        // FCR&4 (clear TX fifo): no-op, there's no TX buffer to clear.
    } else if (addr == UART_LCR) {
        uart_LCR = x;
    } else if (addr == UART_MCR) {
        uart_MCR = x;
    } else {
        printf("uart_write8: unsupported register %d\n", addr);
    }
}
