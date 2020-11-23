//
// Basic opcode re-implementations
//

#include "bgi.h"
#include "patch.h"
#include "debug.h"

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

// -----------------------------------------------------------------------------
// Mnemonic:  codeptr
// Opcode:    0x05
// Stack in:     0
// Stack out:    1
// Bytes:        3
// -----------------------------------------------------------------------------
int op_codeptr(VMThread_t* thread)
{
	uint16_t offset = BGI_ReadCode16(thread);
	uint32_t ip = BGI_GetInstructionPointer(thread);
	BGI_PushStack(thread, (ip + offset) | BGI_CODE_OFFSET);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  codeoffset
// Opcode:    0x06
// Stack in:     0
// Stack out:    1
// Bytes:        3
// -----------------------------------------------------------------------------
int op_codeoffset(VMThread_t* thread)
{
	int16_t offset = BGI_ReadCode16(thread);
	uint32_t ip = BGI_GetInstructionPointer(thread);
	BGI_PushStack(thread, ip + offset);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  readmem
// Opcode:    0x08
// Stack in:     1
// Stack out:    1
// Bytes:        2
// -----------------------------------------------------------------------------
int op_readmem(VMThread_t* thread)
{
	uint8_t* ptr = BGI_PopAndResolveAddress(thread);
	int intSize = BGI_ReadCode8(thread);

	if(intSize == 0)
		BGI_PushStack(thread, *ptr);
	else if(intSize == 1)
		BGI_PushStack(thread, *(uint16_t*)ptr);
	else if(intSize == 2)
		BGI_PushStack(thread, *(uint32_t*)ptr);
	else
		BGI_PushStack(thread, (uint32_t)thread); // ???

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  writecopy
// Opcode:    0x09
// Stack in:     2
// Stack out:    1
// Bytes:        1
// -----------------------------------------------------------------------------
int op_writecopy(VMThread_t* thread)
{
	uint32_t data = BGI_PopStack(thread);
	uint8_t* ptr = BGI_PopAndResolveAddress(thread);
	int intSize = BGI_ReadCode8(thread);
	
	// Write to mem
	BGI_WriteIntToMemory(ptr, intSize, data);

	// Push data back onto the stack
	BGI_PushStack(thread, data);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  write
// Opcode:    0x0A
// Stack in:     2
// Stack out:    0
// Bytes:        1
// -----------------------------------------------------------------------------
int op_write(VMThread_t* thread)
{
	uint8_t* ptr = BGI_PopAndResolveAddress(thread);
	uint32_t data = BGI_PopStack(thread);
	int intSize = BGI_ReadCode8(thread);
	
	// Write to mem
	BGI_WriteIntToMemory(ptr, intSize, data);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  copystack
// Opcode:    0x0C
// Stack in:   1+?
// Stack out:    0
// Bytes:        2
// -----------------------------------------------------------------------------
int op_copystack(VMThread_t* thread)
{
	int intSize = BGI_ReadCode8(thread);
	int count = BGI_ReadCode8(thread);

	// Pop all the values
	uint32_t values[256];
	for(int i = 0; i < count; i++)
		values[i] = BGI_PopStack(thread);

	// Write all the values to memory in reverse order
	uint8_t* dst = BGI_PopAndResolveAddress(thread);
	for(int i = count-1; i >= 0; i--)
	{
		BGI_WriteIntToMemory(dst, intSize, values[i]);
		dst += 1 << intSize;
	}

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memptr
// Opcode:    0x10
// Stack in:     0
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_memptr2(VMThread_t* thread)
{
	BGI_PushStack(thread, BGI_GetMemPointer(thread));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  storememptr
// Opcode:    0x11
// Stack in:     1
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int op_storememptr(VMThread_t* thread)
{
	uint32_t newPtr = BGI_PopStack(thread);
	if(newPtr >= BGI_GetLocalMemSize(thread))
	{
		MessageBox(NULL, "Out of bounds memptr", "Error", 0);
		ExitProcess(1);
	}
	BGI_SetMemPointer(thread, newPtr);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  jmp
// Opcode:    0x14
// Stack in:     1
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int __cdecl op_jmp(VMThread_t* thread)
{
	BGI_SetInstructionPointer(thread, BGI_PopStack(thread));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  [jnz, jz, jg, jge, jle, jl, ja]
// Opcode:    0x15
// Stack in:     2
// Stack out:    0
// Bytes:        1
// -----------------------------------------------------------------------------
int op_cjmp(VMThread_t* thread)
{
	uint8_t op = BGI_ReadCode8(thread);
	uint32_t dest = BGI_PopStack(thread);
	int32_t value = BGI_PopStack(thread);
	bool res = true;
	switch(op)
	{
		case 0:
			res = value != 0;
			break;
		case 1:
			res = value == 0;
			break;
		case 2:
			res = value > 0;
			break;
		case 3:
			res = value >= 0;
			break;
		case 4:
			res = value <= 0;
			break;
		case 5:
			res = value < 0;
			break;
		default:
			res = true;
	}
	if(res)
	{
		if(dest >= BGI_GetCodeSpaceSize(thread))
		{
			printf("Error: Jump destination is outside code space\n");
			haltExecution(thread);
			return 0;
		}
		BGI_SetInstructionPointer(thread, dest);
	}
	return 0;
}


// -----------------------------------------------------------------------------
// Mnemonic:  add
// Opcode:    0x20
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_add(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left + right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  sub
// Opcode:    0x21
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_sub(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left - right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  mul
// Opcode:    0x22
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_mul(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left * right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  div
// Opcode:    0x23
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_div(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	if(right == 0)
		BGI_PushStack(thread, -1);
	else
		BGI_PushStack(thread, left / right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  mod
// Opcode:    0x24
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_mod(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	if(right == 0)
		BGI_PushStack(thread, -1);
	else
		BGI_PushStack(thread, left % right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  and
// Opcode:    0x25
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_and(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left & right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  or
// Opcode:    0x26
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_or(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left | right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  xor
// Opcode:    0x27
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_xor(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left ^ right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  not
// Opcode:    0x28
// Stack in:     1
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_not(VMThread_t* thread)
{
	uint32_t value = BGI_PopStack(thread);
	BGI_PushStack(thread, ~value);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  shl
// Opcode:    0x29
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_shl(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left << (right & 0x1F));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  shr
// Opcode:    0x2A
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_shr(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left >> (right & 0x1F));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  sar
// Opcode:    0x2B
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_sar(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left >> (right & 0x1F));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  eq
// Opcode:    0x30
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_eq(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left == right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  neq
// Opcode:    0x31
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_neq(VMThread_t* thread)
{
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left != right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  leq
// Opcode:    0x32
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_leq(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left <= right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  geq
// Opcode:    0x33
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_geq(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left >= right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  le
// Opcode:    0x34
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_le(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left < right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  ge
// Opcode:    0x35
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_ge(VMThread_t* thread)
{
	int32_t right = BGI_PopStack(thread);
	int32_t left = BGI_PopStack(thread);
	BGI_PushStack(thread, left > right);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  dnotzero
// Opcode:    0x38
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_dnotzero(VMThread_t* thread)
{
	// Why does this exist? The result is the same as op_dnotzero2
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);

	// If not zero, push 1, otherwise push 0
	if((left != 0) && (right != 0))
		BGI_PushStack(thread, 1);
	else
		BGI_PushStack(thread, 0);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  dnotzero2
// Opcode:    0x39
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_dnotzero2(VMThread_t* thread)
{
	// Why does this exist? The result is the same as op_dnotzero
	uint32_t right = BGI_PopStack(thread);
	uint32_t left = BGI_PopStack(thread);

	// If zero, push 0, otherwise push 1
	if((left == 0) && (right == 0))
		BGI_PushStack(thread, 0);
	else
		BGI_PushStack(thread, 1);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  iszero
// Opcode:    0x3A
// Stack in:     1
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_iszero(VMThread_t* thread)
{
	BGI_PushStack(thread, BGI_PopStack(thread) == 0);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  ternary
// Opcode:    0x40
// Stack in:     3
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_ternary(VMThread_t* thread)
{
	uint32_t falseVal = BGI_PopStack(thread);
	uint32_t trueVal = BGI_PopStack(thread);
	uint32_t compare = BGI_PopStack(thread);
	
	if(compare)
		BGI_PushStack(thread, trueVal);
	else
		BGI_PushStack(thread, falseVal);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  muldiv
// Opcode:    0x42
// Stack in:     3
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_muldiv(VMThread_t* thread)
{
	int32_t divisor = BGI_PopStack(thread);
	int32_t multiplier = BGI_PopStack(thread);
	int32_t multiplicand = BGI_PopStack(thread);

	int64_t tmp = (int64_t)multiplicand * (int64_t)multiplier;
	if(divisor == 0)
		BGI_PushStack(thread, -1);
	else
		BGI_PushStack(thread, (int32_t)(tmp / (int64_t)divisor));

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  sin
// Opcode:    0x48
// Stack in:     1
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_sin(VMThread_t* thread)
{
	int32_t angle = BGI_PopStack(thread);

	double tmp = sin((double)angle * ((M_PI / 180.0l) / 65536.0l));

	BGI_PushStack(thread, (int32_t)(tmp * 65536.0l));

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  cos
// Opcode:    0x49
// Stack in:     1
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_cos(VMThread_t* thread)
{
	int32_t angle = BGI_PopStack(thread);

	double tmp = cos((double)angle * ((M_PI / 180.0l) / 65536.0l));

	BGI_PushStack(thread, (int32_t)(tmp * 65536.0l));

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memcpy
// Opcode:    0x60
// Stack in:     3
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int op_memcpy(VMThread_t* thread)
{
	uint32_t size = BGI_PopStack(thread);
	uint8_t* src = BGI_PopAndResolveAddress(thread);
	uint8_t* dst = BGI_PopAndResolveAddress(thread);

	memcpy(dst, src, size);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memclr
// Opcode:    0x61
// Stack in:     2
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int op_memclr(VMThread_t* thread)
{
	uint32_t size = BGI_PopStack(thread);
	uint8_t* ptr = BGI_PopAndResolveAddress(thread);

	memset(ptr, 0, size);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memset
// Opcode:    0x62
// Stack in:     3
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int op_memset(VMThread_t* thread)
{
	uint32_t val = BGI_PopStack(thread);
	uint32_t size = BGI_PopStack(thread);
	uint8_t* ptr = BGI_PopAndResolveAddress(thread);

	memset(ptr, (uint8_t)val, size);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  memcmp
// Opcode:    0x63
// Stack in:     3
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_memcmp(VMThread_t* thread)
{
	uint32_t size = BGI_PopStack(thread);
	uint8_t* src = BGI_PopAndResolveAddress(thread);
	uint8_t* dst = BGI_PopAndResolveAddress(thread);

	if(memcmp(dst, src, size) == 0)
		BGI_PushStack(thread, 1);
	else
		BGI_PushStack(thread, 0);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  strreplace
// Opcode:    0x67
// Stack in:     4
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
// TODO: Find some code that uses this opcode... I have no idea what I'm looking for.
/*
bool BGI_IsDoubleByteSJIS(char c)
{
	if((c < 0x80 || c > 0x9F) && c < 0xE0)
		return false;
	return true;
}

int BGI_CopySJISChar(uint32_t* dst, char* src)
{ 
	if(BGI_IsDoubleByteSJIS(*src))
	{
		*dst = *(uint16_t*)src & 0xFFFF;
		return 1;
	}
	*dst = *src & 0xFF;
	return 0;
}

int BGI_Strunkonwn(char* a, char* search)
{
	int searchLen = strlen(search);
	uint32_t* buf = (char*)malloc(searchLen * sizeof(uint32_t));

	char* ptr = search;

	int len = 0;
	char c = *ptr;
	while(c)
	{
		if(BGI_CopySJISChar(buf++, ptr++))
			ptr++;
		len++;
	}
	char* aPtr = a;
	c = *aPtr;
	while(c)
	{

	}
}

uint8_t* BGI_Strreplace(char* a, char* b, char* search, char* d)
{
	int searchLen = strlen(search);
	int dLen = strlen(d);

	int len = BGI_Strunkonwn(b, search);
}
*/

int op_strreplace(VMThread_t* thread)
{
	uint8_t* d = BGI_PopAndResolveAddress(thread);
	uint8_t* c = BGI_PopAndResolveAddress(thread);
	uint8_t* b = BGI_PopAndResolveAddress(thread);
	uint8_t* a = BGI_PopAndResolveAddress(thread);

	printf("op_strreplace: a: %.8X, b: %.8X, c: %.8X, d: %.8X\n", a, b, c, d);

	//uint8_t* res = BGI_Strreplace(a, b, c, d);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  strlen
// Opcode:    0x68
// Stack in:     1
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------

int op_strlen(VMThread_t* thread)
{
	uint8_t* str = BGI_PopAndResolveAddress(thread);

	BGI_PushStack(thread, strlen(str));
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  streq
// Opcode:    0x69
// Stack in:     2
// Stack out:    1
// Bytes:        0
// -----------------------------------------------------------------------------
int op_streq(VMThread_t* thread)
{
	uint8_t* strA = BGI_PopAndResolveAddress(thread);
	uint8_t* strB = BGI_PopAndResolveAddress(thread);

	BGI_PushStack(thread, strcmp(strA, strB) == 0);
	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  strcpu
// Opcode:    0x6A
// Stack in:     2
// Stack out:    0
// Bytes:        0
// -----------------------------------------------------------------------------
int op_strcpy(VMThread_t* thread)
{
	uint8_t* right = BGI_PopAndResolveAddress(thread);
	uint8_t* left = BGI_PopAndResolveAddress(thread);

	Debug_PrintfSJIS(L"strcpy", right);

	strcpy(left, right);

	return 0;
}

// -----------------------------------------------------------------------------
// Mnemonic:  sys
// Opcode:    0x80
// Stack in:     ?
// Stack out:    ?
// Bytes:      1+?
// -----------------------------------------------------------------------------
int op_sys0(VMThread_t* thread)
{
	uint8_t sysOpcode = BGI_ReadCode8(thread);
	if(sys0Jumptable[sysOpcode] == NULL)
	{
		uint8_t opcode = thread->codeSpace[thread->instructionPointer];
		printf("Undefined sys0 opcode: 0x%.2X (%d) at address %.8LX\n", sysOpcode, sysOpcode, thread->instructionPointer);
		thread->instructionPointer -= 2;
		thread->programCounter -= 2;
		haltExecution(thread);
		return 0;
	}
	return sys0Jumptable[sysOpcode](thread);
}
