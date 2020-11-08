//
// Disassembler
//

#include "patch.h"

typedef struct BGIOpcode
{
	char* mnemonic;
	int numOperands;
	char operands[4];
} BGIOpcode_t;

BGIOpcode_t basicInstructions[256] = {
	// 0x00 push8
	{
		.mnemonic = "push8",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x01 push16
	{
		.mnemonic = "push16",
		.numOperands = 1,
		.operands = {2,0,0,0}
	},
	// 0x02 push32
	{
		.mnemonic = "push32",
		.numOperands = 1,
		.operands = {4,0,0,0}
	},
	// 0x03 ----
	{},
	// 0x04 baseptr
	{
		.mnemonic = "baseptr",
		.numOperands = 1,
		.operands = {2,0,0,0}
	},
	// 0x05 codeptr
	{
		.mnemonic = "codeptr",
		.numOperands = 1,
		.operands = {2,0,0,0}
	},
	// 0x06 codeoffset
	{
		.mnemonic = "codeoffset",
		.numOperands = 1,
		.operands = {2,0,0,0}
	},
	// 0x07 -----
	{},
	// 0x08 readmem
	{
		.mnemonic = "readmem",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x09 writecopy
	{
		.mnemonic = "writecopy",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x0A write
	{
		.mnemonic = "write",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x0B copycode
	{
		.mnemonic = "copycode",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x0C copystack
	{
		.mnemonic = "copystack",
		.numOperands = 2,
		.operands = {1,1,0,0}
	},
	// 0x0D -----
	{},
	// 0x0E -----
	{},
	// 0x0F -----
	{},
	// 0x10 loadbase
	{
		.mnemonic = "loadbase"
	},
	// 0x11 storebase
	{
		.mnemonic = "storebase"
	},
	// 0x12 -----
	{},
	// 0x13 -----
	{},
	// 0x14 jmp
	{
		.mnemonic = "jmp"
	},
	// 0x15 cjmp
	{
		.mnemonic = "cjmp",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x16 call
	{
		.mnemonic = "call"
	},
	// 0x17 ret
	{
		.mnemonic = "ret"
	},
	// 0x18 -----
	{},
	// 0x19 -----
	{},
	// 0x1A -----
	{},
	// 0x1B -----
	{},
	// 0x1C -----
	{},
	// 0x1D -----
	{},
	// 0x1E -----
	{},
	// 0x1F -----
	{},
	// 0x20 add
	{
		.mnemonic = "add"
	},
	// 0x21 sub
	{
		.mnemonic = "sub"
	},
	// 0x22 mul
	{
		.mnemonic = "mul"
	},
	// 0x23 div
	{
		.mnemonic = "div"
	},
	// 0x24 mod
	{
		.mnemonic = "mod"
	},
	// 0x25 and
	{
		.mnemonic = "and"
	},
	// 0x26 or
	{
		.mnemonic = "or"
	},
	// 0x27 xor
	{
		.mnemonic = "xor"
	},
	// 0x28 not
	{
		.mnemonic = "not"
	},
	// 0x29 shl
	{
		.mnemonic = "shl"
	},
	// 0x2A shr
	{
		.mnemonic = "shr"
	},
	// 0x2B sar
	{
		.mnemonic = "sar"
	},
	// 0x2C -----
	{},
	// 0x2D -----
	{},
	// 0x2E -----
	{},
	// 0x2F -----
	{},
	// 0x30 eq
	{
		.mnemonic = "eq"
	},
	// 0x31 neq
	{
		.mnemonic = "neq"
	},
	// 0x32 geq
	{
		.mnemonic = "geq"
	},
	// 0x33 leq
	{
		.mnemonic = "leq"
	},
	// 0x34 ge
	{
		.mnemonic = "ge"
	},
	// 0x35 le
	{
		.mnemonic = "le"
	},
	// 0x36 -----
	{},
	// 0x37 -----
	{},
	// 0x38
	{
		.mnemonic = "dnotzero2"
	},
	// 0x39
	{
		.mnemonic = "dnotzero"
	},
	// 0x3A
	{
		.mnemonic = "iszero"
	},
	// 0x3B
	{},
	// 0x3C
	{},
	// 0x3D
	{},
	// 0x3E
	{},
	// 0x3F
	{},
	// 0x40 ternary
	{
		.mnemonic = "ternary"
	},
	// 0x41 -----
	{},
	// 0x42 -----
	{
		.mnemonic = "muldiv"
	},
	// 0x43 -----
	{},
	// 0x44 -----
	{},
	// 0x45 -----
	{},
	// 0x46 -----
	{},
	// 0x47 -----
	{},
	// 0x48 sin
	{
		.mnemonic = "sin"
	},
	// 0x49 cos
	{
		.mnemonic = "cos"
	},
	// 0x4A-4F -----
	{},{},{},{},{},{},
	// 0x50-5F -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0x60 memcpy
	{
		.mnemonic = "memcpy"
	},
	// 0x61 memcpy
	{
		.mnemonic = "memclr"
	},
	// 0x62 memset
	{
		.mnemonic = "memset"
	},
	// 0x63 memcmp
	{
		.mnemonic = "memcmp"
	},
	// 0x64 -----
	{},
	// 0x65 -----
	{},
	// 0x66 -----
	{},
	// 0x67 -----
	{
		.mnemonic = "strreplace"
	},
	// 0x68 strlen
	{
		.mnemonic = "strlen"
	},
	// 0x69 streq
	{
		.mnemonic = "streq"
	},
	// 0x6A -----
	{
		.mnemonic = "strcpy"
	},
	// 0x6B -----
	{
		.mnemonic = "strconcat"
	},
	// 0x6C -----
	{
		.mnemonic = "getchar"
	},
	// 0x6D -----
	{
		.mnemonic = "tolower"
	},
	// 0x6E -----
	{
		.mnemonic = "quote"
	},
	// 0x6F sprintf
	{
		.mnemonic = "sprintf"
	},
	// 0x70 malloc
	{
		.mnemonic = "malloc"
	},
	// 0x71 free
	{
		.mnemonic = "free"
	},
	// 0x72 -----
	{},
	// 0x73 -----
	{},
	// 0x74 -----
	{
		.mnemonic = "memorychecking"
	},
	// 0x75 -----
	{},
	// 0x76 -----
	{},
	// 0x77 -----
	{},
	// 0x78 confirm
	{
		.mnemonic = "confirm"
	},
	// 0x79 message
	{
		.mnemonic = "message"
	},
	// 0x7A assert
	{
		.mnemonic = "assert"
	},
	// 0x7B dumpmem
	{
		.mnemonic = "dumpmem"
	},
	// 0x7C -----
	{},
	// 0x7D -----
	{},
	// 0x7E -----
	{},
	// 0x7F -----
	{},
	// 0x80 sys
	{
		.mnemonic = "sys",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x81 sys
	{
		.mnemonic = "sys",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x82 - 0x8F -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0x90 grp
	{
		.mnemonic = "grp",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x91 grp
	{
		.mnemonic = "grp",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x92 grp
	{
		.mnemonic = "grp",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0x93 - 0x9F -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xA0 snd
	{
		.mnemonic = "snd",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0xA1 - 0xAF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xB0 ext
	{
		.mnemonic = "ext",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0xB1 - 0xBF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xC0 ext
	{
		.mnemonic = "ext",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0xC1 - 0xCF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xD0 eval
	{
		.mnemonic = "eval",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0xD1 - 0xDF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xE0 ext
	{
		.mnemonic = "ext",
		.numOperands = 1,
		.operands = {1,0,0,0}
	},
	// 0xE1 - 0xEF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
	// 0xF0 - 0xFF -----
	{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},
};

char* jmpOps[] = {
	"jnz",
	"jz",
	"jg",
	"jge",
	"jle",
	"jl",
	"ja"
};

DisasmLine_t* addLine(DisasmLine_t* prev, uint32_t location, int isLabel, char* str)
{
	DisasmLine_t* newLine = (DisasmLine_t*)malloc(sizeof(DisasmLine_t));
	if(!newLine)
	{
		printf("Failed to malloc\n");
		return NULL;
	}
	if(prev)
		prev->next = newLine;

	newLine->str = NULL;
	newLine->str = (char*)malloc(strlen(str)+1);
	strcpy(newLine->str, str);

	newLine->isLabel = isLabel;
	newLine->location = location;
	newLine->next = NULL;

	return newLine;
}

Disasm_t* disassembleProgram(VMThread_t* thread, VMProgramList_t* program)
{
	if(!program || !thread) return NULL;

	uint8_t* ptr = thread->codeSpace;
	off_t end = program->location + program->size;
	off_t pos = program->location;

	DisasmLine_t* top = NULL;
	DisasmLine_t* cur = NULL;

	char str[256];
	if(program->filename)
		snprintf(&str[0], 256, "; Program: %s", program->filename);
	else
		snprintf(&str[0], 256, "; Program: ?");
	printf("%s\n", &str[0]);
	top = addLine(NULL, pos, 0, &str[0]);
	cur = top;

	while(1)
	{
		uint8_t bytecode = ptr[pos++];
		BGIOpcode_t* opcode = &basicInstructions[bytecode];
		if(opcode->mnemonic == NULL)
		{
			snprintf(&str[0], 256, "    .db %.2X", bytecode);
			cur = addLine(cur, pos, 0, &str[0]);
			continue;
		}

		// Conditional jmp is special
		if(bytecode == 0x15)
		{
			int jmpOp = ptr[pos++];
			if(jmpOp > 0x05)
				jmpOp = 0x06;
			char* opStr = jmpOps[jmpOp];
			snprintf(&str[0], 256, "    %s", opStr);
			cur = addLine(cur, pos, 0, &str[0]);
			continue;
		}

		int operands[4];
		char fmt[128];
		int sPos = 0;
		fmt[sPos++] = ' ';
		fmt[sPos++] = ' ';
		fmt[sPos++] = ' ';
		fmt[sPos++] = ' ';
		fmt[sPos++] = '%';
		fmt[sPos++] = 's';
		for(int i = 0; i < opcode->numOperands; i++)
		{
			char pres = '0';
			switch(opcode->operands[i])
			{
				case 1:
					operands[i] = ptr[pos];
					pres = '2';
					break;
				case 2:
					operands[i] = *(uint16_t*)&ptr[pos];
					pres = '4';
					break;
				case 4:
					operands[i] = *(uint32_t*)&ptr[pos];
					pres = '8';
					break;
			}
			fmt[sPos++] = ' ';
			fmt[sPos++] = '0';
			fmt[sPos++] = 'x';
			fmt[sPos++] = '%';
			fmt[sPos++] = '.';
			fmt[sPos++] = pres;
			fmt[sPos++] = 'L';
			fmt[sPos++] = 'X';
			pos += opcode->operands[i];
		}
		fmt[sPos++] = 0;
		
		snprintf(&str[0], 256, fmt, opcode->mnemonic, operands[0], operands[1], operands[2], operands[3]);
		cur = addLine(cur, pos, 0, &str[0]);

		if(pos >= end)
			break;
	}

	Disasm_t* disasm = (Disasm_t*)malloc(sizeof(Disasm_t));
	disasm->thread = thread;
	disasm->program = program;
	disasm->location = program->location;
	disasm->size = program->size;
	disasm->filename = (char*)malloc(strlen(program->filename)+1);
	strcpy(disasm->filename, program->filename);
	disasm->disasm = top;
	disasm->next = NULL;

	return disasm;
}

void freeDisassemblies(Disasm_t* disasm)
{
	while(disasm)
	{
		DisasmLine_t* line = disasm->disasm;
		while(line)
		{
			DisasmLine_t* next = line->next;
			if(line->str)
				free(line->str);
			free(line);
			line = next;
		}
		Disasm_t* n = disasm->next;
		if(disasm->filename)
			free(disasm->filename);
		free(disasm);
		disasm = n;
	}
}
