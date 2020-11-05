;
; Patch for jumping into our code cave whenever the VM tries to execute an opcode
; For Tayutama
;

bits 32

; Address for out code cave
extern _execOpcodePatch

SECTION .jexec

; Short and simple
call _execOpcodePatch
nop ; nop's required to fill the unused space
nop
