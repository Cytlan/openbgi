#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "engine.h"
#include "renderer.h"
#include "opcodes.h"
#include "opcodes_grp0.h"
#include "thread.h"

char* OpcodesGrp0Mnemonics[256] = {
	/* 0x00   0 */ "Unknown_0",
	/* 0x01   1 */ "StopRendering",
	/* 0x02   2 */ "SetFramerate",
	/* 0x03   3 */ "Unknown_0x03",
	/* 0x04   4 */ "Unknown_4",
	/* 0x05   5 */ "Unknown_5",
	/* 0x06   6 */ "--Unknown--",
	/* 0x07   7 */ "--Unknown--",
	/* 0x08   8 */ "Unknown_8",
	/* 0x09   9 */ "Unknown_9",
	/* 0x0A  10 */ "Unknown_10",
	/* 0x0B  11 */ "Unknown_11",
	/* 0x0C  12 */ "SetOpacity",
	/* 0x0D  13 */ "SetAntialiasingLevel",
	/* 0x0E  14 */ "Unknown_0x0E",
	/* 0x0F  15 */ "Unknown_15",
	/* 0x10  16 */ "Unknown_0x10",
	/* 0x11  17 */ "Unknown_17",
	/* 0x12  18 */ "Unknown_18",
	/* 0x13  19 */ "Unknown_0x13",
	/* 0x14  20 */ "Unknown_20",
	/* 0x15  21 */ "Unknown_21",
	/* 0x16  22 */ "Unknown_22",
	/* 0x17  23 */ "--Unknown--",
	/* 0x18  24 */ "Unknown_24",
	/* 0x19  25 */ "Unknown_25",
	/* 0x1A  26 */ "Unknown_26",
	/* 0x1B  27 */ "Unknown_27",
	/* 0x1C  28 */ "Unknown_28",
	/* 0x1D  29 */ "Unknown_29",
	/* 0x1E  30 */ "Unknown_30",
	/* 0x1F  31 */ "Unknown_31",
	/* 0x20  32 */ "Unknown_32",
	/* 0x21  33 */ "Unknown_33",
	/* 0x22  34 */ "Unknown_34",
	/* 0x23  35 */ "Unknown_35",
	/* 0x24  36 */ "Unknown_36",
	/* 0x25  37 */ "--Unknown--",
	/* 0x26  38 */ "--Unknown--",
	/* 0x27  39 */ "--Unknown--",
	/* 0x28  40 */ "Unknown_40",
	/* 0x29  41 */ "Unknown_41",
	/* 0x2A  42 */ "--Unknown--",
	/* 0x2B  43 */ "--Unknown--",
	/* 0x2C  44 */ "Unknown_44",
	/* 0x2D  45 */ "--Unknown--",
	/* 0x2E  46 */ "--Unknown--",
	/* 0x2F  47 */ "--Unknown--",
	/* 0x30  48 */ "Unknown_48",
	/* 0x31  49 */ "Unknown_49",
	/* 0x32  50 */ "Unknown_50",
	/* 0x33  51 */ "Unknown_51",
	/* 0x34  52 */ "Unknown_52",
	/* 0x35  53 */ "Unknown_53",
	/* 0x36  54 */ "--Unknown--",
	/* 0x37  55 */ "Unknown_55",
	/* 0x38  56 */ "Unknown_56",
	/* 0x39  57 */ "--Unknown--",
	/* 0x3A  58 */ "--Unknown--",
	/* 0x3B  59 */ "--Unknown--",
	/* 0x3C  60 */ "Unknown_60",
	/* 0x3D  61 */ "Unknown_61",
	/* 0x3E  62 */ "--Unknown--",
	/* 0x3F  63 */ "Unknown_63",
	/* 0x40  64 */ "Unknown_64",
	/* 0x41  65 */ "Unknown_65",
	/* 0x42  66 */ "Unknown_66",
	/* 0x43  67 */ "Unknown_67",
	/* 0x44  68 */ "Unknown_68",
	/* 0x45  69 */ "Unknown_69",
	/* 0x46  70 */ "Unknown_70",
	/* 0x47  71 */ "Unknown_71",
	/* 0x48  72 */ "Unknown_72",
	/* 0x49  73 */ "Unknown_73",
	/* 0x4A  74 */ "Unknown_74",
	/* 0x4B  75 */ "--Unknown--",
	/* 0x4C  76 */ "Unknown_76",
	/* 0x4D  77 */ "Unknown_77",
	/* 0x4E  78 */ "--Unknown--",
	/* 0x4F  79 */ "--Unknown--",
	/* 0x50  80 */ "Unknown_80",
	/* 0x51  81 */ "Unknown_81",
	/* 0x52  82 */ "--Unknown--",
	/* 0x53  83 */ "Unknown_83",
	/* 0x54  84 */ "Unknown_84",
	/* 0x55  85 */ "Unknown_85",
	/* 0x56  86 */ "Unknown_86",
	/* 0x57  87 */ "Unknown_87",
	/* 0x58  88 */ "Unknown_88",
	/* 0x59  89 */ "Unknown_89",
	/* 0x5A  90 */ "Unknown_90",
	/* 0x5B  91 */ "Unknown_91",
	/* 0x5C  92 */ "Unknown_92",
	/* 0x5D  93 */ "--Unknown--",
	/* 0x5E  94 */ "--Unknown--",
	/* 0x5F  95 */ "--Unknown--",
	/* 0x60  96 */ "CreateFilterObject",
	/* 0x61  97 */ "DestroyFilterObject",
	/* 0x62  98 */ "--Unknown--",
	/* 0x63  99 */ "--Unknown--",
	/* 0x64 100 */ "Unknown_100",
	/* 0x65 101 */ "Unknown_101",
	/* 0x66 102 */ "Unknown_102",
	/* 0x67 103 */ "--Unknown--",
	/* 0x68 104 */ "--Unknown--",
	/* 0x69 105 */ "--Unknown--",
	/* 0x6A 106 */ "--Unknown--",
	/* 0x6B 107 */ "--Unknown--",
	/* 0x6C 108 */ "--Unknown--",
	/* 0x6D 109 */ "--Unknown--",
	/* 0x6E 110 */ "--Unknown--",
	/* 0x6F 111 */ "--Unknown--",
	/* 0x70 112 */ "Unknown_112",
	/* 0x71 113 */ "Unknown_113",
	/* 0x72 114 */ "--Unknown--",
	/* 0x73 115 */ "--Unknown--",
	/* 0x74 116 */ "Unknown_116",
	/* 0x75 117 */ "Unknown_117",
	/* 0x76 118 */ "Unknown_118",
	/* 0x77 119 */ "--Unknown--",
	/* 0x78 120 */ "Unknown_120",
	/* 0x79 121 */ "Unknown_121",
	/* 0x7A 122 */ "Unknown_122",
	/* 0x7B 123 */ "--Unknown--",
	/* 0x7C 124 */ "--Unknown--",
	/* 0x7D 125 */ "--Unknown--",
	/* 0x7E 126 */ "--Unknown--",
	/* 0x7F 127 */ "--Unknown--",
	/* 0x80 128 */ "CreateWindowObject",
	/* 0x81 129 */ "DestroyWindowObject",
	/* 0x82 130 */ "--Unknown--",
	/* 0x83 131 */ "Unknown_131",
	/* 0x84 132 */ "Unknown_132",
	/* 0x85 133 */ "Unknown_133",
	/* 0x86 134 */ "DrawBitmapToWindow",
	/* 0x87 135 */ "Unknown_135",
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
	/* 0x94 148 */ "SetFlagUnknown20",
	/* 0x95 149 */ "SetSplits2",
	/* 0x96 150 */ "SetSplits",
	/* 0x97 151 */ "SetUnknownGrp0Val1and2",
	/* 0x98 152 */ "Unknown_152",
	/* 0x99 153 */ "Unknown_153",
	/* 0x9A 154 */ "Unknown_154",
	/* 0x9B 155 */ "Unknown_155",
	/* 0x9C 156 */ "Unknown_156",
	/* 0x9D 157 */ "Unknown_157",
	/* 0x9E 158 */ "Unknown_158",
	/* 0x9F 159 */ "SetFlagUnknown21",
	/* 0xA0 160 */ "Unknown_160",
	/* 0xA1 161 */ "Unknown_161",
	/* 0xA2 162 */ "Unknown_162",
	/* 0xA3 163 */ "Unknown_163",
	/* 0xA4 164 */ "Unknown_164",
	/* 0xA5 165 */ "Unknown_165",
	/* 0xA6 166 */ "Unknown_166",
	/* 0xA7 167 */ "Unknown_167",
	/* 0xA8 168 */ "--Unknown--",
	/* 0xA9 169 */ "--Unknown--",
	/* 0xAA 170 */ "--Unknown--",
	/* 0xAB 171 */ "--Unknown--",
	/* 0xAC 172 */ "--Unknown--",
	/* 0xAD 173 */ "--Unknown--",
	/* 0xAE 174 */ "--Unknown--",
	/* 0xAF 175 */ "SetFlag1to4",
	/* 0xB0 176 */ "Unknown_176",
	/* 0xB1 177 */ "Unknown_177",
	/* 0xB2 178 */ "--Unknown--",
	/* 0xB3 179 */ "--Unknown--",
	/* 0xB4 180 */ "Unknown_180",
	/* 0xB5 181 */ "Unknown_181",
	/* 0xB6 182 */ "Unknown_182",
	/* 0xB7 183 */ "Unknown_183",
	/* 0xB8 184 */ "Unknown_184",
	/* 0xB9 185 */ "Unknown_185",
	/* 0xBA 186 */ "Unknown_186",
	/* 0xBB 187 */ "--Unknown--",
	/* 0xBC 188 */ "Unknown_188",
	/* 0xBD 189 */ "Unknown_189",
	/* 0xBE 190 */ "Unknown_190",
	/* 0xBF 191 */ "Unknown_191",
	/* 0xC0 192 */ "--Unknown--",
	/* 0xC1 193 */ "--Unknown--",
	/* 0xC2 194 */ "--Unknown--",
	/* 0xC3 195 */ "--Unknown--",
	/* 0xC4 196 */ "--Unknown--",
	/* 0xC5 197 */ "--Unknown--",
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
	/* 0xD2 210 */ "--Unknown--",
	/* 0xD3 211 */ "--Unknown--",
	/* 0xD4 212 */ "Unknown_212",
	/* 0xD5 213 */ "Unknown_213",
	/* 0xD6 214 */ "Unknown_214",
	/* 0xD7 215 */ "Unknown_215",
	/* 0xD8 216 */ "Unknown_216",
	/* 0xD9 217 */ "Unknown_217",
	/* 0xDA 218 */ "Unknown_218",
	/* 0xDB 219 */ "Unknown_219",
	/* 0xDC 220 */ "Unknown_220",
	/* 0xDD 221 */ "Unknown_221",
	/* 0xDE 222 */ "Unknown_222",
	/* 0xDF 223 */ "Unknown_223",
	/* 0xE0 224 */ "CreateGroupObject",
	/* 0xE1 225 */ "Unknown_225",
	/* 0xE2 226 */ "--Unknown--",
	/* 0xE3 227 */ "--Unknown--",
	/* 0xE4 228 */ "Unknown_228",
	/* 0xE5 229 */ "Unknown_229",
	/* 0xE6 230 */ "--Unknown--",
	/* 0xE7 231 */ "--Unknown--",
	/* 0xE8 232 */ "AddObjectToGroup",
	/* 0xE9 233 */ "Unknown_233",
	/* 0xEA 234 */ "--Unknown--",
	/* 0xEB 235 */ "--Unknown--",
	/* 0xEC 236 */ "--Unknown--",
	/* 0xED 237 */ "--Unknown--",
	/* 0xEE 238 */ "--Unknown--",
	/* 0xEF 239 */ "--Unknown--",
	/* 0xF0 240 */ "Unknown_240",
	/* 0xF1 241 */ "Unknown_241",
	/* 0xF2 242 */ "Unknown_242",
	/* 0xF3 243 */ "Unknown_243",
	/* 0xF4 244 */ "--Unknown--",
	/* 0xF5 245 */ "--Unknown--",
	/* 0xF6 246 */ "--Unknown--",
	/* 0xF7 247 */ "--Unknown--",
	/* 0xF8 248 */ "Unknown_248",
	/* 0xF9 249 */ "--Unknown--",
	/* 0xFA 250 */ "Unknown_250",
	/* 0xFB 251 */ "Unknown_251",
	/* 0xFC 252 */ "Unknown_252",
	/* 0xFD 253 */ "Unknown_253",
	/* 0xFE 254 */ "--Unknown--",
	/* 0xFF 255 */ "--Unknown--",
};

