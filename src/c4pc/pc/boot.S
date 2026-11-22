/* boot.S -- c4pc's way in: a multiboot v1 header (GRUB, QEMU's -kernel),
 * a stack, and the interrupt stubs pc.c's IDT points at. */
#define MB_MAGIC 0x1BADB002
#define MB_FLAGS 0x00000003        /* modules page-aligned, memory info */

        .section .multiboot, "a"
        .align 4
        .long MB_MAGIC, MB_FLAGS, -(MB_MAGIC + MB_FLAGS)

        .text
        .globl _start
_start: cli
        mov $stack_top, %esp
        push $0                     /* a null frame for backtraces */
        push %ebx                   /* the multiboot information */
        push %eax                   /* the magic */
        call pc_main
1:      cli
        hlt
        jmp 1b

/* c4_setjmp(buf), c4_longjmp(buf, v) (clib.h): ebx esi edi ebp, the
 * stack as the caller has it after the call, and where it returns to.
 * The MMU's faults come back through these (mmu.c). */
        .globl c4_setjmp
c4_setjmp:
        mov 4(%esp), %eax
        mov %ebx, 0(%eax)
        mov %esi, 4(%eax)
        mov %edi, 8(%eax)
        mov %ebp, 12(%eax)
        lea 4(%esp), %ecx
        mov %ecx, 16(%eax)
        mov (%esp), %ecx
        mov %ecx, 20(%eax)
        xor %eax, %eax
        ret
        .globl c4_longjmp
c4_longjmp:
        mov 4(%esp), %edx
        mov 8(%esp), %eax
        test %eax, %eax
        jnz 1f
        inc %eax
1:      mov 0(%edx), %ebx
        mov 4(%edx), %esi
        mov 8(%edx), %edi
        mov 12(%edx), %ebp
        mov 16(%edx), %esp
        jmp *20(%edx)

/* gdt_load(ptr): load the GDT, reload the segments (code 0x08, data 0x10) */
        .globl gdt_load
gdt_load:
        mov 4(%esp), %eax
        lgdt (%eax)
        mov $0x10, %ax
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %fs
        mov %ax, %gs
        mov %ax, %ss
        ljmp $0x08, $2f
2:      ret

/* Interrupt stubs: each pushes a dummy error code where the CPU pushes
 * none, then its vector, and goes to the common entry, which hands
 * pc.c's isr() a pointer to the frame. */
.macro STUB n
        .globl isr\n
isr\n:
        .if (\n == 8) || (\n >= 10 && \n <= 14) || (\n == 17)
        .else
        push $0
        .endif
        push $\n
        jmp isr_common
.endm
        .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
        STUB \n
        .endr
        .irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
        STUB \n
        .endr

isr_common:
        pusha
        cld
        push %esp
        call isr
        add $4, %esp
        popa
        add $8, %esp
        iret

        .bss
        .align 16
        .space 65536
stack_top:
