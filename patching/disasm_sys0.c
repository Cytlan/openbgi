//
// System instructions
//

#include "patch.h"

BGIOpcode_t sys0Instructions[256] =
{
	// 0x00 ----
	{
		.mnemonic = "srand",
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x01 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x02 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x03 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x04 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x05 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x06 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x07 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x08 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x09 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x0F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x10 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x11 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x12 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x13 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x14 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x15 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x16 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x17 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x18 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x19 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x1F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x20 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x21 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x22 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x23 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x24 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x25 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x26 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x27 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x28 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x29 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x2F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x30 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x31 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x32 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x33 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x34 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x35 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x36 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x37 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x38 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x39 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x3F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x40 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x41 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x42 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x43 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x44 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x45 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x46 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x47 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x48 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x49 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x4F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x50 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x51 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x52 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x53 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x54 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x55 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x56 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x57 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x58 ----
	{
		.mnemonic = "settimer?",
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x59 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x5F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x60 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x61 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x62 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x63 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x64 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x65 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x66 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x67 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x68 ----
	{
		.mnemonic = "capturecloseevent?", // PTR_DAT_0048e39c = &DAT_004c058c
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x69 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x6F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x70 ----
	{
		.mnemonic = "alloc0x70", // Allocates memory into DAT_004bd43c
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x71 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x72 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x73 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x74 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x75 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x76 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x77 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x78 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x79 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x7F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x80 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x81 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x82 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x83 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x84 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x85 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x86 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x87 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x88 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x89 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x8F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x90 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x91 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x92 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x93 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x94 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x95 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x96 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x97 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x98 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x99 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9A ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9B ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9C ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9D -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9E -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0x9F -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xA9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAD -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xAF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xB9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBD -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xBF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xC9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCD -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xCF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xD9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDD -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xDF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xE9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xEA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xEB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xEC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xED -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xEE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xEF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF0 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF1 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF2 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF3 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF4 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF5 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF6 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF7 -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF8 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xF9 ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFA ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFB ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFC ----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFD -----
	{
		.mnemonic = "islauncher",
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFE -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	},
	// 0xFF -----
	{
		.mnemonic = NULL,
		.numOperands = 0,
		.operands = {0,0,0,0}
	}
};
