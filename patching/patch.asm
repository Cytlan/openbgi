;
; Patch to log the program flow
; For Tayutama
;

bits 32

global _start
global _execOpcodePatch
global _initPatch

SECTION .patch

; Imports from Tayutama
GetLastError   equ 0x004800e0
LoadLibraryA   equ 0x00480100
GetProcAddress equ 0x00480178
ExitProcess    equ 0x00480190
MessageBoxA    equ 0x00480350

_start:

; This routine should be executed in place of the call to the jumptable.
; We should hand over control to our DLL in order to do some more extensive logging than
; what we're easily able to to in assembly.
_execOpcodePatch:
	push ESI

	; Re-push VMThread var
	mov EDI, dword [ESP + 0x8]
	push EDI
	; Push opcode
	push ESI

	; Call into the DLL
	call [libProcExecuteOpcode]
	; EAX: Contains the result of the execution (We must keep this!)

	; Return to the original routine and continue execution there
	add ESP, 0x08 ; Discard the VMThread arg and opcode arg
	pop ESI       ; Pop the opcode back into ESI where we found it
	ret

; Initialise the patch DLL and load relevant function addresses
_initPatch:
	; Attempt to load our DLL
	push libName
	call [LoadLibraryA]
	test EAX, EAX
	jnz .continue
		; Loading DLL failed, report error
		push strLoadLibraryA
		jmp reportError
.continue:
	mov dword [libHandle], EAX

	; Load functions
	mov ESI, 0x00
	.loadLoop:
		; Get string address
		mov EBX, [ESI*8+libStrProc]

		; End if NULL
		test EBX, EBX
		jz .endLoadLoop

		; procAddr = GetProcAddress(libStrProc[index*2], libHandle)
		push EBX
		push dword [libHandle]
		call [GetProcAddress]

		; Report error, if any
		test EAX, EAX
		jnz .continueLoadLoop
			push EBX
			jmp reportError
		.continueLoadLoop:

		; libStrProc[(index*2)+4] = procAddr
		mov [ESI*8+libStrProc+4], EAX
		inc ESI
		jmp .loadLoop
	.endLoadLoop:

	; Call init() function
	call [libProcInit]

	ret

; Report GetLastError code and end the process
reportError:
	call [GetLastError] ; Returns error code in EAX

	; Write EAX as a hex string
	xor ESI, ESI
	.errLoop:
		mov ECX, EAX
		shl EAX, 4
		shr ECX, 28
		and ECX, 0xF
		mov byte CL, [ECX + hexChar]
		mov byte [ESI + errMsg], CL
		inc ESI
		cmp ESI, 8
	jne .errLoop
	mov byte [ESI + errMsg], 0x00 ; Remember NULL terminator

	; MessageBoxA(NULL, errMsg, errTitle, 0);
	push 0
	push dword [ESP+0x4]
	push errMsg
	push 0
	call [MessageBoxA]

	; End program
	push 1
	call [ExitProcess]
	ret

; DLL loading
libHandle:               dd 0
libName:                 db "patch.dll", 0x00

; Function names
libProcInitStr:           db "init", 0x00
libProcExecuteOpcodeStr:  db "executeOpcode", 0x00
libProcBGI_ReadCode8Str:  db "BGI_ReadCode8", 0x00
libProcBGI_ReadCode16Str: db "BGI_ReadCode16", 0x00
libProcBGI_ReadCode32Str: db "BGI_ReadCode32", 0x00
libProcBGI_PopStackStr:   db "BGI_PopStack", 0x00
libProcBGI_PushStackStr:  db "BGI_PushStack", 0x00

libStrProc:
	dd libProcInitStr
	libProcInit: dd 0

	dd libProcExecuteOpcodeStr
	libProcExecuteOpcode: dd 0

	dd libProcBGI_ReadCode8Str
	libProcBGI_ReadCode8: dd 0

	dd libProcBGI_ReadCode16Str
	libProcBGI_ReadCode16: dd 0

	dd libProcBGI_ReadCode32Str
	libProcBGI_ReadCode32: dd 0

	dd libProcBGI_PopStackStr
	libProcBGI_PopStack: dd 0

	dd libProcBGI_PushStackStr
	libProcBGI_PushStack: dd 0

	; End if imports
	dd 0

; Error message generation
hexChar:  db "0123456789ABCDEF"
errMsg:   db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

; Error titles
strPatchError: db "Patch error", 0x00
strLoadLibraryA: db "LoadLibraryA", 0x00
