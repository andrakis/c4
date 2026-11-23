/* boot64.S -- c4pc64.elf's way in: the multiboot v1 header (GRUB, QEMU's
 * -kernel) as boot.S has it, then from the 32-bit protected mode a loader
 * leaves us in to long mode: the first 4 GB identity-mapped in 2 MB pages
 * (boot_pd, which screen.c marks write-combining over the framebuffer),
 * PAE, EFER.LME, paging; SSE turned on, as gcc's x86-64 code uses it for
 * every float. Then pc_main, as on i386. The ELF is linked for x86-64 and
 * made an ELF32 (objcopy), which is what multiboot loaders take; its
 * addresses are all below 4 GB.
 *
 * Also the interrupt stubs and c4_setjmp/c4_longjmp, in 64 bits. */
#define MB_MAGIC 0x1BADB002
#define MB_FLAGS 0x00000007        /* modules page-aligned, memory info, a video mode */

        .section .multiboot, "a"
        .align 4
        .long MB_MAGIC, MB_FLAGS, -(MB_MAGIC + MB_FLAGS)
        .long 0, 0, 0, 0, 0
        .long 0, 1024, 768, 32

        .text
        .code32
        .globl _start
_start: cli
        mov %eax, mb_magic
        mov %ebx, mb_info
        mov $stack_top, %esp
        /* long mode, or a word on the screen and a halt */
        mov $0x80000000, %eax
        cpuid
        cmp $0x80000001, %eax
        jb no64
        mov $0x80000001, %eax
        cpuid
        test $(1 << 29), %edx
        jz no64
        /* PML4[0] -> PDPT; PDPT[0..3] -> the four PDs; 2048 PDEs of 2 MB */
        mov $boot_pdpt, %eax
        or $3, %eax
        mov %eax, boot_pml4
        mov $boot_pd, %eax
        or $3, %eax
        mov $boot_pdpt, %edi
        mov $4, %ecx
1:      mov %eax, (%edi)
        movl $0, 4(%edi)
        add $4096, %eax
        add $8, %edi
        loop 1b
        mov $boot_pd, %edi
        xor %eax, %eax
        mov $2048, %ecx
2:      mov %eax, %edx
        shl $21, %edx                  /* the low word of i * 2 MB */
        or $0x83, %edx                 /* present, writable, 2 MB */
        mov %edx, (%edi)
        mov %eax, %edx
        shr $11, %edx                  /* the high word */
        mov %edx, 4(%edi)
        inc %eax
        add $8, %edi
        loop 2b
        mov $boot_pml4, %eax
        mov %eax, %cr3
        mov %cr4, %eax
        or $(0x20 | 0x200 | 0x400), %eax   /* PAE, OSFXSR, OSXMMEXCPT */
        mov %eax, %cr4
        mov $0xC0000080, %ecx              /* EFER */
        rdmsr
        or $0x100, %eax                    /* LME */
        wrmsr
        mov %cr0, %eax
        and $~0x4, %eax                    /* EM off: the FPU and SSE are there */
        or $(0x80000000 | 0x2 | 0x1), %eax /* PG, MP, PE */
        mov %eax, %cr0
        lgdt gdt64_ptr
        ljmp $0x08, $long_entry
no64:   mov $0xB8000, %edi
        mov $msg64, %esi
3:      lodsb
        test %al, %al
        jz 4f
        mov $0x4F, %ah
        stosw
        jmp 3b
4:      cli
        hlt
        jmp 4b

        .code64
long_entry:
        mov $0x10, %ax
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %ss
        mov %ax, %fs
        mov %ax, %gs
        mov $stack_top, %rsp
        push $0                        /* a null frame for backtraces */
        push $0
        mov mb_magic(%rip), %edi
        mov mb_info(%rip), %esi
        call pc_main
5:      cli
        hlt
        jmp 5b

/* c4_setjmp(buf), c4_longjmp(buf, v) (clib.h): rbx rbp r12-r15, the stack
 * as the caller has it after the call, and where it returns to */
        .globl c4_setjmp
c4_setjmp:
        mov %rbx, 0(%rdi)
        mov %rbp, 8(%rdi)
        mov %r12, 16(%rdi)
        mov %r13, 24(%rdi)
        mov %r14, 32(%rdi)
        mov %r15, 40(%rdi)
        lea 8(%rsp), %rcx
        mov %rcx, 48(%rdi)
        mov (%rsp), %rcx
        mov %rcx, 56(%rdi)
        xor %eax, %eax
        ret
        .globl c4_longjmp
c4_longjmp:
        mov %esi, %eax
        test %eax, %eax
        jnz 1f
        inc %eax
1:      mov 0(%rdi), %rbx
        mov 8(%rdi), %rbp
        mov 16(%rdi), %r12
        mov 24(%rdi), %r13
        mov 32(%rdi), %r14
        mov 40(%rdi), %r15
        mov 48(%rdi), %rsp
        jmp *56(%rdi)

/* gdt_load(ptr): load the GDT, reload the segments (code 0x08, data 0x10) */
        .globl gdt_load
gdt_load:
        lgdt (%rdi)
        mov $0x10, %ax
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %fs
        mov %ax, %gs
        mov %ax, %ss
        pop %rax
        push $0x08
        push %rax
        lretq

/* Interrupt stubs: a dummy error code where the CPU pushes none, the
 * vector, then the common entry saves every register and hands pc.c's
 * isr() the frame (struct regs). The stack is 16-aligned for the call. */
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
        .irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,240,255
        STUB \n
        .endr

isr_common:
        push %rax
        push %rcx
        push %rdx
        push %rbx
        push %rbp
        push %rsi
        push %rdi
        push %r8
        push %r9
        push %r10
        push %r11
        push %r12
        push %r13
        push %r14
        push %r15
        cld
        mov %rsp, %rdi
        mov %rsp, %rbx
        and $~15, %rsp
        call isr
        mov %rbx, %rsp
        pop %r15
        pop %r14
        pop %r13
        pop %r12
        pop %r11
        pop %r10
        pop %r9
        pop %r8
        pop %rdi
        pop %rsi
        pop %rbp
        pop %rbx
        pop %rdx
        pop %rcx
        pop %rax
        add $16, %rsp
        iretq

        .data
        .align 8
gdt64:  .quad 0
        .quad 0x00AF9A000000FFFF       /* 0x08: code, 64-bit */
        .quad 0x00CF92000000FFFF       /* 0x10: data */
gdt64_ptr:
        .word gdt64_ptr - gdt64 - 1
        .long gdt64
mb_magic: .long 0
mb_info:  .long 0
msg64:  .asciz "c4pc64: this CPU has no long mode (64-bit). Boot c4pc.elf, the i386 one."

        .bss
        .align 4096
        .globl boot_pml4, boot_pdpt, boot_pd
boot_pml4: .space 4096
boot_pdpt: .space 4096
boot_pd:   .space 4 * 4096
        .align 16
        .space 65536
stack_top:
