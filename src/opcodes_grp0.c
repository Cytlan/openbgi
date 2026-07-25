#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "engine.h"
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
	/* 0x60  96 */ "Unknown_96",
	/* 0x61  97 */ "Unknown_97",
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
	/* 0x81 129 */ "Unknown_129",
	/* 0x82 130 */ "--Unknown--",
	/* 0x83 131 */ "Unknown_131",
	/* 0x84 132 */ "Unknown_132",
	/* 0x85 133 */ "Unknown_133",
	/* 0x86 134 */ "Unknown_134",
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
	/* 0xE0 224 */ "Unknown_224",
	/* 0xE1 225 */ "Unknown_225",
	/* 0xE2 226 */ "--Unknown--",
	/* 0xE3 227 */ "--Unknown--",
	/* 0xE4 228 */ "Unknown_228",
	/* 0xE5 229 */ "Unknown_229",
	/* 0xE6 230 */ "--Unknown--",
	/* 0xE7 231 */ "--Unknown--",
	/* 0xE8 232 */ "Unknown_232",
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
	/* 0x60  96 */ Opcode_Grp0_Unknown_96,
	/* 0x61  97 */ Opcode_Grp0_Unknown_97,
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
	/* 0x81 129 */ Opcode_Grp0_Unknown_129,
	/* 0x82 130 */ NULL,
	/* 0x83 131 */ Opcode_Grp0_Unknown_131,
	/* 0x84 132 */ Opcode_Grp0_Unknown_132,
	/* 0x85 133 */ Opcode_Grp0_Unknown_133,
	/* 0x86 134 */ Opcode_Grp0_Unknown_134,
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
	/* 0xE0 224 */ Opcode_Grp0_Unknown_224,
	/* 0xE1 225 */ Opcode_Grp0_Unknown_225,
	/* 0xE2 226 */ NULL,
	/* 0xE3 227 */ NULL,
	/* 0xE4 228 */ Opcode_Grp0_Unknown_228,
	/* 0xE5 229 */ Opcode_Grp0_Unknown_229,
	/* 0xE6 230 */ NULL,
	/* 0xE7 231 */ NULL,
	/* 0xE8 232 */ Opcode_Grp0_Unknown_232,
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
	return 0xFFFFFFFF;
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
	return 0;
}

uint32_t Opcode_Grp0_Unknown_17(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_18(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_33(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_34(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
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

uint32_t Opcode_Grp0_Unknown_96(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_97(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_100(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_101(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_129(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_131(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_132(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_133(Thread_t* thread)
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

uint32_t Opcode_Grp0_Unknown_134(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_135(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp0_Unknown_136(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_157(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_183(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_184(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_185(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_186(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_188(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_208(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_215(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_216(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_217(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_218(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_219(Thread_t* thread)
{
	return 0xFFFFFFFF;
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

uint32_t Opcode_Grp0_Unknown_224(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_225(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_228(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_229(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp0_Unknown_232(Thread_t* thread)
{
	return 0xFFFFFFFF;
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
