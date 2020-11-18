//
// Basic opcode re-implementations
//

#include "bgi.h"
#include "patch.h"

// -----------------------------------------------------------------------------
// Mnemonic:  push8
// Opcode:    0x00
// Stack in:     0
// Stack out:    1
// Bytes:        2
// -----------------------------------------------------------------------------
int op_push8(VMThread_t* thread)
{
	BGI_PushStack(thread, BGI_ReadCode8(thread));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  push16
// Opcode:    0x01
// Stack in:     0
// Stack out:    1
// Bytes:        3
// -----------------------------------------------------------------------------
int op_push16(VMThread_t* thread)
{
	BGI_PushStack(thread, BGI_ReadCode16(thread));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  push32
// Opcode:    0x02
// Stack in:     0
// Stack out:    1
// Bytes:        5
// -----------------------------------------------------------------------------
int op_push32(VMThread_t* thread)
{
	BGI_PushStack(thread, BGI_ReadCode32(thread));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memptr
// Opcode:    0x04
// Stack in:     0
// Stack out:    1
// Bytes:        3
// -----------------------------------------------------------------------------
int op_memptr(VMThread_t* thread)
{
	uint16_t offset = BGI_ReadCode16(thread);
	uint32_t memPtr = BGI_GetMemPointer(thread);
	BGI_PushStack(thread, (memPtr - offset) | BGI_MEMORY_OFFSET);
	return 0;
}
