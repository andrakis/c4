# c4or1k M3 echo-server: polls UART_LSR for data-ready, echoes each
# received byte back out through UART_TXBUF, and halts on Ctrl-D
# (0x04) -- run through run-c4or1k.sh, not the M1/M2 register-dump
# harness, though main.c's same cpu_step(halt_pc) loop drives it
# either way (see main.c's comment on run mode vs test mode).
#
# UART_MMIO_BASE = 0x90000000 (uart.h); LSR is register 5, RXBUF/
# TXBUF are both register 0 (read vs write, see uart.c).

    movhi r5, 0x9000    # r5 = 0x90000000 = UART_MMIO_BASE

loop:
    lbz   r1, 5(r5)     # LSR
    andi  r1, r1, 1     # UART_LSR_DATA_READY
    sfeqi r1, 0
    bf    loop          # not ready yet -- keep polling
    nop
    lbz   r2, 0(r5)     # RXBUF
    sb    0(r5), r2     # TXBUF: echo it straight back
    sfeqi r2, 4         # Ctrl-D
    bf    done
    nop
    j     loop
    nop
done:
    nop
