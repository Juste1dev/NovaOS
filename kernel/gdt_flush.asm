

global gdt64_flush

section .text
bits 64
gdt64_flush:
    lgdt [rdi]

    mov ax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rax, [rel .flush]
    push rsi
    push rax
    retfq

.flush:
    ret
