//
// Basic instructions
//

#include "patch.h"

BGIOpcode_t basicInstructions[256] =
{
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
