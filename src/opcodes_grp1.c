#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "engine.h"
#include "opcodes.h"
#include "opcodes_grp1.h"
#include "thread.h"

char* OpcodesGrp1Mnemonics[256] = {
    /* 0x00   0 */ "--Unknown--",
    /* 0x01   1 */ "--Unknown--",
    /* 0x02   2 */ "--Unknown--",
    /* 0x03   3 */ "--Unknown--",
    /* 0x04   4 */ "--Unknown--",
    /* 0x05   5 */ "--Unknown--",
    /* 0x06   6 */ "--Unknown--",
    /* 0x07   7 */ "--Unknown--",
    /* 0x08   8 */ "--Unknown--",
    /* 0x09   9 */ "--Unknown--",
    /* 0x0A  10 */ "--Unknown--",
    /* 0x0B  11 */ "--Unknown--",
    /* 0x0C  12 */ "--Unknown--",
    /* 0x0D  13 */ "--Unknown--",
    /* 0x0E  14 */ "--Unknown--",
    /* 0x0F  15 */ "--Unknown--",
    /* 0x10  16 */ "Unknown_16",
    /* 0x11  17 */ "Unknown_17",
    /* 0x12  18 */ "Unknown_18",
    /* 0x13  19 */ "Unknown_19",
    /* 0x14  20 */ "Unknown_20",
    /* 0x15  21 */ "Unknown_21",
    /* 0x16  22 */ "--Unknown--",
    /* 0x17  23 */ "--Unknown--",
    /* 0x18  24 */ "Unknown_24",
    /* 0x19  25 */ "Unknown_25",
    /* 0x1A  26 */ "--Unknown--",
    /* 0x1B  27 */ "--Unknown--",
    /* 0x1C  28 */ "Unknown_28",
    /* 0x1D  29 */ "--Unknown--",
    /* 0x1E  30 */ "Unknown_30",
    /* 0x1F  31 */ "Unknown_31",
    /* 0x20  32 */ "--Unknown--",
    /* 0x21  33 */ "--Unknown--",
    /* 0x22  34 */ "--Unknown--",
    /* 0x23  35 */ "--Unknown--",
    /* 0x24  36 */ "--Unknown--",
    /* 0x25  37 */ "--Unknown--",
    /* 0x26  38 */ "--Unknown--",
    /* 0x27  39 */ "--Unknown--",
    /* 0x28  40 */ "--Unknown--",
    /* 0x29  41 */ "--Unknown--",
    /* 0x2A  42 */ "--Unknown--",
    /* 0x2B  43 */ "--Unknown--",
    /* 0x2C  44 */ "--Unknown--",
    /* 0x2D  45 */ "--Unknown--",
    /* 0x2E  46 */ "--Unknown--",
    /* 0x2F  47 */ "--Unknown--",
    /* 0x30  48 */ "--Unknown--",
    /* 0x31  49 */ "--Unknown--",
    /* 0x32  50 */ "--Unknown--",
    /* 0x33  51 */ "Unknown_51",
    /* 0x34  52 */ "--Unknown--",
    /* 0x35  53 */ "--Unknown--",
    /* 0x36  54 */ "--Unknown--",
    /* 0x37  55 */ "--Unknown--",
    /* 0x38  56 */ "--Unknown--",
    /* 0x39  57 */ "--Unknown--",
    /* 0x3A  58 */ "--Unknown--",
    /* 0x3B  59 */ "--Unknown--",
    /* 0x3C  60 */ "--Unknown--",
    /* 0x3D  61 */ "--Unknown--",
    /* 0x3E  62 */ "--Unknown--",
    /* 0x3F  63 */ "--Unknown--",
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
    /* 0x4C  76 */ "--Unknown--",
    /* 0x4D  77 */ "--Unknown--",
    /* 0x4E  78 */ "--Unknown--",
    /* 0x4F  79 */ "--Unknown--",
    /* 0x50  80 */ "--Unknown--",
    /* 0x51  81 */ "--Unknown--",
    /* 0x52  82 */ "--Unknown--",
    /* 0x53  83 */ "--Unknown--",
    /* 0x54  84 */ "--Unknown--",
    /* 0x55  85 */ "--Unknown--",
    /* 0x56  86 */ "--Unknown--",
    /* 0x57  87 */ "--Unknown--",
    /* 0x58  88 */ "--Unknown--",
    /* 0x59  89 */ "--Unknown--",
    /* 0x5A  90 */ "--Unknown--",
    /* 0x5B  91 */ "--Unknown--",
    /* 0x5C  92 */ "--Unknown--",
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
    /* 0x67 103 */ "Unknown_103",
    /* 0x68 104 */ "Unknown_104",
    /* 0x69 105 */ "--Unknown--",
    /* 0x6A 106 */ "--Unknown--",
    /* 0x6B 107 */ "--Unknown--",
    /* 0x6C 108 */ "--Unknown--",
    /* 0x6D 109 */ "--Unknown--",
    /* 0x6E 110 */ "--Unknown--",
    /* 0x6F 111 */ "--Unknown--",
    /* 0x70 112 */ "--Unknown--",
    /* 0x71 113 */ "--Unknown--",
    /* 0x72 114 */ "--Unknown--",
    /* 0x73 115 */ "--Unknown--",
    /* 0x74 116 */ "--Unknown--",
    /* 0x75 117 */ "--Unknown--",
    /* 0x76 118 */ "--Unknown--",
    /* 0x77 119 */ "--Unknown--",
    /* 0x78 120 */ "--Unknown--",
    /* 0x79 121 */ "--Unknown--",
    /* 0x7A 122 */ "--Unknown--",
    /* 0x7B 123 */ "--Unknown--",
    /* 0x7C 124 */ "--Unknown--",
    /* 0x7D 125 */ "--Unknown--",
    /* 0x7E 126 */ "--Unknown--",
    /* 0x7F 127 */ "--Unknown--",
    /* 0x80 128 */ "--Unknown--",
    /* 0x81 129 */ "--Unknown--",
    /* 0x82 130 */ "--Unknown--",
    /* 0x83 131 */ "--Unknown--",
    /* 0x84 132 */ "--Unknown--",
    /* 0x85 133 */ "--Unknown--",
    /* 0x86 134 */ "--Unknown--",
    /* 0x87 135 */ "--Unknown--",
    /* 0x88 136 */ "Unknown_136",
    /* 0x89 137 */ "Unknown_137",
    /* 0x8A 138 */ "Unknown_138",
    /* 0x8B 139 */ "Unknown_139",
    /* 0x8C 140 */ "Unknown_140",
    /* 0x8D 141 */ "Unknown_141",
    /* 0x8E 142 */ "Unknown_142",
    /* 0x8F 143 */ "--Unknown--",
    /* 0x90 144 */ "Unknown_144",
    /* 0x91 145 */ "Unknown_145",
    /* 0x92 146 */ "Unknown_146",
    /* 0x93 147 */ "Unknown_147",
    /* 0x94 148 */ "Unknown_148",
    /* 0x95 149 */ "Unknown_149",
    /* 0x96 150 */ "Unknown_150",
    /* 0x97 151 */ "--Unknown--",
    /* 0x98 152 */ "SetPhoneticMargin",
    /* 0x99 153 */ "--Unknown--",
    /* 0x9A 154 */ "--Unknown--",
    /* 0x9B 155 */ "--Unknown--",
    /* 0x9C 156 */ "Unknown_156",
    /* 0x9D 157 */ "Unknown_157",
    /* 0x9E 158 */ "--Unknown--",
    /* 0x9F 159 */ "--Unknown--",
    /* 0xA0 160 */ "--Unknown--",
    /* 0xA1 161 */ "--Unknown--",
    /* 0xA2 162 */ "--Unknown--",
    /* 0xA3 163 */ "--Unknown--",
    /* 0xA4 164 */ "--Unknown--",
    /* 0xA5 165 */ "--Unknown--",
    /* 0xA6 166 */ "--Unknown--",
    /* 0xA7 167 */ "--Unknown--",
    /* 0xA8 168 */ "--Unknown--",
    /* 0xA9 169 */ "--Unknown--",
    /* 0xAA 170 */ "--Unknown--",
    /* 0xAB 171 */ "--Unknown--",
    /* 0xAC 172 */ "--Unknown--",
    /* 0xAD 173 */ "--Unknown--",
    /* 0xAE 174 */ "--Unknown--",
    /* 0xAF 175 */ "--Unknown--",
    /* 0xB0 176 */ "--Unknown--",
    /* 0xB1 177 */ "--Unknown--",
    /* 0xB2 178 */ "--Unknown--",
    /* 0xB3 179 */ "--Unknown--",
    /* 0xB4 180 */ "--Unknown--",
    /* 0xB5 181 */ "--Unknown--",
    /* 0xB6 182 */ "--Unknown--",
    /* 0xB7 183 */ "--Unknown--",
    /* 0xB8 184 */ "Unknown_184",
    /* 0xB9 185 */ "--Unknown--",
    /* 0xBA 186 */ "Unknown_186",
    /* 0xBB 187 */ "--Unknown--",
    /* 0xBC 188 */ "--Unknown--",
    /* 0xBD 189 */ "--Unknown--",
    /* 0xBE 190 */ "--Unknown--",
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
    /* 0xD0 208 */ "--Unknown--",
    /* 0xD1 209 */ "--Unknown--",
    /* 0xD2 210 */ "--Unknown--",
    /* 0xD3 211 */ "--Unknown--",
    /* 0xD4 212 */ "--Unknown--",
    /* 0xD5 213 */ "--Unknown--",
    /* 0xD6 214 */ "--Unknown--",
    /* 0xD7 215 */ "--Unknown--",
    /* 0xD8 216 */ "--Unknown--",
    /* 0xD9 217 */ "--Unknown--",
    /* 0xDA 218 */ "--Unknown--",
    /* 0xDB 219 */ "--Unknown--",
    /* 0xDC 220 */ "--Unknown--",
    /* 0xDD 221 */ "--Unknown--",
    /* 0xDE 222 */ "--Unknown--",
    /* 0xDF 223 */ "--Unknown--",
    /* 0xE0 224 */ "--Unknown--",
    /* 0xE1 225 */ "--Unknown--",
    /* 0xE2 226 */ "--Unknown--",
    /* 0xE3 227 */ "--Unknown--",
    /* 0xE4 228 */ "--Unknown--",
    /* 0xE5 229 */ "--Unknown--",
    /* 0xE6 230 */ "--Unknown--",
    /* 0xE7 231 */ "--Unknown--",
    /* 0xE8 232 */ "--Unknown--",
    /* 0xE9 233 */ "--Unknown--",
    /* 0xEA 234 */ "--Unknown--",
    /* 0xEB 235 */ "--Unknown--",
    /* 0xEC 236 */ "--Unknown--",
    /* 0xED 237 */ "--Unknown--",
    /* 0xEE 238 */ "--Unknown--",
    /* 0xEF 239 */ "--Unknown--",
    /* 0xF0 240 */ "--Unknown--",
    /* 0xF1 241 */ "--Unknown--",
    /* 0xF2 242 */ "--Unknown--",
    /* 0xF3 243 */ "--Unknown--",
    /* 0xF4 244 */ "--Unknown--",
    /* 0xF5 245 */ "--Unknown--",
    /* 0xF6 246 */ "--Unknown--",
    /* 0xF7 247 */ "--Unknown--",
    /* 0xF8 248 */ "--Unknown--",
    /* 0xF9 249 */ "--Unknown--",
    /* 0xFA 250 */ "--Unknown--",
    /* 0xFB 251 */ "--Unknown--",
    /* 0xFC 252 */ "--Unknown--",
    /* 0xFD 253 */ "--Unknown--",
    /* 0xFE 254 */ "--Unknown--",
    /* 0xFF 255 */ "--Unknown--",
};

OpcodePtr_t OpcodesGrp1[256] = {
    /* 0x00   0 */ NULL,
    /* 0x01   1 */ NULL,
    /* 0x02   2 */ NULL,
    /* 0x03   3 */ NULL,
    /* 0x04   4 */ NULL,
    /* 0x05   5 */ NULL,
    /* 0x06   6 */ NULL,
    /* 0x07   7 */ NULL,
    /* 0x08   8 */ NULL,
    /* 0x09   9 */ NULL,
    /* 0x0A  10 */ NULL,
    /* 0x0B  11 */ NULL,
    /* 0x0C  12 */ NULL,
    /* 0x0D  13 */ NULL,
    /* 0x0E  14 */ NULL,
    /* 0x0F  15 */ NULL,
    /* 0x10  16 */ Opcode_Grp1_Unknown_16,
    /* 0x11  17 */ Opcode_Grp1_Unknown_17,
    /* 0x12  18 */ Opcode_Grp1_Unknown_18,
    /* 0x13  19 */ Opcode_Grp1_Unknown_19,
    /* 0x14  20 */ Opcode_Grp1_Unknown_20,
    /* 0x15  21 */ Opcode_Grp1_Unknown_21,
    /* 0x16  22 */ NULL,
    /* 0x17  23 */ NULL,
    /* 0x18  24 */ Opcode_Grp1_Unknown_24,
    /* 0x19  25 */ Opcode_Grp1_Unknown_25,
    /* 0x1A  26 */ NULL,
    /* 0x1B  27 */ NULL,
    /* 0x1C  28 */ Opcode_Grp1_Unknown_28,
    /* 0x1D  29 */ NULL,
    /* 0x1E  30 */ Opcode_Grp1_Unknown_30,
    /* 0x1F  31 */ Opcode_Grp1_Unknown_31,
    /* 0x20  32 */ NULL,
    /* 0x21  33 */ NULL,
    /* 0x22  34 */ NULL,
    /* 0x23  35 */ NULL,
    /* 0x24  36 */ NULL,
    /* 0x25  37 */ NULL,
    /* 0x26  38 */ NULL,
    /* 0x27  39 */ NULL,
    /* 0x28  40 */ NULL,
    /* 0x29  41 */ NULL,
    /* 0x2A  42 */ NULL,
    /* 0x2B  43 */ NULL,
    /* 0x2C  44 */ NULL,
    /* 0x2D  45 */ NULL,
    /* 0x2E  46 */ NULL,
    /* 0x2F  47 */ NULL,
    /* 0x30  48 */ NULL,
    /* 0x31  49 */ NULL,
    /* 0x32  50 */ NULL,
    /* 0x33  51 */ Opcode_Grp1_Unknown_51,
    /* 0x34  52 */ NULL,
    /* 0x35  53 */ NULL,
    /* 0x36  54 */ NULL,
    /* 0x37  55 */ NULL,
    /* 0x38  56 */ NULL,
    /* 0x39  57 */ NULL,
    /* 0x3A  58 */ NULL,
    /* 0x3B  59 */ NULL,
    /* 0x3C  60 */ NULL,
    /* 0x3D  61 */ NULL,
    /* 0x3E  62 */ NULL,
    /* 0x3F  63 */ NULL,
    /* 0x40  64 */ Opcode_Grp1_Unknown_64,
    /* 0x41  65 */ Opcode_Grp1_Unknown_65,
    /* 0x42  66 */ Opcode_Grp1_Unknown_66,
    /* 0x43  67 */ Opcode_Grp1_Unknown_67,
    /* 0x44  68 */ Opcode_Grp1_Unknown_68,
    /* 0x45  69 */ Opcode_Grp1_Unknown_69,
    /* 0x46  70 */ Opcode_Grp1_Unknown_70,
    /* 0x47  71 */ Opcode_Grp1_Unknown_71,
    /* 0x48  72 */ Opcode_Grp1_Unknown_72,
    /* 0x49  73 */ Opcode_Grp1_Unknown_73,
    /* 0x4A  74 */ Opcode_Grp1_Unknown_74,
    /* 0x4B  75 */ NULL,
    /* 0x4C  76 */ NULL,
    /* 0x4D  77 */ NULL,
    /* 0x4E  78 */ NULL,
    /* 0x4F  79 */ NULL,
    /* 0x50  80 */ NULL,
    /* 0x51  81 */ NULL,
    /* 0x52  82 */ NULL,
    /* 0x53  83 */ NULL,
    /* 0x54  84 */ NULL,
    /* 0x55  85 */ NULL,
    /* 0x56  86 */ NULL,
    /* 0x57  87 */ NULL,
    /* 0x58  88 */ NULL,
    /* 0x59  89 */ NULL,
    /* 0x5A  90 */ NULL,
    /* 0x5B  91 */ NULL,
    /* 0x5C  92 */ NULL,
    /* 0x5D  93 */ NULL,
    /* 0x5E  94 */ NULL,
    /* 0x5F  95 */ NULL,
    /* 0x60  96 */ Opcode_Grp1_Unknown_96,
    /* 0x61  97 */ Opcode_Grp1_Unknown_97,
    /* 0x62  98 */ NULL,
    /* 0x63  99 */ NULL,
    /* 0x64 100 */ Opcode_Grp1_Unknown_100,
    /* 0x65 101 */ Opcode_Grp1_Unknown_101,
    /* 0x66 102 */ Opcode_Grp1_Unknown_102,
    /* 0x67 103 */ Opcode_Grp1_Unknown_103,
    /* 0x68 104 */ Opcode_Grp1_Unknown_104,
    /* 0x69 105 */ NULL,
    /* 0x6A 106 */ NULL,
    /* 0x6B 107 */ NULL,
    /* 0x6C 108 */ NULL,
    /* 0x6D 109 */ NULL,
    /* 0x6E 110 */ NULL,
    /* 0x6F 111 */ NULL,
    /* 0x70 112 */ NULL,
    /* 0x71 113 */ NULL,
    /* 0x72 114 */ NULL,
    /* 0x73 115 */ NULL,
    /* 0x74 116 */ NULL,
    /* 0x75 117 */ NULL,
    /* 0x76 118 */ NULL,
    /* 0x77 119 */ NULL,
    /* 0x78 120 */ NULL,
    /* 0x79 121 */ NULL,
    /* 0x7A 122 */ NULL,
    /* 0x7B 123 */ NULL,
    /* 0x7C 124 */ NULL,
    /* 0x7D 125 */ NULL,
    /* 0x7E 126 */ NULL,
    /* 0x7F 127 */ NULL,
    /* 0x80 128 */ NULL,
    /* 0x81 129 */ NULL,
    /* 0x82 130 */ NULL,
    /* 0x83 131 */ NULL,
    /* 0x84 132 */ NULL,
    /* 0x85 133 */ NULL,
    /* 0x86 134 */ NULL,
    /* 0x87 135 */ NULL,
    /* 0x88 136 */ Opcode_Grp1_Unknown_136,
    /* 0x89 137 */ Opcode_Grp1_Unknown_137,
    /* 0x8A 138 */ Opcode_Grp1_Unknown_138,
    /* 0x8B 139 */ Opcode_Grp1_Unknown_139,
    /* 0x8C 140 */ Opcode_Grp1_Unknown_140,
    /* 0x8D 141 */ Opcode_Grp1_Unknown_141,
    /* 0x8E 142 */ Opcode_Grp1_Unknown_142,
    /* 0x8F 143 */ NULL,
    /* 0x90 144 */ Opcode_Grp1_Unknown_144,
    /* 0x91 145 */ Opcode_Grp1_Unknown_145,
    /* 0x92 146 */ Opcode_Grp1_Unknown_146,
    /* 0x93 147 */ Opcode_Grp1_Unknown_147,
    /* 0x94 148 */ Opcode_Grp1_Unknown_148,
    /* 0x95 149 */ Opcode_Grp1_Unknown_149,
    /* 0x96 150 */ Opcode_Grp1_Unknown_150,
    /* 0x97 151 */ NULL,
    /* 0x98 152 */ Opcode_Grp1_SetPhoneticMargin,
    /* 0x99 153 */ NULL,
    /* 0x9A 154 */ NULL,
    /* 0x9B 155 */ NULL,
    /* 0x9C 156 */ Opcode_Grp1_Unknown_156,
    /* 0x9D 157 */ Opcode_Grp1_Unknown_157,
    /* 0x9E 158 */ NULL,
    /* 0x9F 159 */ NULL,
    /* 0xA0 160 */ NULL,
    /* 0xA1 161 */ NULL,
    /* 0xA2 162 */ NULL,
    /* 0xA3 163 */ NULL,
    /* 0xA4 164 */ NULL,
    /* 0xA5 165 */ NULL,
    /* 0xA6 166 */ NULL,
    /* 0xA7 167 */ NULL,
    /* 0xA8 168 */ NULL,
    /* 0xA9 169 */ NULL,
    /* 0xAA 170 */ NULL,
    /* 0xAB 171 */ NULL,
    /* 0xAC 172 */ NULL,
    /* 0xAD 173 */ NULL,
    /* 0xAE 174 */ NULL,
    /* 0xAF 175 */ NULL,
    /* 0xB0 176 */ NULL,
    /* 0xB1 177 */ NULL,
    /* 0xB2 178 */ NULL,
    /* 0xB3 179 */ NULL,
    /* 0xB4 180 */ NULL,
    /* 0xB5 181 */ NULL,
    /* 0xB6 182 */ NULL,
    /* 0xB7 183 */ NULL,
    /* 0xB8 184 */ Opcode_Grp1_Unknown_184,
    /* 0xB9 185 */ NULL,
    /* 0xBA 186 */ Opcode_Grp1_Unknown_186,
    /* 0xBB 187 */ NULL,
    /* 0xBC 188 */ NULL,
    /* 0xBD 189 */ NULL,
    /* 0xBE 190 */ NULL,
    /* 0xBF 191 */ Opcode_Grp1_Unknown_191,
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
    /* 0xD0 208 */ NULL,
    /* 0xD1 209 */ NULL,
    /* 0xD2 210 */ NULL,
    /* 0xD3 211 */ NULL,
    /* 0xD4 212 */ NULL,
    /* 0xD5 213 */ NULL,
    /* 0xD6 214 */ NULL,
    /* 0xD7 215 */ NULL,
    /* 0xD8 216 */ NULL,
    /* 0xD9 217 */ NULL,
    /* 0xDA 218 */ NULL,
    /* 0xDB 219 */ NULL,
    /* 0xDC 220 */ NULL,
    /* 0xDD 221 */ NULL,
    /* 0xDE 222 */ NULL,
    /* 0xDF 223 */ NULL,
    /* 0xE0 224 */ NULL,
    /* 0xE1 225 */ NULL,
    /* 0xE2 226 */ NULL,
    /* 0xE3 227 */ NULL,
    /* 0xE4 228 */ NULL,
    /* 0xE5 229 */ NULL,
    /* 0xE6 230 */ NULL,
    /* 0xE7 231 */ NULL,
    /* 0xE8 232 */ NULL,
    /* 0xE9 233 */ NULL,
    /* 0xEA 234 */ NULL,
    /* 0xEB 235 */ NULL,
    /* 0xEC 236 */ NULL,
    /* 0xED 237 */ NULL,
    /* 0xEE 238 */ NULL,
    /* 0xEF 239 */ NULL,
    /* 0xF0 240 */ NULL,
    /* 0xF1 241 */ NULL,
    /* 0xF2 242 */ NULL,
    /* 0xF3 243 */ NULL,
    /* 0xF4 244 */ NULL,
    /* 0xF5 245 */ NULL,
    /* 0xF6 246 */ NULL,
    /* 0xF7 247 */ NULL,
    /* 0xF8 248 */ NULL,
    /* 0xF9 249 */ NULL,
    /* 0xFA 250 */ NULL,
    /* 0xFB 251 */ NULL,
    /* 0xFC 252 */ NULL,
    /* 0xFD 253 */ NULL,
    /* 0xFE 254 */ NULL,
    /* 0xFF 255 */ NULL,
};

