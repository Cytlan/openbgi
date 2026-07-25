#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "engine.h"
#include "opcodes.h"
#include "opcodes_sys0.h"
#include "thread.h"

char* OpcodesSys0Mnemonics[256] = {
	/* 0x00   0 */ "Srand",
	/* 0x01   1 */ "Unknown_1",
	/* 0x02   2 */ "Unknown_2",
	/* 0x03   3 */ "--Unknown--",
	/* 0x04   4 */ "GetSysTime",
	/* 0x05   5 */ "--Unknown--",
	/* 0x06   6 */ "--Unknown--",
	/* 0x07   7 */ "--Unknown--",
	/* 0x08   8 */ "Unknown_8",
	/* 0x09   9 */ "--Unknown--",
	/* 0x0A  10 */ "Unknown_10",
	/* 0x0B  11 */ "Unknown_11",
	/* 0x0C  12 */ "Unknown_12",
	/* 0x0D  13 */ "Unknown_13",
	/* 0x0E  14 */ "Unknown_14",
	/* 0x0F  15 */ "Unknown_15",
	/* 0x10  16 */ "Unknown_16",
	/* 0x11  17 */ "Unknown_17",
	/* 0x12  18 */ "Unknown_18",
	/* 0x13  19 */ "Unknown_19",
	/* 0x14  20 */ "Unknown_20",
	/* 0x15  21 */ "Unknown_21",
	/* 0x16  22 */ "Unknown_22",
	/* 0x17  23 */ "Unknown_23",
	/* 0x18  24 */ "Unknown_24",
	/* 0x19  25 */ "Unknown_25",
	/* 0x1A  26 */ "Unknown_26",
	/* 0x1B  27 */ "Unknown_0x1B",
	/* 0x1C  28 */ "Unknown_28",
	/* 0x1D  29 */ "Unknown_29",
	/* 0x1E  30 */ "Unknown_30",
	/* 0x1F  31 */ "Unknown_31",
	/* 0x20  32 */ "AllocAuxMem",
	/* 0x21  33 */ "Unknown_33",
	/* 0x22  34 */ "--Unknown--",
	/* 0x23  35 */ "--Unknown--",
	/* 0x24  36 */ "Unknown_36",
	/* 0x25  37 */ "Unknown_37",
	/* 0x26  38 */ "--Unknown--",
	/* 0x27  39 */ "--Unknown--",
	/* 0x28  40 */ "CreateDirectory",
	/* 0x29  41 */ "Unknown_41",
	/* 0x2A  42 */ "IsDirectory",
	/* 0x2B  43 */ "--Unknown--",
	/* 0x2C  44 */ "Unknown_44",
	/* 0x2D  45 */ "Unknown_45",
	/* 0x2E  46 */ "--Unknown--",
	/* 0x2F  47 */ "Unknown_47",
	/* 0x30  48 */ "Unknown_48",
	/* 0x31  49 */ "Unknown_49",
	/* 0x32  50 */ "Unknown_50",
	/* 0x33  51 */ "Unknown_51",
	/* 0x34  52 */ "FindFile",
	/* 0x35  53 */ "Unknown_53",
	/* 0x36  54 */ "EnableSearchPaths",
	/* 0x37  55 */ "AddSearchPath",
	/* 0x38  56 */ "Unknown_56",
	/* 0x39  57 */ "--Unknown--",
	/* 0x3A  58 */ "Unknown_58",
	/* 0x3B  59 */ "Unknown_59",
	/* 0x3C  60 */ "Unknown_60",
	/* 0x3D  61 */ "Unknown_0x3D",
	/* 0x3E  62 */ "Unknown_62",
	/* 0x3F  63 */ "Unknown_63",
	/* 0x40  64 */ "LoadProgram",
	/* 0x41  65 */ "DeleteProgram",
	/* 0x42  66 */ "--Unknown--",
	/* 0x43  67 */ "--Unknown--",
	/* 0x44  68 */ "CreateThread",
	/* 0x45  69 */ "Unknown_69",
	/* 0x46  70 */ "GetThreadID",
	/* 0x47  71 */ "Unknown_71",
	/* 0x48  72 */ "Unknown_72",
	/* 0x49  73 */ "Unknown_73",
	/* 0x4A  74 */ "Unknown_74",
	/* 0x4B  75 */ "Unknown_75",
	/* 0x4C  76 */ "Unknown_76",
	/* 0x4D  77 */ "--Unknown--",
	/* 0x4E  78 */ "--Unknown--",
	/* 0x4F  79 */ "--Unknown--",
	/* 0x50  80 */ "Unknown_80",
	/* 0x51  81 */ "--Unknown--",
	/* 0x52  82 */ "--Unknown--",
	/* 0x53  83 */ "--Unknown--",
	/* 0x54  84 */ "Unknown_84",
	/* 0x55  85 */ "--Unknown--",
	/* 0x56  86 */ "--Unknown--",
	/* 0x57  87 */ "--Unknown--",
	/* 0x58  88 */ "SetTimer",
	/* 0x59  89 */ "Unknown_89",
	/* 0x5A  90 */ "Unknown_90",
	/* 0x5B  91 */ "--Unknown--",
	/* 0x5C  92 */ "Unknown_92",
	/* 0x5D  93 */ "Unknown_93",
	/* 0x5E  94 */ "SwitchToThread",
	/* 0x5F  95 */ "Yield",
	/* 0x60  96 */ "SetDisplayMode",
	/* 0x61  97 */ "Unknown_97",
	/* 0x62  98 */ "SetKeySlots",
	/* 0x63  99 */ "Unknown_99",
	/* 0x64 100 */ "Unknown_100",
	/* 0x65 101 */ "Unknown_101",
	/* 0x66 102 */ "SetCursorShape",
	/* 0x67 103 */ "Unknown_103",
	/* 0x68 104 */ "SetGlobalUnknownVal001",
	/* 0x69 105 */ "Unknown_105",
	/* 0x6A 106 */ "Unknown_106",
	/* 0x6B 107 */ "Unknown_107",
	/* 0x6C 108 */ "Unknown_108",
	/* 0x6D 109 */ "Unknown_109",
	/* 0x6E 110 */ "--Unknown--",
	/* 0x6F 111 */ "Unknown_111",
	/* 0x70 112 */ "InitGlobalMem",
	/* 0x71 113 */ "Unknown_113",
	/* 0x72 114 */ "--Unknown--",
	/* 0x73 115 */ "--Unknown--",
	/* 0x74 116 */ "SetFlagUnknown10",
	/* 0x75 117 */ "--Unknown--",
	/* 0x76 118 */ "--Unknown--",
	/* 0x77 119 */ "--Unknown--",
	/* 0x78 120 */ "Unknown_120",
	/* 0x79 121 */ "Unknown_121",
	/* 0x7A 122 */ "Unknown_122",
	/* 0x7B 123 */ "Unknown_123",
	/* 0x7C 124 */ "--Unknown--",
	/* 0x7D 125 */ "--Unknown--",
	/* 0x7E 126 */ "--Unknown--",
	/* 0x7F 127 */ "--Unknown--",
	/* 0x80 128 */ "LoadGlobalDatabase",
	/* 0x81 129 */ "Unknown_129",
	/* 0x82 130 */ "Unknown_130",
	/* 0x83 131 */ "Unknown_131",
	/* 0x84 132 */ "Unknown_132",
	/* 0x85 133 */ "Unknown_133",
	/* 0x86 134 */ "--Unknown--",
	/* 0x87 135 */ "--Unknown--",
	/* 0x88 136 */ "Unknown_136",
	/* 0x89 137 */ "Unknown_137",
	/* 0x8A 138 */ "Unknown_138",
	/* 0x8B 139 */ "Unknown_139",
	/* 0x8C 140 */ "--Unknown--",
	/* 0x8D 141 */ "--Unknown--",
	/* 0x8E 142 */ "--Unknown--",
	/* 0x8F 143 */ "--Unknown--",
	/* 0x90 144 */ "Unknown_144",
	/* 0x91 145 */ "Unknown_145",
	/* 0x92 146 */ "--Unknown--",
	/* 0x93 147 */ "--Unknown--",
	/* 0x94 148 */ "Unknown_148",
	/* 0x95 149 */ "Unknown_149",
	/* 0x96 150 */ "Unknown_150",
	/* 0x97 151 */ "Unknown_151",
	/* 0x98 152 */ "Unknown_152",
	/* 0x99 153 */ "Unknown_153",
	/* 0x9A 154 */ "Unknown_154",
	/* 0x9B 155 */ "--Unknown--",
	/* 0x9C 156 */ "Unknown_156",
	/* 0x9D 157 */ "Unknown_157",
	/* 0x9E 158 */ "--Unknown--",
	/* 0x9F 159 */ "--Unknown--",
	/* 0xA0 160 */ "PopGlobalList",
	/* 0xA1 161 */ "PushGlobalList",
	/* 0xA2 162 */ "--Unknown--",
	/* 0xA3 163 */ "--Unknown--",
	/* 0xA4 164 */ "--Unknown--",
	/* 0xA5 165 */ "--Unknown--",
	/* 0xA6 166 */ "--Unknown--",
	/* 0xA7 167 */ "--Unknown--",
	/* 0xA8 168 */ "Unknown_168",
	/* 0xA9 169 */ "Unknown_169",
	/* 0xAA 170 */ "--Unknown--",
	/* 0xAB 171 */ "--Unknown--",
	/* 0xAC 172 */ "Unknown_172",
	/* 0xAD 173 */ "--Unknown--",
	/* 0xAE 174 */ "--Unknown--",
	/* 0xAF 175 */ "--Unknown--",
	/* 0xB0 176 */ "Unknown_176",
	/* 0xB1 177 */ "Unknown_177",
	/* 0xB2 178 */ "--Unknown--",
	/* 0xB3 179 */ "--Unknown--",
	/* 0xB4 180 */ "Unknown_180",
	/* 0xB5 181 */ "Unknown_181",
	/* 0xB6 182 */ "Unknown_182",
	/* 0xB7 183 */ "--Unknown--",
	/* 0xB8 184 */ "--Unknown--",
	/* 0xB9 185 */ "--Unknown--",
	/* 0xBA 186 */ "--Unknown--",
	/* 0xBB 187 */ "--Unknown--",
	/* 0xBC 188 */ "--Unknown--",
	/* 0xBD 189 */ "--Unknown--",
	/* 0xBE 190 */ "--Unknown--",
	/* 0xBF 191 */ "--Unknown--",
	/* 0xC0 192 */ "Unknown_192",
	/* 0xC1 193 */ "Unknown_193",
	/* 0xC2 194 */ "--Unknown--",
	/* 0xC3 195 */ "--Unknown--",
	/* 0xC4 196 */ "Unknown_196",
	/* 0xC5 197 */ "Unknown_197",
	/* 0xC6 198 */ "--Unknown--",
	/* 0xC7 199 */ "--Unknown--",
	/* 0xC8 200 */ "--Unknown--",
	/* 0xC9 201 */ "--Unknown--",
	/* 0xCA 202 */ "--Unknown--",
	/* 0xCB 203 */ "--Unknown--",
	/* 0xCC 204 */ "--Unknown--",
	/* 0xCD 205 */ "--Unknown--",
	/* 0xCE 206 */ "--Unknown--",
	/* 0xCF 207 */ "--Unknown--",
	/* 0xD0 208 */ "Unknown_208",
	/* 0xD1 209 */ "Unknown_209",
	/* 0xD2 210 */ "Unknown_210",
	/* 0xD3 211 */ "Unknown_211",
	/* 0xD4 212 */ "Unknown_212",
	/* 0xD5 213 */ "--Unknown--",
	/* 0xD6 214 */ "--Unknown--",
	/* 0xD7 215 */ "--Unknown--",
	/* 0xD8 216 */ "Unknown_216",
	/* 0xD9 217 */ "Unknown_217",
	/* 0xDA 218 */ "--Unknown--",
	/* 0xDB 219 */ "--Unknown--",
	/* 0xDC 220 */ "Unknown_220",
	/* 0xDD 221 */ "Unknown_221",
	/* 0xDE 222 */ "--Unknown--",
	/* 0xDF 223 */ "--Unknown--",
	/* 0xE0 224 */ "Unknown_224",
	/* 0xE1 225 */ "Unknown_225",
	/* 0xE2 226 */ "Unknown_226",
	/* 0xE3 227 */ "Unknown_227",
	/* 0xE4 228 */ "--Unknown--",
	/* 0xE5 229 */ "--Unknown--",
	/* 0xE6 230 */ "--Unknown--",
	/* 0xE7 231 */ "--Unknown--",
	/* 0xE8 232 */ "Unknown_232",
	/* 0xE9 233 */ "--Unknown--",
	/* 0xEA 234 */ "--Unknown--",
	/* 0xEB 235 */ "--Unknown--",
	/* 0xEC 236 */ "Unknown_236",
	/* 0xED 237 */ "Unknown_237",
	/* 0xEE 238 */ "Unknown_238",
	/* 0xEF 239 */ "Unknown_239",
	/* 0xF0 240 */ "Unknown_240",
	/* 0xF1 241 */ "Unknown_241",
	/* 0xF2 242 */ "Unknown_242",
	/* 0xF3 243 */ "Unknown_243",
	/* 0xF4 244 */ "Unknown_244",
	/* 0xF5 245 */ "Unknown_245",
	/* 0xF6 246 */ "Unknown_246",
	/* 0xF7 247 */ "Unknown_247",
	/* 0xF8 248 */ "Unknown_248",
	/* 0xF9 249 */ "Unknown_249",
	/* 0xFA 250 */ "Unknown_250",
	/* 0xFB 251 */ "Unknown_251",
	/* 0xFC 252 */ "Unknown_252",
	/* 0xFD 253 */ "IsLauncher",
	/* 0xFE 254 */ "Unknown_254",
	/* 0xFF 255 */ "--Unknown--"
};

OpcodePtr_t OpcodesSys0[256] = {
	/* 0x00   0 */ Opcode_Sys0_Srand,
	/* 0x01   1 */ Opcode_Sys0_Unknown_1,
	/* 0x02   2 */ Opcode_Sys0_Unknown_2,
	/* 0x03   3 */ NULL,
	/* 0x04   4 */ Opcode_Sys0_GetSysTime,
	/* 0x05   5 */ NULL,
	/* 0x06   6 */ NULL,
	/* 0x07   7 */ NULL,
	/* 0x08   8 */ Opcode_Sys0_Unknown_8,
	/* 0x09   9 */ NULL,
	/* 0x0A  10 */ Opcode_Sys0_Unknown_10,
	/* 0x0B  11 */ Opcode_Sys0_Unknown_11,
	/* 0x0C  12 */ Opcode_Sys0_Unknown_12,
	/* 0x0D  13 */ Opcode_Sys0_Unknown_13,
	/* 0x0E  14 */ Opcode_Sys0_Unknown_14,
	/* 0x0F  15 */ Opcode_Sys0_Unknown_15,
	/* 0x10  16 */ Opcode_Sys0_Unknown_16,
	/* 0x11  17 */ Opcode_Sys0_Unknown_17,
	/* 0x12  18 */ Opcode_Sys0_Unknown_18,
	/* 0x13  19 */ Opcode_Sys0_Unknown_19,
	/* 0x14  20 */ Opcode_Sys0_Unknown_20,
	/* 0x15  21 */ Opcode_Sys0_Unknown_21,
	/* 0x16  22 */ Opcode_Sys0_Unknown_22,
	/* 0x17  23 */ Opcode_Sys0_Unknown_23,
	/* 0x18  24 */ Opcode_Sys0_Unknown_24,
	/* 0x19  25 */ Opcode_Sys0_Unknown_25,
	/* 0x1A  26 */ Opcode_Sys0_Unknown_26,
	/* 0x1B  27 */ Opcode_Sys0_Unknown_0x1B,
	/* 0x1C  28 */ Opcode_Sys0_Unknown_28,
	/* 0x1D  29 */ Opcode_Sys0_Unknown_29,
	/* 0x1E  30 */ Opcode_Sys0_Unknown_30,
	/* 0x1F  31 */ Opcode_Sys0_Unknown_31,
	/* 0x20  32 */ Opcode_Sys0_AllocAuxMem,
	/* 0x21  33 */ Opcode_Sys0_Unknown_33,
	/* 0x22  34 */ NULL,
	/* 0x23  35 */ NULL,
	/* 0x24  36 */ Opcode_Sys0_Unknown_36,
	/* 0x25  37 */ Opcode_Sys0_Unknown_37,
	/* 0x26  38 */ NULL,
	/* 0x27  39 */ NULL,
	/* 0x28  40 */ Opcode_Sys0_CreateDirectory,
	/* 0x29  41 */ Opcode_Sys0_Unknown_41,
	/* 0x2A  42 */ Opcode_Sys0_IsDirectory,
	/* 0x2B  43 */ NULL,
	/* 0x2C  44 */ Opcode_Sys0_Unknown_44,
	/* 0x2D  45 */ Opcode_Sys0_Unknown_45,
	/* 0x2E  46 */ NULL,
	/* 0x2F  47 */ Opcode_Sys0_Unknown_47,
	/* 0x30  48 */ Opcode_Sys0_Unknown_48,
	/* 0x31  49 */ Opcode_Sys0_Unknown_49,
	/* 0x32  50 */ Opcode_Sys0_Unknown_50,
	/* 0x33  51 */ Opcode_Sys0_Unknown_51,
	/* 0x34  52 */ Opcode_Sys0_FindFile,
	/* 0x35  53 */ Opcode_Sys0_Unknown_53,
	/* 0x36  54 */ Opcode_Sys0_EnableSearchPaths,
	/* 0x37  55 */ Opcode_Sys0_AddSearchPath,
	/* 0x38  56 */ Opcode_Sys0_Unknown_56,
	/* 0x39  57 */ NULL,
	/* 0x3A  58 */ Opcode_Sys0_Unknown_58,
	/* 0x3B  59 */ Opcode_Sys0_Unknown_59,
	/* 0x3C  60 */ Opcode_Sys0_Unknown_60,
	/* 0x3D  61 */ Opcode_Sys0_Unknown_0x3D,
	/* 0x3E  62 */ Opcode_Sys0_Unknown_62,
	/* 0x3F  63 */ Opcode_Sys0_Unknown_63,
	/* 0x40  64 */ Opcode_Sys0_LoadProgram,
	/* 0x41  65 */ Opcode_Sys0_DeleteProgram,
	/* 0x42  66 */ NULL,
	/* 0x43  67 */ NULL,
	/* 0x44  68 */ Opcode_Sys0_CreateThread,
	/* 0x45  69 */ Opcode_Sys0_Unknown_69,
	/* 0x46  70 */ Opcode_Sys0_GetThreadID,
	/* 0x47  71 */ Opcode_Sys0_Unknown_71,
	/* 0x48  72 */ Opcode_Sys0_Unknown_72,
	/* 0x49  73 */ Opcode_Sys0_Unknown_73,
	/* 0x4A  74 */ Opcode_Sys0_Unknown_74,
	/* 0x4B  75 */ Opcode_Sys0_Unknown_75,
	/* 0x4C  76 */ Opcode_Sys0_Unknown_76,
	/* 0x4D  77 */ NULL,
	/* 0x4E  78 */ NULL,
	/* 0x4F  79 */ NULL,
	/* 0x50  80 */ Opcode_Sys0_Unknown_80,
	/* 0x51  81 */ NULL,
	/* 0x52  82 */ NULL,
	/* 0x53  83 */ NULL,
	/* 0x54  84 */ Opcode_Sys0_Unknown_84,
	/* 0x55  85 */ NULL,
	/* 0x56  86 */ NULL,
	/* 0x57  87 */ NULL,
	/* 0x58  88 */ Opcode_Sys0_SetTimer,
	/* 0x59  89 */ Opcode_Sys0_Unknown_89,
	/* 0x5A  90 */ Opcode_Sys0_Unknown_90,
	/* 0x5B  91 */ NULL,
	/* 0x5C  92 */ Opcode_Sys0_Unknown_92,
	/* 0x5D  93 */ Opcode_Sys0_Unknown_93,
	/* 0x5E  94 */ Opcode_Sys0_SwitchToThread,
	/* 0x5F  95 */ Opcode_Sys0_Yield,
	/* 0x60  96 */ Opcode_Sys0_SetDisplayMode,
	/* 0x61  97 */ Opcode_Sys0_Unknown_97,
	/* 0x62  98 */ Opcode_Sys0_SetKeySlots,
	/* 0x63  99 */ Opcode_Sys0_Unknown_99,
	/* 0x64 100 */ Opcode_Sys0_Unknown_100,
	/* 0x65 101 */ Opcode_Sys0_Unknown_101,
	/* 0x66 102 */ Opcode_Sys0_SetCursorShape,
	/* 0x67 103 */ Opcode_Sys0_Unknown_103,
	/* 0x68 104 */ Opcode_Sys0_SetGlobalUnknownVal001,
	/* 0x69 105 */ Opcode_Sys0_Unknown_105,
	/* 0x6A 106 */ Opcode_Sys0_Unknown_106,
	/* 0x6B 107 */ Opcode_Sys0_Unknown_107,
	/* 0x6C 108 */ Opcode_Sys0_Unknown_108,
	/* 0x6D 109 */ Opcode_Sys0_Unknown_109,
	/* 0x6E 110 */ NULL,
	/* 0x6F 111 */ Opcode_Sys0_Unknown_111,
	/* 0x70 112 */ Opcode_Sys0_InitGlobalMem,
	/* 0x71 113 */ Opcode_Sys0_Unknown_113,
	/* 0x72 114 */ NULL,
	/* 0x73 115 */ NULL,
	/* 0x74 116 */ Opcode_Sys0_SetFlagUnknown10,
	/* 0x75 117 */ NULL,
	/* 0x76 118 */ NULL,
	/* 0x77 119 */ NULL,
	/* 0x78 120 */ Opcode_Sys0_Unknown_120,
	/* 0x79 121 */ Opcode_Sys0_Unknown_121,
	/* 0x7A 122 */ Opcode_Sys0_Unknown_122,
	/* 0x7B 123 */ Opcode_Sys0_Unknown_123,
	/* 0x7C 124 */ NULL,
	/* 0x7D 125 */ NULL,
	/* 0x7E 126 */ NULL,
	/* 0x7F 127 */ NULL,
	/* 0x80 128 */ Opcode_Sys0_LoadGlobalDatabase,
	/* 0x81 129 */ Opcode_Sys0_Unknown_129,
	/* 0x82 130 */ Opcode_Sys0_Unknown_130,
	/* 0x83 131 */ Opcode_Sys0_Unknown_131,
	/* 0x84 132 */ Opcode_Sys0_Unknown_132,
	/* 0x85 133 */ Opcode_Sys0_Unknown_133,
	/* 0x86 134 */ NULL,
	/* 0x87 135 */ NULL,
	/* 0x88 136 */ Opcode_Sys0_Unknown_136,
	/* 0x89 137 */ Opcode_Sys0_Unknown_137,
	/* 0x8A 138 */ Opcode_Sys0_Unknown_138,
	/* 0x8B 139 */ Opcode_Sys0_Unknown_139,
	/* 0x8C 140 */ NULL,
	/* 0x8D 141 */ NULL,
	/* 0x8E 142 */ NULL,
	/* 0x8F 143 */ NULL,
	/* 0x90 144 */ Opcode_Sys0_Unknown_144,
	/* 0x91 145 */ Opcode_Sys0_Unknown_145,
	/* 0x92 146 */ NULL,
	/* 0x93 147 */ NULL,
	/* 0x94 148 */ Opcode_Sys0_Unknown_148,
	/* 0x95 149 */ Opcode_Sys0_Unknown_149,
	/* 0x96 150 */ Opcode_Sys0_Unknown_150,
	/* 0x97 151 */ Opcode_Sys0_Unknown_151,
	/* 0x98 152 */ Opcode_Sys0_Unknown_152,
	/* 0x99 153 */ Opcode_Sys0_Unknown_153,
	/* 0x9A 154 */ Opcode_Sys0_Unknown_154,
	/* 0x9B 155 */ NULL,
	/* 0x9C 156 */ Opcode_Sys0_Unknown_156,
	/* 0x9D 157 */ Opcode_Sys0_Unknown_157,
	/* 0x9E 158 */ NULL,
	/* 0x9F 159 */ NULL,
	/* 0xA0 160 */ Opcode_Sys0_PopGlobalList,
	/* 0xA1 161 */ Opcode_Sys0_PushGlobalList,
	/* 0xA2 162 */ NULL,
	/* 0xA3 163 */ NULL,
	/* 0xA4 164 */ NULL,
	/* 0xA5 165 */ NULL,
	/* 0xA6 166 */ NULL,
	/* 0xA7 167 */ NULL,
	/* 0xA8 168 */ Opcode_Sys0_Unknown_168,
	/* 0xA9 169 */ Opcode_Sys0_Unknown_169,
	/* 0xAA 170 */ NULL,
	/* 0xAB 171 */ NULL,
	/* 0xAC 172 */ Opcode_Sys0_Unknown_172,
	/* 0xAD 173 */ NULL,
	/* 0xAE 174 */ NULL,
	/* 0xAF 175 */ NULL,
	/* 0xB0 176 */ Opcode_Sys0_Unknown_176,
	/* 0xB1 177 */ Opcode_Sys0_Unknown_177,
	/* 0xB2 178 */ NULL,
	/* 0xB3 179 */ NULL,
	/* 0xB4 180 */ Opcode_Sys0_Unknown_180,
	/* 0xB5 181 */ Opcode_Sys0_Unknown_181,
	/* 0xB6 182 */ Opcode_Sys0_Unknown_182,
	/* 0xB7 183 */ NULL,
	/* 0xB8 184 */ NULL,
	/* 0xB9 185 */ NULL,
	/* 0xBA 186 */ NULL,
	/* 0xBB 187 */ NULL,
	/* 0xBC 188 */ NULL,
	/* 0xBD 189 */ NULL,
	/* 0xBE 190 */ NULL,
	/* 0xBF 191 */ NULL,
	/* 0xC0 192 */ Opcode_Sys0_Unknown_192,
	/* 0xC1 193 */ Opcode_Sys0_Unknown_193,
	/* 0xC2 194 */ NULL,
	/* 0xC3 195 */ NULL,
	/* 0xC4 196 */ Opcode_Sys0_Unknown_196,
	/* 0xC5 197 */ Opcode_Sys0_Unknown_197,
	/* 0xC6 198 */ NULL,
	/* 0xC7 199 */ NULL,
	/* 0xC8 200 */ NULL,
	/* 0xC9 201 */ NULL,
	/* 0xCA 202 */ NULL,
	/* 0xCB 203 */ NULL,
	/* 0xCC 204 */ NULL,
	/* 0xCD 205 */ NULL,
	/* 0xCE 206 */ NULL,
	/* 0xCF 207 */ NULL,
	/* 0xD0 208 */ Opcode_Sys0_Unknown_208,
	/* 0xD1 209 */ Opcode_Sys0_Unknown_209,
	/* 0xD2 210 */ Opcode_Sys0_Unknown_210,
	/* 0xD3 211 */ Opcode_Sys0_Unknown_211,
	/* 0xD4 212 */ Opcode_Sys0_Unknown_212,
	/* 0xD5 213 */ NULL,
	/* 0xD6 214 */ NULL,
	/* 0xD7 215 */ NULL,
	/* 0xD8 216 */ Opcode_Sys0_Unknown_216,
	/* 0xD9 217 */ Opcode_Sys0_Unknown_217,
	/* 0xDA 218 */ NULL,
	/* 0xDB 219 */ NULL,
	/* 0xDC 220 */ Opcode_Sys0_Unknown_220,
	/* 0xDD 221 */ Opcode_Sys0_Unknown_221,
	/* 0xDE 222 */ NULL,
	/* 0xDF 223 */ NULL,
	/* 0xE0 224 */ Opcode_Sys0_Unknown_224,
	/* 0xE1 225 */ Opcode_Sys0_Unknown_225,
	/* 0xE2 226 */ Opcode_Sys0_Unknown_226,
	/* 0xE3 227 */ Opcode_Sys0_Unknown_227,
	/* 0xE4 228 */ NULL,
	/* 0xE5 229 */ NULL,
	/* 0xE6 230 */ NULL,
	/* 0xE7 231 */ NULL,
	/* 0xE8 232 */ Opcode_Sys0_Unknown_232,
	/* 0xE9 233 */ NULL,
	/* 0xEA 234 */ NULL,
	/* 0xEB 235 */ NULL,
	/* 0xEC 236 */ Opcode_Sys0_Unknown_236,
	/* 0xED 237 */ Opcode_Sys0_Unknown_237,
	/* 0xEE 238 */ Opcode_Sys0_Unknown_238,
	/* 0xEF 239 */ Opcode_Sys0_Unknown_239,
	/* 0xF0 240 */ Opcode_Sys0_Unknown_240,
	/* 0xF1 241 */ Opcode_Sys0_Unknown_241,
	/* 0xF2 242 */ Opcode_Sys0_Unknown_242,
	/* 0xF3 243 */ Opcode_Sys0_Unknown_243,
	/* 0xF4 244 */ Opcode_Sys0_Unknown_244,
	/* 0xF5 245 */ Opcode_Sys0_Unknown_245,
	/* 0xF6 246 */ Opcode_Sys0_Unknown_246,
	/* 0xF7 247 */ Opcode_Sys0_Unknown_247,
	/* 0xF8 248 */ Opcode_Sys0_Unknown_248,
	/* 0xF9 249 */ Opcode_Sys0_Unknown_249,
	/* 0xFA 250 */ Opcode_Sys0_Unknown_250,
	/* 0xFB 251 */ Opcode_Sys0_Unknown_251,
	/* 0xFC 252 */ Opcode_Sys0_Unknown_252,
	/* 0xFD 253 */ Opcode_Sys0_IsLauncher,
	/* 0xFE 254 */ Opcode_Sys0_Unknown_254,
	/* 0xFF 255 */ NULL
};

uint32_t Opcode_Sys0_Srand(Thread_t* thread)
{
	uint32_t data = Thread_PopStack(thread);
	srand(data);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_1(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_2(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_GetSysTime(Thread_t* thread)
{
	// Dummy
	uint32_t data = 0xDEADBEEF;
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_8(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_10(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_11(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_12(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_13(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_14(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_15(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_16(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_17(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_18(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_19(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_20(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_21(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_22(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_23(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_24(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_25(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_26(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_Unknown_0x1B(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	uint32_t data = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_28(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_29(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_30(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_31(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_AllocAuxMem(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_33(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_36(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_37(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_CreateDirectory(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	printf("[Thread %d]: %sCreating directory \"%s\"\n", thread->threadId, TLevel[thread->level], ptr);
	Thread_PushStack(thread, 1);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_41(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_IsDirectory(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	Thread_PushStack(thread, 0);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_44(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_45(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_47(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_48(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_49(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint8_t* filename = Thread_PopAndResolveAddress(thread);
	uint8_t* ptr2 = Thread_PopAndResolveAddress(thread);
	uint8_t* memoryBuffer = Thread_PopAndResolveAddress(thread);

	//uint32_t res = FUN_00439940(memoryBuffer, ptr2, filename, value2, value1);

	printf("[Thread %d]: %sFUN_00439940(\"%s\", \"%s\", \"%s\", 0x%.8X, 0x%.8X)\n", thread->threadId, TLevel[thread->level],
		memoryBuffer, ptr2, filename, value2, value1);
	Thread_PushStack(thread, 1);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_50(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_51(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_FindFile(Thread_t* thread)
{
	uint8_t* ptr1 = Thread_PopAndResolveAddress(thread);
	uint8_t* ptr2 = Thread_PopAndResolveAddress(thread);
	printf("[Thread %d]: %s(\"%s\", \"%s\")\n", thread->threadId, TLevel[thread->level], ptr1, ptr2);
	Thread_PushStack(thread, 0xDEADBEEF);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_53(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_EnableSearchPaths(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetEnableSearchPaths(value);
	return 0;
}

uint32_t Opcode_Sys0_AddSearchPath(Thread_t* thread)
{
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	Engine_AddSearchPath(ptr);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_56(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_58(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_59(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_60(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_Unknown_0x3D(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint8_t* ptr = Thread_PopAndResolveAddress(thread);
	Thread_PushStack(thread, 0);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_62(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_63(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_LoadProgram(Thread_t* thread)
{
	uint8_t* filename = Thread_PopAndResolveAddress(thread);
	uint8_t* archive = Thread_PopAndResolveAddress(thread);
	printf("[Thread %d]: %sAttempting to load program [%s : %s]\n", thread->threadId, TLevel[thread->level], archive, filename);

	uint8_t* code = Engine_ReadFile(gEngine, archive, filename);
	if(code == NULL)
		return 1;
	uint32_t location = Thread_LoadCode(thread, code, filename);
	free(code);

	Thread_PushStack(thread, location);

	return 0;
}

uint32_t Opcode_Sys0_DeleteProgram(Thread_t* thread)
{
	uint32_t res = Thread_DeleteProgram(thread);
	Thread_PushStack(thread, res);
	return 0;
}

uint32_t Opcode_Sys0_CreateThread(Thread_t* thread)
{
	uint32_t memorySize = Thread_PopStack(thread);
	uint32_t codeSize = Thread_PopStack(thread);
	uint32_t stackSize = Thread_PopStack(thread);
	uint8_t* programFilename = Thread_PopAndResolveAddress(thread);
	uint8_t* archiveFilename = Thread_PopAndResolveAddress(thread);
	printf("[Thread %d]: %sAttempting create thread for program [%s : %s]\n", thread->threadId, TLevel[thread->level], archiveFilename, programFilename);
	uint32_t res = Engine_LoadProgram(gEngine, archiveFilename, programFilename, stackSize, codeSize, memorySize);
	Thread_PushStack(thread, res);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_69(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_GetThreadID(Thread_t* thread)
{
	uint32_t threadId = Thread_GetThreadID(thread);
	Thread_PushStack(thread, threadId);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_71(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_72(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_73(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_74(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_75(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_76(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_80(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_84(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_SetTimer(Thread_t* thread)
{
	uint32_t data = Thread_PopStack(thread);
	Thread_SetUnknownTimestamp(thread, data);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_89(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_90(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_92(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 2;
}

uint32_t Opcode_Sys0_Unknown_93(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_SwitchToThread(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Yield(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_SetDisplayMode(Thread_t* thread)
{
	uint32_t fullscreen = Thread_PopStack(thread);
	uint32_t contextParam = Thread_PopStack(thread);
	uint32_t modeIndex = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_97(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_SetKeySlots(Thread_t* thread)
{
	uint32_t* keys = (uint32_t*)Thread_PopAndResolveAddress(thread);
	uint32_t value = Thread_PopStack(thread);
	Engine_SetKeySlots(value, keys);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_99(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_100(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_101(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_SetCursorShape(Thread_t* thread)
{
	uint32_t shapeType = Thread_PopStack(thread);
	gCursorShape = shapeType;
	return 0;
}

uint32_t Opcode_Sys0_Unknown_103(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_SetGlobalUnknownVal001(Thread_t* thread)
{
	uint32_t data = Thread_PopStack(thread);
	SetGlobalUnknownVal001(data);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_105(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_106(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_107(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_108(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_109(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_111(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_InitGlobalMem(Thread_t* thread)
{
	uint32_t level = Thread_PopStack(thread);
	uint32_t res = InitGlobalMemory(level);
	Thread_PushStack(thread, res);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_113(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_SetFlagUnknown10(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetFlagUnknown10(value);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_120(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_121(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_122(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_123(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_LoadGlobalDatabase(Thread_t* thread)
{
	// It normally loads the database, but pushes 1 if it's not found, or 2 if it's corrupt
	// We'll pretend it doesn't exist
	Thread_PushStack(thread, 1);
	printf("[Thread %d]: %sWarning: dummy opcode: Pretending global database file doesn't exist\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_129(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_130(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_131(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_132(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_133(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_136(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_137(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_138(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_139(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_144(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_145(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_148(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_149(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_150(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_151(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_152(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_153(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_154(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_156(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_157(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_PopGlobalList(Thread_t* thread)
{
	uint32_t* ptr = (uint32_t*)Thread_PopAndResolveAddress(thread);
	uint32_t res = Engine_PopGlobalList(ptr);
	Thread_PushStack(thread, res);
	return 0;
}

uint32_t Opcode_Sys0_PushGlobalList(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Engine_PushGlobalList(0, value2, value1);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_168(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_169(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_172(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_176(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_177(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_180(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_181(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_182(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_192(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_193(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_196(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_197(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_208(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_209(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_210(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_211(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_212(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_216(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_217(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_220(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_221(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_224(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_225(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_226(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_227(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_232(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_236(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_237(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_238(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_239(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_240(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_241(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_242(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_243(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_244(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_245(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_246(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_247(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_248(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_249(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_250(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_251(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Sys0_Unknown_252(Thread_t* thread)
{
	return 0xFFFFFFFF;
}


uint32_t Opcode_Sys0_IsLauncher(Thread_t* thread)
{
	// Dummy
	uint32_t data = 0;
	Thread_PushStack(thread, data);
	return 0;
}

uint32_t Opcode_Sys0_Unknown_254(Thread_t* thread)
{
	return 0xFFFFFFFF;
}