OpcodePtr_t OpcodesGrp0[256] = {
	/* 0x00   0 */ Opcode_Grp0_Unknown_0,
	/* 0x01   1 */ Opcode_Grp0_StopRendering,
	/* 0x02   2 */ Opcode_Grp0_SetFramerate,
	/* 0x03   3 */ Opcode_Grp0_Unknown_0x03,
	/* 0x04   4 */ Opcode_Grp0_Unknown_4,
	/* 0x05   5 */ Opcode_Grp0_Unknown_5,
	/* 0x06   6 */ NULL,
	/* 0x07   7 */ NULL,
	/* 0x08   8 */ Opcode_Grp0_Unknown_8,
	/* 0x09   9 */ Opcode_Grp0_Unknown_9,
	/* 0x0A  10 */ Opcode_Grp0_Unknown_10,
	/* 0x0B  11 */ Opcode_Grp0_Unknown_11,
	/* 0x0C  12 */ Opcode_Grp0_SetOpacity,
	/* 0x0D  13 */ Opcode_Grp0_SetAntialiasingLevel,
	/* 0x0E  14 */ Opcode_Grp0_Unknown_0x0E,
	/* 0x0F  15 */ Opcode_Grp0_Unknown_15,
	/* 0x10  16 */ Opcode_Grp0_Unknown_0x10,
	/* 0x11  17 */ Opcode_Grp0_Unknown_17,
	/* 0x12  18 */ Opcode_Grp0_Unknown_18,
	/* 0x13  19 */ Opcode_Grp0_Unknown_0x13,
	/* 0x14  20 */ Opcode_Grp0_Unknown_20,
	/* 0x15  21 */ Opcode_Grp0_Unknown_21,
	/* 0x16  22 */ Opcode_Grp0_Unknown_22,
	/* 0x17  23 */ NULL,
	/* 0x18  24 */ Opcode_Grp0_Unknown_24,
	/* 0x19  25 */ Opcode_Grp0_Unknown_25,
	/* 0x1A  26 */ Opcode_Grp0_Unknown_26,
	/* 0x1B  27 */ Opcode_Grp0_Unknown_27,
	/* 0x1C  28 */ Opcode_Grp0_Unknown_28,
	/* 0x1D  29 */ Opcode_Grp0_Unknown_29,
	/* 0x1E  30 */ Opcode_Grp0_Unknown_30,
	/* 0x1F  31 */ Opcode_Grp0_Unknown_31,
	/* 0x20  32 */ Opcode_Grp0_Unknown_32,
	/* 0x21  33 */ Opcode_Grp0_Unknown_33,
	/* 0x22  34 */ Opcode_Grp0_Unknown_34,
	/* 0x23  35 */ Opcode_Grp0_Unknown_35,
	/* 0x24  36 */ Opcode_Grp0_Unknown_36,
	/* 0x25  37 */ NULL,
	/* 0x26  38 */ NULL,
	/* 0x27  39 */ NULL,
	/* 0x28  40 */ Opcode_Grp0_Unknown_40,
	/* 0x29  41 */ Opcode_Grp0_Unknown_41,
	/* 0x2A  42 */ NULL,
	/* 0x2B  43 */ NULL,
	/* 0x2C  44 */ Opcode_Grp0_Unknown_44,
	/* 0x2D  45 */ NULL,
	/* 0x2E  46 */ NULL,
	/* 0x2F  47 */ NULL,
	/* 0x30  48 */ Opcode_Grp0_Unknown_48,
	/* 0x31  49 */ Opcode_Grp0_Unknown_49,
	/* 0x32  50 */ Opcode_Grp0_Unknown_50,
	/* 0x33  51 */ Opcode_Grp0_Unknown_51,
	/* 0x34  52 */ Opcode_Grp0_Unknown_52,
	/* 0x35  53 */ Opcode_Grp0_Unknown_53,
	/* 0x36  54 */ NULL,
	/* 0x37  55 */ Opcode_Grp0_Unknown_55,
	/* 0x38  56 */ Opcode_Grp0_Unknown_56,
	/* 0x39  57 */ NULL,
	/* 0x3A  58 */ NULL,
	/* 0x3B  59 */ NULL,
	/* 0x3C  60 */ Opcode_Grp0_Unknown_60,
	/* 0x3D  61 */ Opcode_Grp0_Unknown_61,
	/* 0x3E  62 */ NULL,
	/* 0x3F  63 */ Opcode_Grp0_Unknown_63,
	/* 0x40  64 */ Opcode_Grp0_Unknown_64,
	/* 0x41  65 */ Opcode_Grp0_Unknown_65,
	/* 0x42  66 */ Opcode_Grp0_Unknown_66,
	/* 0x43  67 */ Opcode_Grp0_Unknown_67,
	/* 0x44  68 */ Opcode_Grp0_Unknown_68,
	/* 0x45  69 */ Opcode_Grp0_Unknown_69,
	/* 0x46  70 */ Opcode_Grp0_Unknown_70,
	/* 0x47  71 */ Opcode_Grp0_Unknown_71,
	/* 0x48  72 */ Opcode_Grp0_Unknown_72,
	/* 0x49  73 */ Opcode_Grp0_Unknown_73,
	/* 0x4A  74 */ Opcode_Grp0_Unknown_74,
	/* 0x4B  75 */ NULL,
	/* 0x4C  76 */ Opcode_Grp0_Unknown_76,
	/* 0x4D  77 */ Opcode_Grp0_Unknown_77,
	/* 0x4E  78 */ NULL,
	/* 0x4F  79 */ NULL,
	/* 0x50  80 */ Opcode_Grp0_Unknown_80,
	/* 0x51  81 */ Opcode_Grp0_Unknown_81,
	/* 0x52  82 */ NULL,
	/* 0x53  83 */ Opcode_Grp0_Unknown_83,
	/* 0x54  84 */ Opcode_Grp0_Unknown_84,
	/* 0x55  85 */ Opcode_Grp0_Unknown_85,
	/* 0x56  86 */ Opcode_Grp0_Unknown_86,
	/* 0x57  87 */ Opcode_Grp0_Unknown_87,
	/* 0x58  88 */ Opcode_Grp0_Unknown_88,
	/* 0x59  89 */ Opcode_Grp0_Unknown_89,
	/* 0x5A  90 */ Opcode_Grp0_Unknown_90,
	/* 0x5B  91 */ Opcode_Grp0_Unknown_91,
	/* 0x5C  92 */ Opcode_Grp0_Unknown_92,
	/* 0x5D  93 */ NULL,
	/* 0x5E  94 */ NULL,
	/* 0x5F  95 */ NULL,
	/* 0x60  96 */ Opcode_Grp0_CreateFilterObject,
	/* 0x61  97 */ Opcode_Grp0_DestroyFilterObject,
	/* 0x62  98 */ NULL,
	/* 0x63  99 */ NULL,
	/* 0x64 100 */ Opcode_Grp0_Unknown_100,
	/* 0x65 101 */ Opcode_Grp0_Unknown_101,
	/* 0x66 102 */ Opcode_Grp0_Unknown_102,
	/* 0x67 103 */ NULL,
	/* 0x68 104 */ NULL,
	/* 0x69 105 */ NULL,
	/* 0x6A 106 */ NULL,
	/* 0x6B 107 */ NULL,
	/* 0x6C 108 */ NULL,
	/* 0x6D 109 */ NULL,
	/* 0x6E 110 */ NULL,
	/* 0x6F 111 */ NULL,
	/* 0x70 112 */ Opcode_Grp0_Unknown_112,
	/* 0x71 113 */ Opcode_Grp0_Unknown_113,
	/* 0x72 114 */ NULL,
	/* 0x73 115 */ NULL,
	/* 0x74 116 */ Opcode_Grp0_Unknown_116,
	/* 0x75 117 */ Opcode_Grp0_Unknown_117,
	/* 0x76 118 */ Opcode_Grp0_Unknown_118,
	/* 0x77 119 */ NULL,
	/* 0x78 120 */ Opcode_Grp0_Unknown_120,
	/* 0x79 121 */ Opcode_Grp0_Unknown_121,
	/* 0x7A 122 */ Opcode_Grp0_Unknown_122,
	/* 0x7B 123 */ NULL,
	/* 0x7C 124 */ NULL,
	/* 0x7D 125 */ NULL,
	/* 0x7E 126 */ NULL,
	/* 0x7F 127 */ NULL,
	/* 0x80 128 */ Opcode_Grp0_CreateWindowObject,
	/* 0x81 129 */ Opcode_Grp0_DestroyWindowObject,
	/* 0x82 130 */ NULL,
	/* 0x83 131 */ Opcode_Grp0_Unknown_131,
	/* 0x84 132 */ Opcode_Grp0_Unknown_132,
	/* 0x85 133 */ Opcode_Grp0_Unknown_133,
	/* 0x86 134 */ Opcode_Grp0_DrawBitmapToWindow,
	/* 0x87 135 */ Opcode_Grp0_Unknown_135,
	/* 0x88 136 */ Opcode_Grp0_Unknown_136,
	/* 0x89 137 */ Opcode_Grp0_Unknown_137,
	/* 0x8A 138 */ Opcode_Grp0_Unknown_138,
	/* 0x8B 139 */ Opcode_Grp0_Unknown_139,
	/* 0x8C 140 */ NULL,
	/* 0x8D 141 */ NULL,
	/* 0x8E 142 */ NULL,
	/* 0x8F 143 */ NULL,
	/* 0x90 144 */ Opcode_Grp0_Unknown_144,
	/* 0x91 145 */ Opcode_Grp0_Unknown_145,
	/* 0x92 146 */ NULL,
	/* 0x93 147 */ NULL,
	/* 0x94 148 */ Opcode_Grp0_SetFlagUnknown20,
	/* 0x95 149 */ Opcode_Grp0_SetSplits2,
	/* 0x96 150 */ Opcode_Grp0_SetSplits,
	/* 0x97 151 */ Opcode_Grp0_SetUnknownGrp0Val1and2,
	/* 0x98 152 */ Opcode_Grp0_Unknown_152,
	/* 0x99 153 */ Opcode_Grp0_Unknown_153,
	/* 0x9A 154 */ Opcode_Grp0_Unknown_154,
	/* 0x9B 155 */ Opcode_Grp0_Unknown_155,
	/* 0x9C 156 */ Opcode_Grp0_Unknown_156,
	/* 0x9D 157 */ Opcode_Grp0_Unknown_157,
	/* 0x9E 158 */ Opcode_Grp0_Unknown_158,
	/* 0x9F 159 */ Opcode_Grp0_SetFlagUnknown21,
	/* 0xA0 160 */ Opcode_Grp0_Unknown_160,
	/* 0xA1 161 */ Opcode_Grp0_Unknown_161,
	/* 0xA2 162 */ Opcode_Grp0_Unknown_162,
	/* 0xA3 163 */ Opcode_Grp0_Unknown_163,
	/* 0xA4 164 */ Opcode_Grp0_Unknown_164,
	/* 0xA5 165 */ Opcode_Grp0_Unknown_165,
	/* 0xA6 166 */ Opcode_Grp0_Unknown_166,
	/* 0xA7 167 */ Opcode_Grp0_Unknown_167,
	/* 0xA8 168 */ NULL,
	/* 0xA9 169 */ NULL,
	/* 0xAA 170 */ NULL,
	/* 0xAB 171 */ NULL,
	/* 0xAC 172 */ NULL,
	/* 0xAD 173 */ NULL,
	/* 0xAE 174 */ NULL,
	/* 0xAF 175 */ Opcode_Grp0_SetFlag1to4,
	/* 0xB0 176 */ Opcode_Grp0_Unknown_176,
	/* 0xB1 177 */ Opcode_Grp0_Unknown_177,
	/* 0xB2 178 */ NULL,
	/* 0xB3 179 */ NULL,
	/* 0xB4 180 */ Opcode_Grp0_Unknown_180,
	/* 0xB5 181 */ Opcode_Grp0_Unknown_181,
	/* 0xB6 182 */ Opcode_Grp0_Unknown_182,
	/* 0xB7 183 */ Opcode_Grp0_Unknown_183,
	/* 0xB8 184 */ Opcode_Grp0_Unknown_184,
	/* 0xB9 185 */ Opcode_Grp0_Unknown_185,
	/* 0xBA 186 */ Opcode_Grp0_Unknown_186,
	/* 0xBB 187 */ NULL,
	/* 0xBC 188 */ Opcode_Grp0_Unknown_188,
	/* 0xBD 189 */ Opcode_Grp0_Unknown_189,
	/* 0xBE 190 */ Opcode_Grp0_Unknown_190,
	/* 0xBF 191 */ Opcode_Grp0_Unknown_191,
	/* 0xC0 192 */ NULL,
	/* 0xC1 193 */ NULL,
	/* 0xC2 194 */ NULL,
	/* 0xC3 195 */ NULL,
	/* 0xC4 196 */ NULL,
	/* 0xC5 197 */ NULL,
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
	/* 0xD0 208 */ Opcode_Grp0_Unknown_208,
	/* 0xD1 209 */ Opcode_Grp0_Unknown_209,
	/* 0xD2 210 */ NULL,
	/* 0xD3 211 */ NULL,
	/* 0xD4 212 */ Opcode_Grp0_Unknown_212,
	/* 0xD5 213 */ Opcode_Grp0_Unknown_213,
	/* 0xD6 214 */ Opcode_Grp0_Unknown_214,
	/* 0xD7 215 */ Opcode_Grp0_Unknown_215,
	/* 0xD8 216 */ Opcode_Grp0_Unknown_216,
	/* 0xD9 217 */ Opcode_Grp0_Unknown_217,
	/* 0xDA 218 */ Opcode_Grp0_Unknown_218,
	/* 0xDB 219 */ Opcode_Grp0_Unknown_219,
	/* 0xDC 220 */ Opcode_Grp0_Unknown_220,
	/* 0xDD 221 */ Opcode_Grp0_Unknown_221,
	/* 0xDE 222 */ Opcode_Grp0_Unknown_222,
	/* 0xDF 223 */ Opcode_Grp0_Unknown_223,
	/* 0xE0 224 */ Opcode_Grp0_CreateGroupObject,
	/* 0xE1 225 */ Opcode_Grp0_Unknown_225,
	/* 0xE2 226 */ NULL,
	/* 0xE3 227 */ NULL,
	/* 0xE4 228 */ Opcode_Grp0_Unknown_228,
	/* 0xE5 229 */ Opcode_Grp0_Unknown_229,
	/* 0xE6 230 */ NULL,
	/* 0xE7 231 */ NULL,
	/* 0xE8 232 */ Opcode_Grp0_AddObjectToGroup,
	/* 0xE9 233 */ Opcode_Grp0_Unknown_233,
	/* 0xEA 234 */ NULL,
	/* 0xEB 235 */ NULL,
	/* 0xEC 236 */ NULL,
	/* 0xED 237 */ NULL,
	/* 0xEE 238 */ NULL,
	/* 0xEF 239 */ NULL,
	/* 0xF0 240 */ Opcode_Grp0_Unknown_240,
	/* 0xF1 241 */ Opcode_Grp0_Unknown_241,
	/* 0xF2 242 */ Opcode_Grp0_Unknown_242,
	/* 0xF3 243 */ Opcode_Grp0_Unknown_243,
	/* 0xF4 244 */ NULL,
	/* 0xF5 245 */ NULL,
	/* 0xF6 246 */ NULL,
	/* 0xF7 247 */ NULL,
	/* 0xF8 248 */ Opcode_Grp0_Unknown_248,
	/* 0xF9 249 */ NULL,
	/* 0xFA 250 */ Opcode_Grp0_Unknown_250,
	/* 0xFB 251 */ Opcode_Grp0_Unknown_251,
	/* 0xFC 252 */ Opcode_Grp0_Unknown_252,
	/* 0xFD 253 */ Opcode_Grp0_Unknown_253,
	/* 0xFE 254 */ NULL,
	/* 0xFF 255 */ NULL,
};

