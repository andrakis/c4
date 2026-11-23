/* aptramp.S -- where the PC's other processors start (ap.c): a Startup
 * IPI's vector 07h starts each in real mode at 0700:0000, so ap.c copies
 * this page to 0x7000. It goes to protected mode on a GDT of its own,
 * takes a number (lock xadd: the order they arrive in), a stack by it,
 * the boot processor's CR4, CR3 and CR0 (its paging, its FPU and SSE
 * bits) and, for c4pc64.elf, EFER.LME and long mode; then ap_entry(n),
 * which loads the kernel's own GDT and IDT (pc.c). ap.c fills the words
 * at the end before the IPIs. */
#define T 0x7000
#define A(x) (T + (x) - ap_tramp_start)

        .text
        .globl ap_tramp_start, ap_tramp_end, ap_tramp_count, ap_tramp_stacks, ap_tramp_stack_size
        .globl ap_tramp_cr0, ap_tramp_cr3, ap_tramp_cr4, ap_tramp_entry
        .code16
ap_tramp_start:
        cli
        cld
        xor %ax, %ax
        mov %ax, %ds
        lgdtl A(tgdt_ptr)
        mov %cr0, %eax
        or $1, %eax
        mov %eax, %cr0
        ljmpl $0x08, $A(pm32)

        .code32
pm32:   mov $0x10, %ax
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %ss
        mov %ax, %fs
        mov %ax, %gs
        mov $1, %eax
        lock xadd %eax, A(ap_tramp_count)
        mov %eax, %ebx                  /* this processor's number, from 0 */
        lea 1(%ebx), %eax
        imul A(ap_tramp_stack_size), %eax
        add A(ap_tramp_stacks), %eax
        mov %eax, %esp                  /* the top of stack n */
        mov A(ap_tramp_cr4), %eax
        mov %eax, %cr4
        mov A(ap_tramp_cr3), %eax
        mov %eax, %cr3
#ifdef __x86_64__
        mov $0xC0000080, %ecx           /* EFER.LME */
        rdmsr
        or $0x100, %eax
        wrmsr
        mov A(ap_tramp_cr0), %eax
        mov %eax, %cr0                  /* paging on: long mode, still in 32-bit code */
        ljmp $0x18, $A(lm64)
        .code64
lm64:   mov $0x10, %ax
        mov %ax, %ds
        mov %ax, %ss
        mov %esp, %eax                  /* a 32-bit move clears the top half, which */
        mov %rax, %rsp                  /* the switch from compatibility mode leaves undefined */
        mov $A(ap_tramp_entry), %esi
        mov (%rsi), %rax
        mov %ebx, %edi
        and $~15, %rsp
        call *%rax
1:      cli
        hlt
        jmp 1b
#else
        mov A(ap_tramp_cr0), %eax
        mov %eax, %cr0
        ljmp $0x08, $A(pg32)
pg32:   push %ebx
        call *A(ap_tramp_entry)
1:      cli
        hlt
        jmp 1b
#endif

        .align 8
tgdt:   .quad 0
        .quad 0x00CF9A000000FFFF        /* 0x08: code, 32-bit */
        .quad 0x00CF92000000FFFF        /* 0x10: data */
        .quad 0x00AF9A000000FFFF        /* 0x18: code, 64-bit */
tgdt_ptr:
        .word tgdt_ptr - tgdt - 1
        .long A(tgdt)
        .align 8
ap_tramp_count:      .long 0
ap_tramp_stacks:     .long 0
ap_tramp_stack_size: .long 0
ap_tramp_cr0:        .long 0
ap_tramp_cr3:        .long 0
ap_tramp_cr4:        .long 0
ap_tramp_entry:      .quad 0
ap_tramp_end:
