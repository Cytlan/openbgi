#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "opcodes.h"
#include "opcodes_sys0.h"
#include "opcodes_grp0.h"
#include "opcodes_grp1.h"
#include "opcodes_grp2.h"
#include "opcodes_snd0.h"
#include "opcodes_ext0.h"
#include "opcodes_ext1.h"
#include "thread.h"

char* OpcodesMnemonics[256] = {
	/* 0x00   0 */ "Push8",
	/* 0x01   1 */ "Push16",
	/* 0x02   2 */ "Push32",
	/* 0x03   3 */ "Unknown",
	/* 0x04   4 */ "BaseOffset",
	/* 0x05   5 */ "CodeAddr",
	/* 0x06   6 */ "CodeOffset",
	/* 0x07   7 */ "Unknown",
	/* 0x08   8 */ "ReadMem",
	/* 0x09   9 */ "WriteCopy",
	/* 0x0A  10 */ "WriteMem",
	/* 0x0B  11 */ "Unknown",
	/* 0x0C  12 */ "CopyStack",
	/* 0x0D  13 */ "Unknown",
	/* 0x0E  14 */ "Unknown",
	/* 0x0F  15 */ "Unknown",
	/* 0x10  16 */ "PushBasePointer",
	/* 0x11  17 */ "PopBasePointer",
	/* 0x12  18 */ "Unknown",
	/* 0x13  19 */ "Unknown",
	/* 0x14  20 */ "Jump",
	/* 0x15  21 */ "CondJump",
	/* 0x16  22 */ "Call",
	/* 0x17  23 */ "Ret",
	/* 0x18  24 */ "Unknown",
	/* 0x19  25 */ "Unknown",
	/* 0x1A  26 */ "Unknown",
	/* 0x1B  27 */ "Unknown",
	/* 0x1C  28 */ "Unknown",
	/* 0x1D  29 */ "Unknown",
	/* 0x1E  30 */ "Unknown",
	/* 0x1F  31 */ "Unknown",
	/* 0x20  32 */ "Add",
	/* 0x21  33 */ "Sub",
	/* 0x22  34 */ "Mul",
	/* 0x23  35 */ "Div",
	/* 0x24  36 */ "Mod",
	/* 0x25  37 */ "And",
	/* 0x26  38 */ "Or",
	/* 0x27  39 */ "Xor",
	/* 0x28  40 */ "Not",
	/* 0x29  41 */ "SHL",
	/* 0x2A  42 */ "SHR",
	/* 0x2B  43 */ "SAR",
	/* 0x2C  44 */ "Unknown",
	/* 0x2D  45 */ "Unknown",
	/* 0x2E  46 */ "Unknown",
	/* 0x2F  47 */ "Unknown",
	/* 0x30  48 */ "Eq",
	/* 0x31  49 */ "Neq",
	/* 0x32  50 */ "Leq",
	/* 0x33  51 */ "Geq",
	/* 0x34  52 */ "Ge",
	/* 0x35  53 */ "Le",
	/* 0x36  54 */ "Unknown",
	/* 0x37  55 */ "Unknown",
	/* 0x38  56 */ "Unknown",
	/* 0x39  57 */ "Unknown",
	/* 0x3A  58 */ "IsZero",
	/* 0x3B  59 */ "Unknown",
	/* 0x3C  60 */ "Unknown",
	/* 0x3D  61 */ "Unknown",
	/* 0x3E  62 */ "Unknown",
	/* 0x3F  63 */ "Unknown",
	/* 0x40  64 */ "Ternary",
	/* 0x41  65 */ "Unknown",
	/* 0x42  66 */ "Unknown",
	/* 0x43  67 */ "Unknown",
	/* 0x44  68 */ "Unknown",
	/* 0x45  69 */ "Unknown",
	/* 0x46  70 */ "Unknown",
	/* 0x47  71 */ "Unknown",
	/* 0x48  72 */ "Unknown",
	/* 0x49  73 */ "Unknown",
	/* 0x4A  74 */ "Unknown",
	/* 0x4B  75 */ "Unknown",
	/* 0x4C  76 */ "Unknown",
	/* 0x4D  77 */ "Unknown",
	/* 0x4E  78 */ "Unknown",
	/* 0x4F  79 */ "Unknown",
	/* 0x50  80 */ "Unknown",
	/* 0x51  81 */ "Unknown",
	/* 0x52  82 */ "Unknown",
	/* 0x53  83 */ "Unknown",
	/* 0x54  84 */ "Unknown",
	/* 0x55  85 */ "Unknown",
	/* 0x56  86 */ "Unknown",
	/* 0x57  87 */ "Unknown",
	/* 0x58  88 */ "Unknown",
	/* 0x59  89 */ "Unknown",
	/* 0x5A  90 */ "Unknown",
	/* 0x5B  91 */ "Unknown",
	/* 0x5C  92 */ "Unknown",
	/* 0x5D  93 */ "Unknown",
	/* 0x5E  94 */ "Unknown",
	/* 0x5F  95 */ "Unknown",
	/* 0x60  96 */ "Memcpy",
	/* 0x61  97 */ "Unknown",
	/* 0x62  98 */ "Unknown",
	/* 0x63  99 */ "Unknown",
	/* 0x64 100 */ "Unknown",
	/* 0x65 101 */ "Unknown",
	/* 0x66 102 */ "Unknown",
	/* 0x67 103 */ "Unknown",
	/* 0x68 104 */ "Unknown",
	/* 0x69 105 */ "Unknown",
	/* 0x6A 106 */ "Unknown",
	/* 0x6B 107 */ "Unknown",
	/* 0x6C 108 */ "Unknown",
	/* 0x6D 109 */ "Unknown",
	/* 0x6E 110 */ "Unknown",
	/* 0x6F 111 */ "Sprintf",
	/* 0x70 112 */ "Unknown",
	/* 0x71 113 */ "Unknown",
	/* 0x72 114 */ "Unknown",
	/* 0x73 115 */ "Unknown",
	/* 0x74 116 */ "Unknown",
	/* 0x75 117 */ "Unknown",
	/* 0x76 118 */ "Unknown",
	/* 0x77 119 */ "Unknown",
	/* 0x78 120 */ "Unknown",
	/* 0x79 121 */ "Unknown",
	/* 0x7A 122 */ "Unknown",
	/* 0x7B 123 */ "Unknown",
	/* 0x7C 124 */ "Unknown",
	/* 0x7D 125 */ "Unknown",
	/* 0x7E 126 */ "Unknown",
	/* 0x7F 127 */ "Unknown",
	/* 0x80 128 */ "Sys0",
	/* 0x81 129 */ "Unknown",
	/* 0x82 130 */ "Unknown",
	/* 0x83 131 */ "Unknown",
	/* 0x84 132 */ "Unknown",
	/* 0x85 133 */ "Unknown",
	/* 0x86 134 */ "Unknown",
	/* 0x87 135 */ "Unknown",
	/* 0x88 136 */ "Unknown",
	/* 0x89 137 */ "Unknown",
	/* 0x8A 138 */ "Unknown",
	/* 0x8B 139 */ "Unknown",
	/* 0x8C 140 */ "Unknown",
	/* 0x8D 141 */ "Unknown",
	/* 0x8E 142 */ "Unknown",
	/* 0x8F 143 */ "Unknown",
	/* 0x90 144 */ "Grp0",
	/* 0x91 145 */ "Grp1",
	/* 0x92 146 */ "Grp2",
	/* 0x93 147 */ "Unknown",
	/* 0x94 148 */ "Unknown",
	/* 0x95 149 */ "Unknown",
	/* 0x96 150 */ "Unknown",
	/* 0x97 151 */ "Unknown",
	/* 0x98 152 */ "Unknown",
	/* 0x99 153 */ "Unknown",
	/* 0x9A 154 */ "Unknown",
	/* 0x9B 155 */ "Unknown",
	/* 0x9C 156 */ "Unknown",
	/* 0x9D 157 */ "Unknown",
	/* 0x9E 158 */ "Unknown",
	/* 0x9F 159 */ "Unknown",
	/* 0xA0 160 */ "Snd0",
	/* 0xA1 161 */ "Unknown",
	/* 0xA2 162 */ "Unknown",
	/* 0xA3 163 */ "Unknown",
	/* 0xA4 164 */ "Unknown",
	/* 0xA5 165 */ "Unknown",
	/* 0xA6 166 */ "Unknown",
	/* 0xA7 167 */ "Unknown",
	/* 0xA8 168 */ "Unknown",
	/* 0xA9 169 */ "Unknown",
	/* 0xAA 170 */ "Unknown",
	/* 0xAB 171 */ "Unknown",
	/* 0xAC 172 */ "Unknown",
	/* 0xAD 173 */ "Unknown",
	/* 0xAE 174 */ "Unknown",
	/* 0xAF 175 */ "Unknown",
	/* 0xB0 176 */ "Ext0",
	/* 0xB1 177 */ "Unknown",
	/* 0xB2 178 */ "Unknown",
	/* 0xB3 179 */ "Unknown",
	/* 0xB4 180 */ "Unknown",
	/* 0xB5 181 */ "Unknown",
	/* 0xB6 182 */ "Unknown",
	/* 0xB7 183 */ "Unknown",
	/* 0xB8 184 */ "Unknown",
	/* 0xB9 185 */ "Unknown",
	/* 0xBA 186 */ "Unknown",
	/* 0xBB 187 */ "Unknown",
	/* 0xBC 188 */ "Unknown",
	/* 0xBD 189 */ "Unknown",
	/* 0xBE 190 */ "Unknown",
	/* 0xBF 191 */ "Unknown",
	/* 0xC0 192 */ "Ext1",
	/* 0xC1 193 */ "Unknown",
	/* 0xC2 194 */ "Unknown",
	/* 0xC3 195 */ "Unknown",
	/* 0xC4 196 */ "Unknown",
	/* 0xC5 197 */ "Unknown",
	/* 0xC6 198 */ "Unknown",
	/* 0xC7 199 */ "Unknown",
	/* 0xC8 200 */ "Unknown",
	/* 0xC9 201 */ "Unknown",
	/* 0xCA 202 */ "Unknown",
	/* 0xCB 203 */ "Unknown",
	/* 0xCC 204 */ "Unknown",
	/* 0xCD 205 */ "Unknown",
	/* 0xCE 206 */ "Unknown",
	/* 0xCF 207 */ "Unknown",
	/* 0xD0 208 */ "Unknown",
	/* 0xD1 209 */ "Unknown",
	/* 0xD2 210 */ "Unknown",
	/* 0xD3 211 */ "Unknown",
	/* 0xD4 212 */ "Unknown",
	/* 0xD5 213 */ "Unknown",
	/* 0xD6 214 */ "Unknown",
	/* 0xD7 215 */ "Unknown",
	/* 0xD8 216 */ "Unknown",
	/* 0xD9 217 */ "Unknown",
	/* 0xDA 218 */ "Unknown",
	/* 0xDB 219 */ "Unknown",
	/* 0xDC 220 */ "Unknown",
	/* 0xDD 221 */ "Unknown",
	/* 0xDE 222 */ "Unknown",
	/* 0xDF 223 */ "Unknown",
	/* 0xE0 224 */ "Unknown",
	/* 0xE1 225 */ "Unknown",
	/* 0xE2 226 */ "Unknown",
	/* 0xE3 227 */ "Unknown",
	/* 0xE4 228 */ "Unknown",
	/* 0xE5 229 */ "Unknown",
	/* 0xE6 230 */ "Unknown",
	/* 0xE7 231 */ "Unknown",
	/* 0xE8 232 */ "Unknown",
	/* 0xE9 233 */ "Unknown",
	/* 0xEA 234 */ "Unknown",
	/* 0xEB 235 */ "Unknown",
	/* 0xEC 236 */ "Unknown",
	/* 0xED 237 */ "Unknown",
	/* 0xEE 238 */ "Unknown",
	/* 0xEF 239 */ "Unknown",
	/* 0xF0 240 */ "Unknown",
	/* 0xF1 241 */ "Unknown",
	/* 0xF2 242 */ "Unknown",
	/* 0xF3 243 */ "Unknown",
	/* 0xF4 244 */ "Unknown",
	/* 0xF5 245 */ "Unknown",
	/* 0xF6 246 */ "Unknown",
	/* 0xF7 247 */ "Unknown",
	/* 0xF8 248 */ "Unknown",
	/* 0xF9 249 */ "Unknown",
	/* 0xFA 250 */ "Unknown",
	/* 0xFB 251 */ "Unknown",
	/* 0xFC 252 */ "Unknown",
	/* 0xFD 253 */ "Unknown",
	/* 0xFE 254 */ "Unknown",
	/* 0xFF 255 */ "Unknown",
};

OpcodePtr_t Opcodes[256] = {
	/* 0x00   0 */ Opcode_Push8,
	/* 0x01   1 */ Opcode_Push16,
	/* 0x02   2 */ Opcode_Push32,
	/* 0x03   3 */ 0,
	/* 0x04   4 */ Opcode_BaseOffset,
	/* 0x05   5 */ Opcode_CodeAddr,
	/* 0x06   6 */ Opcode_CodeOffset,
	/* 0x07   7 */ 0,
	/* 0x08   8 */ Opcode_ReadMem,
	/* 0x09   9 */ Opcode_WriteCopy,
	/* 0x0A  10 */ Opcode_WriteMem,
	/* 0x0B  11 */ 0,
	/* 0x0C  12 */ Opcode_CopyStack,
	/* 0x0D  13 */ 0,
	/* 0x0E  14 */ 0,
	/* 0x0F  15 */ 0,
	/* 0x10  16 */ Opcode_PushBasePointer,
	/* 0x11  17 */ Opcode_PopBasePointer,
	/* 0x12  18 */ 0,
	/* 0x13  19 */ 0,
	/* 0x14  20 */ Opcode_Jump,
	/* 0x15  21 */ Opcode_CondJump,
	/* 0x16  22 */ Opcode_Call,
	/* 0x17  23 */ Opcode_Ret,
	/* 0x18  24 */ 0,
	/* 0x19  25 */ 0,
	/* 0x1A  26 */ 0,
	/* 0x1B  27 */ 0,
	/* 0x1C  28 */ 0,
	/* 0x1D  29 */ 0,
	/* 0x1E  30 */ 0,
	/* 0x1F  31 */ 0,
	/* 0x20  32 */ Opcode_Add,
	/* 0x21  33 */ Opcode_Sub,
	/* 0x22  34 */ Opcode_Mul,
	/* 0x23  35 */ Opcode_Div,
	/* 0x24  36 */ Opcode_Mod,
	/* 0x25  37 */ Opcode_And,
	/* 0x26  38 */ Opcode_Or,
	/* 0x27  39 */ Opcode_Xor,
	/* 0x28  40 */ Opcode_Not,
	/* 0x29  41 */ Opcode_SHL,
	/* 0x2A  42 */ Opcode_SHR,
	/* 0x2B  43 */ Opcode_SAR,
	/* 0x2C  44 */ 0,
	/* 0x2D  45 */ 0,
	/* 0x2E  46 */ 0,
	/* 0x2F  47 */ 0,
	/* 0x30  48 */ Opcode_Eq,
	/* 0x31  49 */ Opcode_Neq,
	/* 0x32  50 */ Opcode_Leq,
	/* 0x33  51 */ Opcode_Geq,
	/* 0x34  52 */ Opcode_Ge,
	/* 0x35  53 */ Opcode_Le,
	/* 0x36  54 */ 0,
	/* 0x37  55 */ 0,
	/* 0x38  56 */ 0,
	/* 0x39  57 */ 0,
	/* 0x3A  58 */ Opcode_IsZero,
	/* 0x3B  59 */ 0,
	/* 0x3C  60 */ 0,
	/* 0x3D  61 */ 0,
	/* 0x3E  62 */ 0,
	/* 0x3F  63 */ 0,
	/* 0x40  64 */ Opcode_Ternary,
	/* 0x41  65 */ 0,
	/* 0x42  66 */ 0,
	/* 0x43  67 */ 0,
	/* 0x44  68 */ 0,
	/* 0x45  69 */ 0,
	/* 0x46  70 */ 0,
	/* 0x47  71 */ 0,
	/* 0x48  72 */ 0,
	/* 0x49  73 */ 0,
	/* 0x4A  74 */ 0,
	/* 0x4B  75 */ 0,
	/* 0x4C  76 */ 0,
	/* 0x4D  77 */ 0,
	/* 0x4E  78 */ 0,
	/* 0x4F  79 */ 0,
	/* 0x50  80 */ 0,
	/* 0x51  81 */ 0,
	/* 0x52  82 */ 0,
	/* 0x53  83 */ 0,
	/* 0x54  84 */ 0,
	/* 0x55  85 */ 0,
	/* 0x56  86 */ 0,
	/* 0x57  87 */ 0,
	/* 0x58  88 */ 0,
	/* 0x59  89 */ 0,
	/* 0x5A  90 */ 0,
	/* 0x5B  91 */ 0,
	/* 0x5C  92 */ 0,
	/* 0x5D  93 */ 0,
	/* 0x5E  94 */ 0,
	/* 0x5F  95 */ 0,
	/* 0x60  96 */ Opcode_Memcpy,
	/* 0x61  97 */ 0,
	/* 0x62  98 */ 0,
	/* 0x63  99 */ 0,
	/* 0x64 100 */ 0,
	/* 0x65 101 */ 0,
	/* 0x66 102 */ 0,
	/* 0x67 103 */ 0,
	/* 0x68 104 */ 0,
	/* 0x69 105 */ 0,
	/* 0x6A 106 */ 0,
	/* 0x6B 107 */ 0,
	/* 0x6C 108 */ 0,
	/* 0x6D 109 */ 0,
	/* 0x6E 110 */ 0,
	/* 0x6F 111 */ Opcode_Sprintf,
	/* 0x70 112 */ 0,
	/* 0x71 113 */ 0,
	/* 0x72 114 */ 0,
	/* 0x73 115 */ 0,
	/* 0x74 116 */ 0,
	/* 0x75 117 */ 0,
	/* 0x76 118 */ 0,
	/* 0x77 119 */ 0,
	/* 0x78 120 */ 0,
	/* 0x79 121 */ 0,
	/* 0x7A 122 */ 0,
	/* 0x7B 123 */ 0,
	/* 0x7C 124 */ 0,
	/* 0x7D 125 */ 0,
	/* 0x7E 126 */ 0,
	/* 0x7F 127 */ 0,
	/* 0x80 128 */ Opcode_Sys0,
	/* 0x81 129 */ 0,
	/* 0x82 130 */ 0,
	/* 0x83 131 */ 0,
	/* 0x84 132 */ 0,
	/* 0x85 133 */ 0,
	/* 0x86 134 */ 0,
	/* 0x87 135 */ 0,
	/* 0x88 136 */ 0,
	/* 0x89 137 */ 0,
	/* 0x8A 138 */ 0,
	/* 0x8B 139 */ 0,
	/* 0x8C 140 */ 0,
	/* 0x8D 141 */ 0,
	/* 0x8E 142 */ 0,
	/* 0x8F 143 */ 0,
	/* 0x90 144 */ Opcode_Grp0,
	/* 0x91 145 */ Opcode_Grp1,
	/* 0x92 146 */ Opcode_Grp2,
	/* 0x93 147 */ 0,
	/* 0x94 148 */ 0,
	/* 0x95 149 */ 0,
	/* 0x96 150 */ 0,
	/* 0x97 151 */ 0,
	/* 0x98 152 */ 0,
	/* 0x99 153 */ 0,
	/* 0x9A 154 */ 0,
	/* 0x9B 155 */ 0,
	/* 0x9C 156 */ 0,
	/* 0x9D 157 */ 0,
	/* 0x9E 158 */ 0,
	/* 0x9F 159 */ 0,
	/* 0xA0 160 */ Opcode_Snd0,
	/* 0xA1 161 */ 0,
	/* 0xA2 162 */ 0,
	/* 0xA3 163 */ 0,
	/* 0xA4 164 */ 0,
	/* 0xA5 165 */ 0,
	/* 0xA6 166 */ 0,
	/* 0xA7 167 */ 0,
	/* 0xA8 168 */ 0,
	/* 0xA9 169 */ 0,
	/* 0xAA 170 */ 0,
	/* 0xAB 171 */ 0,
	/* 0xAC 172 */ 0,
	/* 0xAD 173 */ 0,
	/* 0xAE 174 */ 0,
	/* 0xAF 175 */ 0,
	/* 0xB0 176 */ Opcode_Ext0,
	/* 0xB1 177 */ 0,
	/* 0xB2 178 */ 0,
	/* 0xB3 179 */ 0,
	/* 0xB4 180 */ 0,
	/* 0xB5 181 */ 0,
	/* 0xB6 182 */ 0,
	/* 0xB7 183 */ 0,
	/* 0xB8 184 */ 0,
	/* 0xB9 185 */ 0,
	/* 0xBA 186 */ 0,
	/* 0xBB 187 */ 0,
	/* 0xBC 188 */ 0,
	/* 0xBD 189 */ 0,
	/* 0xBE 190 */ 0,
	/* 0xBF 191 */ 0,
	/* 0xC0 192 */ Opcode_Ext1,
	/* 0xC1 193 */ 0,
	/* 0xC2 194 */ 0,
	/* 0xC3 195 */ 0,
	/* 0xC4 196 */ 0,
	/* 0xC5 197 */ 0,
	/* 0xC6 198 */ 0,
	/* 0xC7 199 */ 0,
	/* 0xC8 200 */ 0,
	/* 0xC9 201 */ 0,
	/* 0xCA 202 */ 0,
	/* 0xCB 203 */ 0,
	/* 0xCC 204 */ 0,
	/* 0xCD 205 */ 0,
	/* 0xCE 206 */ 0,
	/* 0xCF 207 */ 0,
	/* 0xD0 208 */ 0,
	/* 0xD1 209 */ 0,
	/* 0xD2 210 */ 0,
	/* 0xD3 211 */ 0,
	/* 0xD4 212 */ 0,
	/* 0xD5 213 */ 0,
	/* 0xD6 214 */ 0,
	/* 0xD7 215 */ 0,
	/* 0xD8 216 */ 0,
	/* 0xD9 217 */ 0,
	/* 0xDA 218 */ 0,
	/* 0xDB 219 */ 0,
	/* 0xDC 220 */ 0,
	/* 0xDD 221 */ 0,
	/* 0xDE 222 */ 0,
	/* 0xDF 223 */ 0,
	/* 0xE0 224 */ 0,
	/* 0xE1 225 */ 0,
	/* 0xE2 226 */ 0,
	/* 0xE3 227 */ 0,
	/* 0xE4 228 */ 0,
	/* 0xE5 229 */ 0,
	/* 0xE6 230 */ 0,
	/* 0xE7 231 */ 0,
	/* 0xE8 232 */ 0,
	/* 0xE9 233 */ 0,
	/* 0xEA 234 */ 0,
	/* 0xEB 235 */ 0,
	/* 0xEC 236 */ 0,
	/* 0xED 237 */ 0,
	/* 0xEE 238 */ 0,
	/* 0xEF 239 */ 0,
	/* 0xF0 240 */ 0,
	/* 0xF1 241 */ 0,
	/* 0xF2 242 */ 0,
	/* 0xF3 243 */ 0,
	/* 0xF4 244 */ 0,
	/* 0xF5 245 */ 0,
	/* 0xF6 246 */ 0,
	/* 0xF7 247 */ 0,
	/* 0xF8 248 */ 0,
	/* 0xF9 249 */ 0,
	/* 0xFA 250 */ 0,
	/* 0xFB 251 */ 0,
	/* 0xFC 252 */ 0,
	/* 0xFD 253 */ 0,
	/* 0xFE 254 */ 0,
	/* 0xFF 255 */ 0,
};

uint32_t Opcode_Push8(Thread_t* thread)
{
	uint8_t data = Thread_ReadCode8(thread);
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_Push16(Thread_t* thread)
{
	uint16_t data = Thread_ReadCode16(thread);
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_Push32(Thread_t* thread)
{
	uint32_t data = Thread_ReadCode32(thread);
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_CodeOffset(Thread_t* thread)
{
	uint16_t data = Thread_ReadCode16(thread);
	uint32_t ip = Thread_GetInstructionPointer(thread);
	data += ip;
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_CodeAddr(Thread_t* thread)
{
	uint16_t offset = Thread_ReadCode16(thread);
	uint32_t ip = Thread_GetInstructionPointer(thread);
	Thread_PushStack(thread, (ip + offset) | 0x11000000);
	return 0;
}

uint32_t Opcode_Jump(Thread_t* thread)
{
	uint32_t dest = Thread_PopStack(thread);
	Thread_SetInstructionPointer(thread, dest);
	return 0;
}

uint32_t Opcode_CondJump(Thread_t* thread)
{
	uint32_t dest = Thread_PopStack(thread);
	uint32_t value = Thread_PopStack(thread);
	uint8_t op = Thread_ReadCode8(thread);
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
	if(res == true)
	{
		// TODO: Bounds checking
		printf("[Thread %d]: %sJump taken\n", thread->threadId, TLevel[thread->level]);
		Thread_SetInstructionPointer(thread, dest);
	}
	else
		printf("[Thread %d]: %sJump not taken\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Call(Thread_t* thread)
{
	uint32_t ptr = Thread_GetBasePointer(thread);
	uint32_t size = Thread_GetLocalMemSize(thread);
	if(size <= ptr + 4)
	{
		// Out of memory (Tho the engine says スタック領域が不足しています)
		printf("Error: Out of memory\n");
		//haltExecution(thread);
		return 0;
	}

	uint32_t returnPtr = Thread_GetInstructionPointer(thread) + 1;
	Thread_WriteReturnAddr(thread, returnPtr);

	return Opcode_Jump(thread);
}

uint32_t Opcode_Ret(Thread_t* thread)
{
	uint32_t memPtr = Thread_GetBasePointer(thread);
	if(memPtr != 0)
	{
		Thread_SetInstructionPointer(thread, Thread_ReadReturnAddr(thread));
		return 0;
	}
	return 4;
}

uint32_t Opcode_PushBasePointer(Thread_t* thread)
{
	uint32_t data = Thread_GetBasePointer(thread);
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_PopBasePointer(Thread_t* thread)
{
	uint32_t newPointer = Thread_PopStack(thread);
	Thread_SetBasePointer(thread, newPointer);
	return 0;
}

uint32_t Opcode_Add(Thread_t* thread)
{
	uint32_t rhs = Thread_PopStack(thread);
	uint32_t lhs = Thread_PopStack(thread);
	Thread_PushStack(thread, lhs + rhs);
	return 0;
}

uint32_t Opcode_Sys0(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sSys0 Executing opcode Sys0.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesSys0Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesSys0[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0x80%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesSys0[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Grp0(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sGrp0 Executing opcode Grp0.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesGrp0Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesGrp0[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0x90%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesGrp0[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Grp1(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sGrp1 Executing opcode Grp1.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesGrp1Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesGrp1[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0x91%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesGrp1[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Grp2(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sGrp2 Executing opcode Grp2.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesGrp2Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesGrp2[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0x92%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesGrp2[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Snd0(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sSnd0 Executing opcode Snd0.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesSnd0Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesSnd0[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0xA0%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesSnd0[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Ext0(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sExt0 Executing opcode Ext0.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesExt0Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesExt0[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0xB0%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesExt0[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_Ext1(Thread_t* thread)
{
	uint8_t opcode = Thread_ReadImm8(thread);
	printf("[Thread %d]: %sExt1 Executing opcode Ext1.%s (0x%.2X / %d)\n", thread->threadId, TLevel[thread->level], OpcodesExt1Mnemonics[opcode], opcode, opcode);
	thread->level++;
	if(OpcodesExt1[opcode] == NULL)
	{
		thread->level--;
		printf("[Thread %d]: %sError: opcode 0xC0%.2X (%d) not implemented\n", thread->threadId, TLevel[thread->level], opcode, opcode);
		return 0xFFFFFFFF;
	}
	uint32_t res = OpcodesExt1[opcode](thread);
	thread->level--;
	return res;
}

uint32_t Opcode_BaseOffset(Thread_t* thread)
{
	uint16_t offset = Thread_ReadCode16(thread);
	uint32_t base = Thread_GetBasePointer(thread);
	uint32_t relativeOffset = base - offset;
	uint32_t taggedValue    = relativeOffset | 0x12000000;
	Thread_PushStack(thread, taggedValue);
	return 0;
}

uint32_t Opcode_CopyStack(Thread_t* thread)
{
	uint8_t size = Thread_ReadCode8(thread);
	uint8_t count = Thread_ReadCode8(thread);

	// Pop all the values
	uint32_t values[256];
	for(int i = 0; i < count; i++)
		values[i] = Thread_PopStack(thread);

	// Write all the values to memory in reverse order
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	for(int i = count-1; i >= 0; i--)
	{
		Thread_WriteIntToMemory(thread, ptr, size, values[i]);
		ptr += 1 << size;
	}

	return 0;
}

uint32_t Opcode_ReadMem(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	uint8_t size = Thread_ReadCode8(thread);
	uint32_t value = 0;
	switch(size)
	{
		case 0:
			value = *(uint8_t*)ptr;
			printf("[Thread %d]: %sRead 0x%.2X from memory\n", thread->threadId, TLevel[thread->level], value);
			break;
		case 1:
			value = *(uint16_t*)ptr;
			printf("[Thread %d]: %sRead 0x%.4X from memory\n", thread->threadId, TLevel[thread->level], value);
			break;
		case 2:
			value = *(uint32_t*)ptr;
			printf("[Thread %d]: %sRead 0x%.8X from memory\n", thread->threadId, TLevel[thread->level], value);
			break;
		default:
			printf("[Thread %d]: %sInvalid size %d of value 0x%.8X when reading from memory\n", thread->threadId, TLevel[thread->level], size, value);
			return 1;
	}
	Thread_PushStack(thread, value);
	return 0;
}

uint32_t Opcode_WriteCopy(Thread_t* thread)
{
	uint32_t data = Thread_PopStack(thread);
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	uint8_t size = Thread_ReadCode8(thread);
	
	// Write to mem
	Thread_WriteIntToMemory(thread, ptr, size, data);

	// Push data back onto the stack
	Thread_PushStack(thread, data);

	return 0;
}

uint32_t Opcode_WriteMem(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	uint32_t value = Thread_PopStack(thread);
	uint8_t size = Thread_ReadCode8(thread);
	Thread_WriteIntToMemory(thread, ptr, size, value);
	return 0;
}

uint32_t Opcode_Ternary(Thread_t* thread)
{
	uint32_t falseVal = Thread_PopStack(thread);
	uint32_t trueVal = Thread_PopStack(thread);
	uint32_t compare = Thread_PopStack(thread);
	
	if(compare)
		Thread_PushStack(thread, trueVal);
	else
		Thread_PushStack(thread, falseVal);

	return 0;
}

uint32_t Opcode_SHL(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left << (right & 0x1F));
	return 0;
}

uint32_t Opcode_Mul(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left * right);
	return 0;
}

uint32_t Opcode_Div(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	if(right == 0)
		Thread_PushStack(thread, -1);
	else
		Thread_PushStack(thread, left / right);
	return 0;
}

uint32_t Opcode_Sub(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left - right);
	return 0;
}

uint32_t Opcode_SAR(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left >> (right & 0x1F));
	return 0;
}

uint32_t Opcode_Eq(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left == right);
	return 0;
}

uint32_t Opcode_Neq(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left != right);
	return 0;
}

uint32_t Opcode_Leq(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left <= right);
	return 0;
}

uint32_t Opcode_Geq(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left >= right);
	return 0;
}

uint32_t Opcode_Ge(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left > right);
	return 0;
}

uint32_t Opcode_Le(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left < right);
	return 0;
}

uint32_t Opcode_SHR(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left >> (right & 0x1F));
	return 0;
}

uint32_t Opcode_Mod(Thread_t* thread)
{
	int32_t right = Thread_PopStack(thread);
	int32_t left = Thread_PopStack(thread);
	if(right == 0)
		Thread_PushStack(thread, -1);
	else
		Thread_PushStack(thread, left % right);
	return 0;
}

uint32_t Opcode_And(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left & right);
	return 0;
}

uint32_t Opcode_Or(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left | right);
	return 0;
}

uint32_t Opcode_Xor(Thread_t* thread)
{
	uint32_t right = Thread_PopStack(thread);
	uint32_t left = Thread_PopStack(thread);
	Thread_PushStack(thread, left ^ right);
	return 0;
}

uint32_t Opcode_Not(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Thread_PushStack(thread, ~value);
	return 0;
}

uint32_t Opcode_Sprintf(Thread_t* thread)
{
	uint8_t* fmt = Thread_PopAndResolveAddress(thread);
	uint8_t* dst = Thread_PopAndResolveAddress(thread);
	Thread_Sprintf(thread, dst, fmt);
	return 0;
}

uint32_t Opcode_Memcpy(Thread_t* thread)
{
	uint32_t size = Thread_PopStack(thread);
	uint8_t* src = Thread_PopAndResolveAddress(thread);
	uint8_t* dst = Thread_PopAndResolveAddress(thread);
	memcpy(dst, src, size);
	return 0;
}

uint32_t Opcode_IsZero(Thread_t* thread)
{
	Thread_PushStack(thread, Thread_PopStack(thread) == 0);
	return 0;
}
