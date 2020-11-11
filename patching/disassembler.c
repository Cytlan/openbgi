//
// Disassembler
//

#include "patch.h"

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
			snprintf(&str[0], 256, "    .db 0x%.2X", bytecode);
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

		// Multi-byte instructions
		uint8_t multibyteBytecode = 0;
		BGIOpcode_t* multibyteOpcode = NULL;
		switch(bytecode)
		{
			case 0x80:
				multibyteBytecode = ptr[pos++];
				multibyteOpcode = &sys0Instructions[multibyteBytecode];
				break;
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
		if(multibyteOpcode && multibyteOpcode->mnemonic)
		{
			fmt[sPos++] = '.';
			fmt[sPos++] = '%';
			fmt[sPos++] = 's';
		}
		else if(multibyteOpcode && !multibyteOpcode->mnemonic)
		{
			fmt[sPos++] = '.';
			fmt[sPos++] = '0';
			fmt[sPos++] = 'x';
			fmt[sPos++] = '%';
			fmt[sPos++] = '.';
			fmt[sPos++] = '2';
			fmt[sPos++] = 'X';
		}

		BGIOpcode_t* parseOpcode = opcode;
		if(multibyteOpcode)
			parseOpcode = multibyteOpcode;
		for(int i = 0; i < parseOpcode->numOperands; i++)
		{
			char pres = '0';
			switch(parseOpcode->operands[i])
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
			pos += parseOpcode->operands[i];
		}
		fmt[sPos++] = 0;
		
		if(multibyteOpcode && multibyteOpcode->mnemonic)
			snprintf(&str[0], 256, fmt, opcode->mnemonic, multibyteOpcode->mnemonic, operands[0], operands[1], operands[2], operands[3]);
		else if(multibyteOpcode && !multibyteOpcode->mnemonic)
			snprintf(&str[0], 256, fmt, opcode->mnemonic, multibyteBytecode, operands[0], operands[1], operands[2], operands[3]);
		else
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