uint32_t Opcode_Grp1_Unknown_16(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_17(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_18(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_19(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_20(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_21(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_24(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_25(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_28(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_30(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_31(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_51(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_64(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_65(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_66(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_67(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_68(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_69(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_70(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_71(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_72(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_73(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_74(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_96(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_97(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_100(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_101(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_102(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_103(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_104(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_136(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_137(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_138(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_139(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_140(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_141(Thread_t* thread)
{
    uint32_t value1 = Thread_PopStack(thread);
    uint32_t value2 = Thread_PopStack(thread);
    uint32_t value3 = Thread_PopStack(thread);
    uint32_t value4 = Thread_PopStack(thread);
    uint32_t value5 = Thread_PopStack(thread);
    uint32_t value6 = Thread_PopStack(thread);
    printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
    return 0;
}

uint32_t Opcode_Grp1_Unknown_142(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_144(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_145(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_146(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_147(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_148(Thread_t* thread)
{
    uint8_t* ptr1 = Thread_PopAndResolveAddress(thread);
    uint8_t* ptr2 = Thread_PopAndResolveAddress(thread);
    printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
    return 0;
}

uint32_t Opcode_Grp1_Unknown_149(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_150(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_SetPhoneticMargin(Thread_t* thread)
{
	uint32_t value1 = Thread_PopStack(thread);
	uint32_t value2 = Thread_PopStack(thread);
	uint32_t value3 = Thread_PopStack(thread);
	uint32_t value4 = Thread_PopStack(thread);
	uint32_t value5 = Thread_PopStack(thread);
	//uint32_t value6 = Thread_PopStack(thread);
	printf("[Thread %d]: %sWarning: dummy opcode\n", thread->threadId, TLevel[thread->level]);
	return 0;
}

uint32_t Opcode_Grp1_Unknown_156(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_157(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_184(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_186(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

uint32_t Opcode_Grp1_Unknown_191(Thread_t* thread)
{
	return 0xFFFFFFFF;
}