uint32_t Opcode_Grp0_Unknown_0(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_StopRendering(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_SetFramerate(Thread_t* thread)
{
	uint32_t fps = Thread_PopStack(thread);
	Engine_SetFramerateTime(fps);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_0x03(Thread_t* thread)
{
	uint32_t size = Thread_PopStack(thread);
	// Max 0x20000000?
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_4(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_5(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_8(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_9(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_10(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_11(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_SetOpacity(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_SetAntialiasingLevel(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetAntialiasingLevel(value);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_0x0E(Thread_t* thread)
{
	// SetCacheQuantityForFont
	uint32_t value1 = Thread_PopStack(thread); // Cache size
	uint32_t value2 = Thread_PopStack(thread); // ???
	uint32_t value3 = Thread_PopStack(thread); // Font width
	uint32_t value4 = Thread_PopStack(thread); // Window size
	uint32_t value5 = Thread_PopStack(thread); // Font number
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	/*
	uVar6 = FUN_004366d0(value5,value4,value3,value2,value1);

	無効なフォント番号 [ %d ] が指定されました - An invalid font number [%d] was specified.
	無効なウィンドウサイズ [ %d, %d ] が指定されました - An invalid window size [%d, %d] was specified.
	無効なフォント幅 [ %d ] が指定されました - An invalid font width [%d] was specified.
	無効なキャッシュ量 [ %d ] が指定されました - An invalid cache size [%d] was specified.

	*/
	return 0;
}

uint32_t Opcode_Grp0_Unknown_15(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_0x10(Thread_t* thread)
{	
	uint8_t* filename = Thread_PopAndResolveAddress(thread);
	uint8_t* archive = Thread_PopAndResolveAddress(thread);
	uint32_t bitmapSlot = Thread_PopStack(thread);
	printf("[Thread %d]: %sLoad bitmap? [%s : %s] (%d)\n", thread->threadId, TLevel[thread->level], filename, archive, bitmapSlot);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);

	Engine_t* engine = thread->engine;
	Renderer_LoadBitmap(engine->renderer, bitmapSlot, filename, archive);
	return 2;
}

uint32_t Opcode_Grp0_Unknown_17(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_18(Thread_t* thread)
{
	// Maybe unload assets?
	uint32_t grpAssetId = Thread_PopStack(thread);
	Thread_PushStack(thread, 0x00000001);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_0x13(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_20(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_21(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_22(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t* ptr = (uint32_t*)Thread_PopAndResolveAddress(thread);

	*ptr = 0x03FD2298; ptr++;
	*ptr = 0x00000190; ptr++;
	*ptr = 0x00000064; ptr++;
	*ptr = 0x00000028; ptr++;
	*ptr = 0x00000002; ptr++;
	*ptr = 0x00000004; ptr++;
	*ptr = 0x00000000; ptr++;

	Thread_PushStack(thread, 1);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_24(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_25(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_26(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_27(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_28(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_29(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_30(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_31(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_32(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t value5 = Thread_PopStack(thread);
	uint32_t value6 = Thread_PopStack(thread);
	Thread_SchedulePush(thread, 0x00000000);
	Thread_SchedulePush(thread, 0x00000078);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 2;
}

uint32_t Opcode_Grp0_Unknown_33(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_34(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t value5 = Thread_PopStack(thread);
	uint32_t value6 = Thread_PopStack(thread);
	uint32_t value7 = Thread_PopStack(thread);
	Thread_SchedulePush(thread, 0x00000000);
	Thread_SchedulePush(thread, 0x00000078);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 2;
}

uint32_t Opcode_Grp0_Unknown_35(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_36(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_40(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_41(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_44(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_48(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_49(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_50(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_51(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_52(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_53(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_55(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_56(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_60(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_61(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_63(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_64(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_65(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_66(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_67(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_68(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_69(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_70(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_71(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_72(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_73(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_74(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_76(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_77(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_80(Thread_t* thread)
{
	// CreateSpriteObject
	uint32_t handle = thread->engine->spriteObjectHandle;
	thread->engine->spriteObjectHandle++;
	Thread_PushStack(thread, handle);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_81(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_83(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_84(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_85(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_86(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t value5 = Thread_PopStack(thread);
	uint32_t value6 = Thread_PopStack(thread);
	uint32_t value7 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_87(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_88(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_89(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_90(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_91(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_92(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_CreateFilterObject(Thread_t* thread)
{
	Thread_PushStack(thread, thread->engine->filterObjectHandle);
	thread->engine->filterObjectHandle++;
	return 0;
}

uint32_t Opcode_Grp0_DestroyFilterObject(Thread_t* thread)
{
	uint32_t data = Thread_PopStack(thread);
	thread->engine->filterObjectHandle--;
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_100(Thread_t* thread)
{
    uint32_t unknown = Thread_PopStack(thread);
    uint32_t filterObject = Thread_PopStack(thread);
    printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
    return 0;
}

uint32_t Opcode_Grp0_Unknown_101(Thread_t* thread)
{
    uint32_t value1 = Thread_PopStack(thread);
    uint32_t opacity = Thread_PopStack(thread);
    uint32_t unknown = Thread_PopStack(thread);
    uint32_t filterObject = Thread_PopStack(thread);
    printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
    return 0;
}

uint32_t Opcode_Grp0_Unknown_102(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_112(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_113(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_116(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_117(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_118(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_120(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_121(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_122(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_CreateWindowObject(Thread_t* thread)
{
	uint32_t height = Thread_PopStack(thread);
	uint32_t width = Thread_PopStack(thread);
	if(height == 19 && width == 25)
	{
		height = 600;
		width = 800;
	}
	uint32_t handle = Renderer_CreateScreen(thread->engine->renderer, width, height);
	Thread_PushStack(thread, handle);
	return 0;
}

uint32_t Opcode_Grp0_DestroyWindowObject(Thread_t* thread)
{
	uint32_t handle = Thread_PopStack(thread);
	Renderer_DestroyScreen(thread->engine->renderer, handle);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_131(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_132(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_133(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t y = Thread_PopStack(thread);
	uint32_t x = Thread_PopStack(thread);
	uint32_t screenId = Thread_PopStack(thread);
	Renderer_SetScreenParams(thread->engine->renderer, screenId, x, y);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_DrawBitmapToWindow(Thread_t* thread)
{
	uint32_t bitmapId = Thread_PopStack(thread);
	uint32_t unknown1 = Thread_PopStack(thread);
	uint32_t unknown2 = Thread_PopStack(thread);
	uint32_t screenId = Thread_PopStack(thread);
	if((screenId & 0x00FFFFFF) > RENDERER_MAX_SCREENS)
	{
		printf("[Thread %d]: %sError: attempted to draw to invalid screen 0x%08X\n", thread->threadId, TLevel[thread->level], screenId);
		return 10;
	}
	Renderer_DrawBitmapToScreen(thread->engine->renderer, bitmapId, screenId & 0x000000FF);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_135(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t value5 = Thread_PopStack(thread);
	uint32_t value6 = Thread_PopStack(thread);
	uint32_t value7 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_136(Thread_t* thread)
{
	// Seems to set the drawable area for button elements etc.
	// Maybe it's SetAvailableAreaOfWindow?
	uint32_t height = Thread_PopStack(thread);
	uint32_t width = Thread_PopStack(thread);
	uint32_t x = Thread_PopStack(thread);
	uint32_t y = Thread_PopStack(thread);
	uint32_t screenId = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_137(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_138(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_139(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_144(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_145(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_SetFlagUnknown20(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetFlagUnknown20(value);
	return 0;
}

uint32_t Opcode_Grp0_SetSplits2(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_SetSplits(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_SetUnknownGrp0Val1and2(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Engine_SetUnknownGrp0Val1and2(value1, value2);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_152(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_153(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_154(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_155(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_156(Thread_t* thread)
{
	// Sets DAT_004871c0
	uint32_t value = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_157(Thread_t* thread)
{
	uint32_t route1 = Thread_PopStack(thread);
	uint32_t coord1 = Thread_PopStack(thread);
	uint32_t coord2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_158(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_SetFlagUnknown21(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetFlagUnknown21(value);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_160(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_161(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_162(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_163(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_164(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_165(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_166(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_167(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_SetFlag1to4(Thread_t* thread)
{
	uint32_t value = Thread_PopStack(thread);
	Engine_SetFlagUnknown1to4(value);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_176(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_177(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_180(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_181(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_182(Thread_t* thread)
{
	/*
	r8:  [24598017, 2, 16731, 3, 0x00000090, 0x00000AE3]
	exec: [24598209, 2, 16731, 0, 0x0090, 1, 0x00000001,0x00000002,0x04598C10,0x00000000,0x0000022B,0x00000AE3,0x00000AE4,0x00000434,0x00001000,0x00359EB0,0x0035A330,0x00020000,0x00359ED0,0x019C0048,0x03E35798,0x00000002,0x00001630,0x00020000,0x00359EF0,0x019E0050]
	rc8:  [24598394, 2, 16731, 4, 0x000000B6, 0x00000AE3]
	pop:  [24598541, 2, 16731, 1, 0x100003D0] <- List location
	pop:  [24598686, 2, 16731, 1, 0xC0000000]
	mem:  [24598863, READ, Local, 0x00000001, 000003D0, 019e0420, 00429149, 312]
	mem:  [24599072, READ, Local, 0x1000039C, 000003D4, 019e0424, 00429149, 312] <- Location for total elements
	mem:  [24599277, READ, Local, 0xFFFFFFFF, 000003D8, 019e0428, 00429149, 312]
	mem:  [24599470, READ, Local, 0x00000001, 000003DC, 019e042c, 00429149, 312]
	mem:  [24599662, READ, Local, 0x00000000, 000003E0, 019e0430, 00429149, 312]
	mem:  [24599853, READ, Local, 0x00000202, 000003E4, 019e0434, 00429149, 312]
	mem:  [24600054, READ, Local, 0x00000000, 000003E8, 019e0438, 00429149, 312]
	mem:  [24600271, READ, Local, 0x00000001, 000003EC, 019e043c, 00429149, 312]

	mem:  [24600467, READ, Local, 0x1000039C, 000003D4, 019e0424, 0042914b, 312] <- Location for total elements

	mem:  [24600658, READ, Local, 0x00000001, 000003D0, 019e0420, 00429168, 312]

	mem:  [24600853, READ, Local, 0x00000001, 000003D0, 019e0420, 00429190, 312]

	mem:  [24601054, READ, Local, 0x00000005, 0000039C, 019e03ec, 004291c6, 312] <- Total elements
	mem:  [24601282, READ, Local, 0x10000270, 000003A0, 019e03f0, 004291c6, 312] <- Location of list
	mem:  [24601479, READ, Local, 0xFFFFFFFF, 000003A4, 019e03f4, 004291c6, 312]
	mem:  [24601673, READ, Local, 0x00000000, 000003A8, 019e03f8, 004291c6, 312]
	mem:  [24601866, READ, Local, 0x00000000, 000003AC, 019e03fc, 004291c6, 312]
	mem:  [24602066, READ, Local, 0x00000000, 000003B0, 019e0400, 004291c6, 312]
	mem:  [24602270, READ, Local, 0xFFFFFFFF, 000003B4, 019e0404, 004291c6, 312]
	mem:  [24602465, READ, Local, 0x00000000, 000003B8, 019e0408, 004291c6, 312]
	mem:  [24602657, READ, Local, 0x00000000, 000003BC, 019e040c, 004291c6, 312]
	mem:  [24602853, READ, Local, 0xFFFFFFFF, 000003C0, 019e0410, 004291c6, 312]
	mem:  [24603054, READ, Local, 0x00000000, 000003C4, 019e0414, 004291c6, 312]
	mem:  [24603258, READ, Local, 0x00000000, 000003C8, 019e0418, 004291c6, 312]
	mem:  [24603454, READ, Local, 0xFFFFFFFF, 000003CC, 019e041c, 004291c6, 312]

	mem:  [24603693, READ, Local, 0x10000270, 000003A0, 019e03f0, 004291c8, 312]

	mem:  [24603907, READ, Local, 0x00000005, 0000039C, 019e03ec, 004291df, 312] <- Total elements

	mem:  [24604119, READ, Local, 0x00000005, 0000039C, 019e03ec, 004291ff, 312] <- Total elements

	mem:  [24604333, READ, Local, 0x00000001, 00000270, 019e02c0, 00429222, 312]
	mem:  [24604540, READ, Local, 0x00000064, 00000274, 019e02c4, 00429222, 312] <- X coordinate
	mem:  [24604746, READ, Local, 0x000000EB, 00000278, 019e02c8, 00429222, 312] <- Y coordinate
	mem:  [24604950, READ, Local, 0x00000801, 0000027C, 019e02cc, 00429222, 312] <- [tit_101 : sysgrp.arc] (New game)
	mem:  [24605171, READ, Local, 0xFFFFFFFF, 00000280, 019e02d0, 00429222, 312]
	mem:  [24605381, READ, Local, 0x00000802, 00000284, 019e02d4, 00429222, 312] <- [tit_102 : sysgrp.arc] (New game : highlighted)
	mem:  [24605596, READ, Local, 0xFFFFFFFF, 00000288, 019e02d8, 00429222, 312]
	mem:  [24605806, READ, Local, 0x00000000, 0000028C, 019e02dc, 00429222, 312]
	mem:  [24606001, READ, Local, 0x00000000, 00000290, 019e02e0, 00429222, 312]
	mem:  [24606212, READ, Local, 0xFFFFFFFF, 00000294, 019e02e4, 00429222, 312]
	mem:  [24606413, READ, Local, 0x00000000, 00000298, 019e02e8, 00429222, 312]
	mem:  [24606610, READ, Local, 0x00000000, 0000029C, 019e02ec, 00429222, 312]
	mem:  [24606806, READ, Local, 0xFFFFFFFF, 000002A0, 019e02f0, 00429222, 312]
	mem:  [24607019, READ, Local, 0x00000000, 000002A4, 019e02f4, 00429222, 312]
	mem:  [24607241, READ, Local, 0x00000000, 000002A8, 019e02f8, 00429222, 312]

	mem:  [24607435, READ, Local, 0x00000005, 0000039C, 019e03ec, 00429224, 312] <- Total elements

	mem:  [24607630, READ, Local, 0x00000001, 000002AC, 019e02fc, 00429222, 312]
	mem:  [24607827, READ, Local, 0x00000064, 000002B0, 019e0300, 00429222, 312] <- X coordinate
	mem:  [24608026, READ, Local, 0x00000122, 000002B4, 019e0304, 00429222, 312] <- Y coordinate
	mem:  [24608238, READ, Local, 0x00000806, 000002B8, 019e0308, 00429222, 312] <- [tit_106 : sysgrp.arc] (Continue : disabled)
	mem:  [24608467, READ, Local, 0xFFFFFFFF, 000002BC, 019e030c, 00429222, 312]
	mem:  [24608678, READ, Local, 0x00000805, 000002C0, 019e0310, 00429222, 312] <- [tit_105 : sysgrp.arc] (Continue : highlighted)
	mem:  [24608883, READ, Local, 0xFFFFFFFE, 000002C4, 019e0314, 00429222, 312]
	mem:  [24609089, READ, Local, 0x00000000, 000002C8, 019e0318, 00429222, 312]
	mem:  [24609297, READ, Local, 0x00000000, 000002CC, 019e031c, 00429222, 312]
	mem:  [24609494, READ, Local, 0xFFFFFFFF, 000002D0, 019e0320, 00429222, 312]
	mem:  [24609692, READ, Local, 0x00000000, 000002D4, 019e0324, 00429222, 312]
	mem:  [24609892, READ, Local, 0x00000000, 000002D8, 019e0328, 00429222, 312]
	mem:  [24610097, READ, Local, 0xFFFFFFFF, 000002DC, 019e032c, 00429222, 312]
	mem:  [24610298, READ, Local, 0x00000000, 000002E0, 019e0330, 00429222, 312]
	mem:  [24610493, READ, Local, 0x00000000, 000002E4, 019e0334, 00429222, 312]

	mem:  [24610693, READ, Local, 0x00000005, 0000039C, 019e03ec, 00429224, 312] <- Total elements

	mem:  [24610894, READ, Local, 0x00000001, 000002E8, 019e0338, 00429222, 312]
	mem:  [24611103, READ, Local, 0x00000064, 000002EC, 019e033c, 00429222, 312] <- X coordinate
	mem:  [24611302, READ, Local, 0x00000159, 000002F0, 019e0340, 00429222, 312] <- Y coordinate
	mem:  [24611497, READ, Local, 0x00000807, 000002F4, 019e0344, 00429222, 312] <- [tit_107 : sysgrp.arc] (Options)
	mem:  [24611693, READ, Local, 0xFFFFFFFF, 000002F8, 019e0348, 00429222, 312]
	mem:  [24611889, READ, Local, 0x00000808, 000002FC, 019e034c, 00429222, 312] <- [tit_108 : sysgrp.arc] (Options : highlighted)
	mem:  [24612121, READ, Local, 0xFFFFFFFF, 00000300, 019e0350, 00429222, 312]
	mem:  [24612272, READ, Local, 0x00000000, 00000304, 019e0354, 00429222, 312]
	mem:  [24612410, READ, Local, 0x00000000, 00000308, 019e0358, 00429222, 312]
	mem:  [24612547, READ, Local, 0xFFFFFFFF, 0000030C, 019e035c, 00429222, 312]
	mem:  [24612684, READ, Local, 0x00000000, 00000310, 019e0360, 00429222, 312]
	mem:  [24612819, READ, Local, 0x00000000, 00000314, 019e0364, 00429222, 312]
	mem:  [24612957, READ, Local, 0xFFFFFFFF, 00000318, 019e0368, 00429222, 312]
	mem:  [24613107, READ, Local, 0x00000000, 0000031C, 019e036c, 00429222, 312]
	mem:  [24613247, READ, Local, 0x00000000, 00000320, 019e0370, 00429222, 312]

	mem:  [24613398, READ, Local, 0x00000005, 0000039C, 019e03ec, 00429224, 312] <- Total elements

	mem:  [24613558, READ, Local, 0x00000001, 00000324, 019e0374, 00429222, 312]
	mem:  [24613709, READ, Local, 0x00000064, 00000328, 019e0378, 00429222, 312] <- X coordinate
	mem:  [24613860, READ, Local, 0x00000190, 0000032C, 019e037c, 00429222, 312] <- Y coordinate
	mem:  [24614031, READ, Local, 0x0000080C, 00000330, 019e0380, 00429222, 312] <- [tit_112 : sysgrp.arc] (Bonus : disabled)
	mem:  [24614232, READ, Local, 0xFFFFFFFF, 00000334, 019e0384, 00429222, 312]
	mem:  [24614412, READ, Local, 0xFFFFFFFF, 00000338, 019e0388, 00429222, 312]
	mem:  [24614593, READ, Local, 0xFFFFFFFE, 0000033C, 019e038c, 00429222, 312]
	mem:  [24614773, READ, Local, 0x00000000, 00000340, 019e0390, 00429222, 312]
	mem:  [24614956, READ, Local, 0x00000000, 00000344, 019e0394, 00429222, 312]
	mem:  [24615147, READ, Local, 0xFFFFFFFF, 00000348, 019e0398, 00429222, 312]
	mem:  [24615349, READ, Local, 0x00000000, 0000034C, 019e039c, 00429222, 312]
	mem:  [24615550, READ, Local, 0x00000000, 00000350, 019e03a0, 00429222, 312]
	mem:  [24615748, READ, Local, 0xFFFFFFFF, 00000354, 019e03a4, 00429222, 312]
	mem:  [24615947, READ, Local, 0x00000000, 00000358, 019e03a8, 00429222, 312]
	mem:  [24616158, READ, Local, 0x00000000, 0000035C, 019e03ac, 00429222, 312]

	mem:  [24616356, READ, Local, 0x00000005, 0000039C, 019e03ec, 00429224, 312] <- Total elements

	mem:  [24616551, READ, Local, 0x00000001, 00000360, 019e03b0, 00429222, 312]
	mem:  [24616745, READ, Local, 0x00000064, 00000364, 019e03b4, 00429222, 312] <- X coordinate
	mem:  [24616941, READ, Local, 0x000001C7, 00000368, 019e03b8, 00429222, 312] <- Y coordinate
	mem:  [24617151, READ, Local, 0x0000080D, 0000036C, 019e03bc, 00429222, 312] <- [tit_113 : sysgrp.arc] (Quit)
	mem:  [24617350, READ, Local, 0xFFFFFFFF, 00000370, 019e03c0, 00429222, 312]
	mem:  [24617543, READ, Local, 0x0000080E, 00000374, 019e03c4, 00429222, 312] <- [tit_114 : sysgrp.arc] (Quit : highlighted)
	mem:  [24617736, READ, Local, 0xFFFFFFFF, 00000378, 019e03c8, 00429222, 312]
	mem:  [24617931, READ, Local, 0x00000000, 0000037C, 019e03cc, 00429222, 312]
	mem:  [24618143, READ, Local, 0x00000000, 00000380, 019e03d0, 00429222, 312]
	mem:  [24618342, READ, Local, 0xFFFFFFFF, 00000384, 019e03d4, 00429222, 312]
	mem:  [24618544, READ, Local, 0x00000000, 00000388, 019e03d8, 00429222, 312]
	mem:  [24618744, READ, Local, 0x00000000, 0000038C, 019e03dc, 00429222, 312]
	mem:  [24618943, READ, Local, 0xFFFFFFFF, 00000390, 019e03e0, 00429222, 312]
	mem:  [24619152, READ, Local, 0x00000000, 00000394, 019e03e4, 00429222, 312]
	mem:  [24619356, READ, Local, 0x00000000, 00000398, 019e03e8, 00429222, 312]

	mem:  [24619558, READ, Local, 0x00000005, 0000039C, 019e03ec, 00429224, 312] <- Total elements

	mem:  [24619755, READ, Local, 0x00000001, 000003D0, 019e0420, 0042924c, 312]
	push: [24621511, 2, 16731, 2, 0x00000000]
	exec: [24621704, 2, 16731, 3, 0x0090, 1, 0x00000001,0x00000002,0x04598C10,0x00000000,0x0000022A,0x00000AE3,0x00000AE5,0x00000434,0x00001000,0x00359EB0,0x0035A330,0x00020000,0x00359ED0,0x019C0048,0x03E35798,0x00000002,0x00001630,0x00020000,0x00359EF0,0x019E0050]
	*/

	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Thread_PushStack(thread, 0);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_183(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

int gScreenObjectId = 0x00000001;
uint32_t Opcode_Grp0_Unknown_184(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	Thread_PushStack(thread, gScreenObjectId++);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_185(Thread_t* thread)
{
	// Maybe create screen object?
	uint32_t value1 = Thread_PopStack(thread);
	Thread_PushStack(thread, 0x00000001);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_186(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Thread_PushStack(thread, 0x00000000);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_188(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Thread_PushStack(thread, 0x00000001);

	// This write might not actually happen here: Need to do a mem dump before & after this opcode to verify
	uint32_t* ptr = (uint32_t*)Thread_ResolveAddr(thread, value2);
	if(thread->ticks == 35696)
	{
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000069; ptr++;
		*ptr = 0x0000005A; ptr++;
		*ptr = 0x00000058; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
	}
	else if(thread->ticks == 36318)
	{
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000069; ptr++;
		*ptr = 0x0000005A; ptr++;
		*ptr = 0x00000058; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
	}
	else if(
		thread->ticks == 659 ||
		thread->ticks == 678 ||
		thread->ticks == 697 ||
		thread->ticks == 716 ||
		thread->ticks == 735 ||
		thread->ticks == 754 ||
		thread->ticks == 773 ||
		thread->ticks == 792 ||
		thread->ticks == 811 ||
		thread->ticks == 849 ||
		thread->ticks == 868 ||
		thread->ticks == 887 ||
		thread->ticks == 906 ||
		thread->ticks == 925 ||
		thread->ticks == 944 ||
		thread->ticks == 830 || 
		thread->ticks == 887
	)
	{
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000000; ptr++;
		*ptr = 0x00000002; ptr++;
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000032; ptr++;
		*ptr = 0x00000014; ptr++;
	}
	else if(thread->ticks == 963)
	{
		*ptr = 0x00000000; ptr++;
		*ptr = 0x00000000; ptr++;
		*ptr = 0x00000000; ptr++;
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000032; ptr++;
		*ptr = 0x00000014; ptr++;
	}
	else if(thread->ticks == 963 || thread->ticks == 36940 || thread->ticks == 37562)
	{
		*ptr = 0x00000001; ptr++;
		*ptr = 0x00000069; ptr++;
		*ptr = 0x0000005A; ptr++;
		*ptr = 0x00000058; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
		*ptr = 0xFFFFFFFF; ptr++;
	}
	else
	{
		if(thread->ticks < 16817 || thread->ticks >= 19742)
		{
			*ptr = 0x00000000; ptr++;
			*ptr = 0x00000000; ptr++;
		}
		else
		{
			*ptr = 0x00000001; ptr++;
			*ptr = 0xFFFFFF00; ptr++;
		}
		if(thread->ticks >= 19742)
		{
			*ptr = 0x00000002; ptr++;
			*ptr = 0x00000001; ptr++;
			*ptr = 0x0000004E; ptr++;
			*ptr = 0x00000046; ptr++;
		}
		else
		{
			*ptr = 0x00000000; ptr++;
			*ptr = 0x00000000; ptr++;
			*ptr = 0x00000001; ptr++;
			*ptr = 0x00000062; ptr++;
			*ptr = 0x00000026; ptr++;
		}
	}

	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_189(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_190(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_191(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	Thread_PushStack(thread, 0x00000001);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_208(Thread_t* thread)
{
	// CreateKnobObject
	uint32_t value1 = Thread_PopStack(thread);

	uint32_t handle = thread->engine->knobObjectHandle;
	thread->engine->knobObjectHandle++;
	Thread_PushStack(thread, handle);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_209(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_212(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_213(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_214(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_215(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	if(
		thread->ticks == 35382 ||
		thread->ticks == 35432 ||
		thread->ticks == 36004 ||
		thread->ticks == 36054 ||
		thread->ticks == 36626 ||
		thread->ticks == 36676 ||
		thread->ticks == 37248 ||
		thread->ticks == 37298 ||
		thread->ticks == 37870 ||
		thread->ticks == 37920
	)
		Thread_PushStack(thread, 0x00000066);
	else if(
		thread->ticks == 35592 ||
		thread->ticks == 35642 ||
		thread->ticks == 36214 ||
		thread->ticks == 36264 ||
		thread->ticks == 36836 ||
		thread->ticks == 36886 ||
		thread->ticks == 37458 ||
		thread->ticks == 37508
	)
		Thread_PushStack(thread, 0x00000002);
	else
		Thread_PushStack(thread, 0x00000080);
	Thread_PushStack(thread, 0);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_216(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_217(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_218(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_219(Thread_t* thread)
{
	Thread_PushStack(thread, 0);
	//uint32_t value1 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_220(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_221(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_222(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_223(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_CreateGroupObject(Thread_t* thread)
{
	Thread_PushStack(thread, 0xf0000000);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_225(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_228(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t groupHandle = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_229(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t groupHandle = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_AddObjectToGroup(Thread_t* thread)
{
	// 無効なグループハンドルが指定されました - Invalid group handle specified
	// 指定されたオブジェクトにはオーナーが存在します - The specified object has an owner
	// 無効なオブジェクトハンドルが指定されました - Invalid object handle specified (== 1)
	// 自分自身をグループに登録することはできません - You cannot register yourself to a group. (!= 3)
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t objectHandle = Thread_PopStack(thread);
	uint32_t groupHandle = Thread_PopStack(thread);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_233(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_240(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_241(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_242(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_243(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_248(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_250(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_251(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_252(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_253(Thread_t* thread)
{
	return 0xFFFFFFFF;
}
