

MBALIGN  equ 1 << 0
MEMINFO  equ 1 << 1
VIDINFO  equ 1 << 2
FLAGS    equ MBALIGN | MEMINFO | VIDINFO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0
    dd 0
    dd 1920
    dd 1080
    dd 32

section .data
mb_magic:  dd 0
mb_info:   dd 0

align 8
gdt64:
    dq 0
.code64: equ $ - gdt64
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)
.data64: equ $ - gdt64
    dq (1<<41)|(1<<44)|(1<<47)
.ptr:
    dw $ - gdt64 - 1
    dq gdt64

section .bss
align 16
stack_bottom:
    resb 131072
stack_top:

align 4096
pml4:   resb 4096
pdpt:   resb 4096
pd0:    resb 4096
pd1:    resb 4096
pd2:    resb 4096
pd3:    resb 4096

section .text
bits 32
global _start
_start:
    mov esp, stack_top

    mov [mb_magic], eax
    mov [mb_info],  ebx

    pushfd
    pop   eax
    mov   ecx, eax
    xor   eax, (1<<21)
    push  eax
    popfd
    pushfd
    pop   eax
    push  ecx
    popfd
    cmp   eax, ecx
    je    .no64

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb  .no64
    mov eax, 0x80000001
    cpuid
    test edx, (1<<29)
    jz  .no64

    call build_page_tables

    lgdt [gdt64.ptr]

    mov eax, cr4
    or  eax, (1<<5)
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1<<8)
    wrmsr

    mov eax, cr0
    or  eax, (1<<31)|(1<<0)
    mov cr0, eax

    jmp gdt64.code64:long_mode_entry

.no64:
    hlt
    jmp .no64

build_page_tables:

    mov eax, pdpt
    or  eax, 0x03
    mov [pml4], eax

    mov eax, pd0
    or  eax, 0x03
    mov [pdpt + 0], eax
    mov eax, pd1
    or  eax, 0x03
    mov [pdpt + 8], eax
    mov eax, pd2
    or  eax, 0x03
    mov [pdpt + 16], eax
    mov eax, pd3
    or  eax, 0x03
    mov [pdpt + 24], eax

    mov edi, pd0
    mov eax, 0x00000083
    mov ecx, 4 * 512
.fill_pd:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    dec ecx
    jnz .fill_pd

    ret

bits 64
long_mode_entry:

    mov ax, gdt64.data64
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top

    mov edi, dword [mb_magic]
    mov esi, dword [mb_info]

    extern kernel_main
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
