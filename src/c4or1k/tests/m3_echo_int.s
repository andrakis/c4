# c4or1k M3 echo-server, interrupt-driven instead of polled: enables
# UART_IER_RDI and SR_IEE, spins doing nothing, and lets EXCEPT_INT
# (raised via cpu_raise_interrupt from uart.c, delivered through
# PICMR/PICSR exactly as cpu_check_for_interrupt does) drive the
# actual echo from its handler at the real vector address. Exercises
# the whole M2+M3 IRQ pipeline end to end: UART -> PIC -> SR_IEE ->
# EXCEPT_INT -> handler -> l.rfe.
#
# On Ctrl-D the handler redirects EPCR to final_halt (via %hi/%lo,
# same trick tests/m1_test.s uses for l.jalr) instead of returning to
# the spin loop, so main.c's cpu_step(halt_pc) loop still terminates
# cleanly -- there's no other way to "fall off the end" of a program
# whose main flow is an intentional infinite spin.

    movhi r5, 0x9000    # r5 = UART_MMIO_BASE
    ori   r6, r0, 1
    sb    1(r5), r6      # UART_IER = RDI enabled
    ori   r8, r0, 4
    mtspr r0, r8, 0x4800 # PICMR |= line 2's bit (cpu_set_spr ORs in 0x3 unconditionally too)
    ori   r7, r0, 5      # SR: SM(bit0) | IEE(bit2)
    mtspr r0, r7, 17

spin:
    nop
    j     spin
    nop

.org 0x200               # EXCEPT_INT vector (word address; 0x800 bytes)
int_handler:
    lbz   r10, 0(r5)     # RXBUF
    sb    0(r5), r10     # echo
    sfeqi r10, 4         # Ctrl-D
    bf    int_done
    nop
    rfe                  # normal case: resume the spin loop
int_done:
    movhi r11, %hi(final_halt)
    ori   r11, r11, %lo(final_halt)
    mtspr r0, r11, 32    # SPR_EPCR_BASE -- redirect the return address itself
    rfe                  # lands at final_halt, not back in the spin loop

final_halt:
    nop
