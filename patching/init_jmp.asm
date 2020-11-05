;
; Init code jump
; Overwrites an error check in the WinMain function
; For Tayutama
;

bits 32

extern _initPatch

SECTION .jinit

_start:

	call _initPatch
	jmp 0x00451f2a ; Jump over unused code
