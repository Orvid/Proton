.intel_syntax noprefix
.global GDT_Update
.global TSS_Update
.global TSS_GetTaskRegister
.global GDT_SwitchToUserMode
.global GDT_LogicalProcessorInit
.global GDT_LogicalProcessorInitEnd
.extern gEnteredUserModeAddress
.extern SMP_LogicalProcessorStartup
.extern gGDT_RegisterPointer
.extern gSMP_TempStackTop

GDT_Update:
	cli
    mov eax, [esp+4]
	mov gGDT_RegisterPointer, eax
    lgdt [eax]
 
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:GDT_Flush
GDT_Flush:
    ret

TSS_Update:
	mov eax, [esp + 4]
	ltr ax
	ret

TSS_GetTaskRegister:
	str eax
	ret

GDT_SwitchToUserMode:
	mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

	mov eax, esp
	push 0x23
	push eax
	pushf
	push 0x1B
	push gEnteredUserModeAddress
	iret

.code16
GDT_LogicalProcessorInit:
	/cli

	mov eax, 0xA000
	lgdt [eax]

	mov eax, cr0
	or eax, 0x01
	mov cr0, eax

	jmp 0x08:((GDT_LogicalProcessorEnterProtectedMode - GDT_LogicalProcessorInit) + 0x8000)

.code32
GDT_LogicalProcessorEnterProtectedMode:
	lea ecx, [GDT_LogicalProcessorEnteredProtectedMode]
	jmp ecx

GDT_LogicalProcessorEnteredProtectedMode:
	mov eax, gGDT_RegisterPointer
	lgdt [eax]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

	jmp 0x08:GDT_LogicalProcessorSetStack

GDT_LogicalProcessorSetStack:
	mov esp, gSMP_TempStackTop
	mov eax, 0x4000
	sub gSMP_TempStackTop, eax
	call SMP_LogicalProcessorStartup
	jmp $

GDT_LogicalProcessorInitEnd:
	ret
