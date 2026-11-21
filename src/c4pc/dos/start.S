/* start.S -- C4PC.COM's entry, and its jump into protected mode. */
        .code16
        .section .start, "ax"
        .globl _start
_start: cld
        movzwl %sp, %esp            /* gcc -m16 addresses the stack with esp */
        xorl %ebp, %ebp
        /* the BSS: a .COM gets its file and whatever follows, not zeros */
        movw $__bss_start, %di
        movw $__bss_end, %cx
        subw %di, %cx
        xorb %al, %al
        rep stosb
        calll main
        movb $0x4c, %ah             /* main returned: the loader failed */
        int $0x21

/* go_pm(entry, mbi, pm32_linear, gdtr_offset): never returns.
 * Interrupts off, the GDT loaded, PE set, then a far return to pm32 --
 * code in this file, at its linear address, in a flat 32-bit segment. */
        .text
        .globl go_pm
go_pm:  cli
        movl 4(%esp), %edi          /* entry */
        movl 8(%esp), %ebx          /* the multiboot information, linear */
        movl 12(%esp), %esi         /* pm32, linear */
        movl 16(%esp), %eax         /* &gdtr, in this segment */
        lgdtl (%eax)
        movl %cr0, %eax
        orl $1, %eax
        movl %eax, %cr0
        pushl $0x08
        pushl %esi
        lretl
        .code32
        .globl pm32
pm32:   movw $0x10, %ax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %fs
        movw %ax, %gs
        movw %ax, %ss
        movl $0x9fff0, %esp
        movl $0x2BADB002, %eax
        jmp *%edi
