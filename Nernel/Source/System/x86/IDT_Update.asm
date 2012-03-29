.intel_syntax noprefix
.global IDT_Update
.global IDT_ISRDispatch
.global IDT_IRQDispatch

.extern IDT_ISRHandler
.extern IDT_IRQHandler

IDT_Update:
	cli
    mov eax, [esp]+4 
    lidt [eax]      
    ret 

IDT_ISRDispatch:
    pusha
    mov ax, ds
    push eax
 
/	mov edx, 0x2f8
/	mov eax, 13
/	outb dx, al
/	mov eax, 10
/	outb dx, al
/	mov eax, 37
/	outb dx, al
/	outb dx, al
/	outb dx, al
/	outb dx, al
/	mov eax, 13
/	outb dx, al
/	mov eax, 10
/	outb dx, al

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
 
    call IDT_ISRHandler
 
	pop eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

/	mov edx, 0x2f8
/	mov eax, 13
/	outb dx, al
/	mov eax, 10
/	outb dx, al
/	mov eax, 37
/	outb dx, al
/	outb dx, al
/	outb dx, al
/	outb dx, al
/	mov eax, 13
/	outb dx, al
/	mov eax, 10
/	outb dx, al
 
	popa
    add esp, 8
    sti
    iret
 
IDT_IRQDispatch:
    pusha
    mov ax, ds
    push eax
 
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
 
    call IDT_IRQHandler
 
    pop ebx
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx
 
    popa
    add esp, 8
    sti
    iret
