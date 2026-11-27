/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * os_gl.c - the picture shown through OpenGL (os_gl.h)
 *
 * No header of OpenGL is included: the few types and numbers that are
 * needed are below, and every function is asked for by its name from
 * the back end's loader, so that the program builds and runs on a system
 * without OpenGL.
 *
 * The same code draws through OpenGL ES 3.0 and later, which a context
 * says of itself in its version string (any that a back end hands over):
 * the shaders then begin with ES's
 * version line, the picture's pixels go into the texture in the one byte
 * order ES takes, with red and blue changed over when the texture is read,
 * and a shader chain is there only where the context can draw into
 * textures of floats.
 *
 * The picture is one texture of 8 bits a colour, written row by row as
 * the frames change it (rows from the top, as the engine has them: a
 * texture's first row is the picture's top one everywhere in here, and
 * only the last draw, into the window, turns it over).  A shader chain
 * runs as a plan of passes (os_glhook.c) into textures of 16-bit floats;
 * the plan and its textures are kept until the picture's size, the size
 * it is shown at or the chain is another, the compiled passes until the
 * end.  A chain that does not compile or cannot be set up is given up
 * for the run, and the picture is drawn bicubically instead.
 */
#include "host/os_gl.h"
#include "host/os_glhook.h"

#ifdef BGI_GL

#include "bgi/deflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BGI_WIN32
#include "host/os_win32.h"
#define GLCALL WINAPI
#else
#define GLCALL
#endif

// ---- OpenGL as far as it is used -----------------------------------------------------------------

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;

#define GL_ONE                        1
#define GL_SRC_ALPHA                  0x0302
#define GL_ONE_MINUS_SRC_ALPHA        0x0303
#define GL_BLEND                      0x0BE2
#define GL_TRIANGLES                  0x0004
#define GL_UNPACK_ROW_LENGTH          0x0CF2
#define GL_UNPACK_ALIGNMENT           0x0CF5
#define GL_MAX_TEXTURE_SIZE           0x0D33
#define GL_TEXTURE_2D                 0x0DE1
#define GL_UNSIGNED_BYTE              0x1401
#define GL_HALF_FLOAT                 0x140B
#define GL_RED                        0x1903
#define GL_BLUE                       0x1905
#define GL_RGBA                       0x1908
#define GL_RENDERER                   0x1F01
#define GL_VERSION                    0x1F02
#define GL_EXTENSIONS                 0x1F03
#define GL_NEAREST                    0x2600
#define GL_LINEAR                     0x2601
#define GL_LINEAR_MIPMAP_LINEAR       0x2703
#define GL_TEXTURE_MAG_FILTER         0x2800
#define GL_TEXTURE_MIN_FILTER         0x2801
#define GL_TEXTURE_WRAP_S             0x2802
#define GL_TEXTURE_WRAP_T             0x2803
#define GL_COLOR_BUFFER_BIT           0x4000
#define GL_RGB8                       0x8051
#define GL_RGBA8                      0x8058
#define GL_BGRA                       0x80E1
#define GL_CLAMP_TO_EDGE              0x812F
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_UNSIGNED_INT_8_8_8_8_REV   0x8367
#define GL_TEXTURE0                   0x84C0
#define GL_MAX_TEXTURE_IMAGE_UNITS    0x8872
#define GL_RGBA16F                    0x881A
#define GL_FRAGMENT_SHADER            0x8B30
#define GL_VERTEX_SHADER              0x8B31
#define GL_COMPILE_STATUS             0x8B81
#define GL_LINK_STATUS                0x8B82
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_FRAMEBUFFER                0x8D40
#define GL_TEXTURE_SWIZZLE_R          0x8E42
#define GL_TEXTURE_SWIZZLE_B          0x8E44
#define GL_TEXTURE_SWIZZLE_A          0x8E45

static struct
{
	const GLubyte*(GLCALL* GetString)(GLenum);
	GLenum(GLCALL* GetError)(void);
	void(GLCALL* GetIntegerv)(GLenum, GLint*);
	void(GLCALL* Viewport)(GLint, GLint, GLsizei, GLsizei);
	void(GLCALL* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void(GLCALL* Clear)(GLbitfield);
	void(GLCALL* PixelStorei)(GLenum, GLint);
	void(GLCALL* GenTextures)(GLsizei, GLuint*);
	void(GLCALL* DeleteTextures)(GLsizei, const GLuint*);
	void(GLCALL* BindTexture)(GLenum, GLuint);
	void(GLCALL* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
	void(GLCALL* TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
	void(GLCALL* TexParameteri)(GLenum, GLenum, GLint);
	void(GLCALL* DrawArrays)(GLenum, GLint, GLsizei);
	void(GLCALL* ActiveTexture)(GLenum);
	void(GLCALL* GenerateMipmap)(GLenum);
	void(GLCALL* GenFramebuffers)(GLsizei, GLuint*);
	void(GLCALL* DeleteFramebuffers)(GLsizei, const GLuint*);
	void(GLCALL* BindFramebuffer)(GLenum, GLuint);
	void(GLCALL* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	GLenum(GLCALL* CheckFramebufferStatus)(GLenum);
	void(GLCALL* GenVertexArrays)(GLsizei, GLuint*);
	void(GLCALL* DeleteVertexArrays)(GLsizei, const GLuint*);
	void(GLCALL* BindVertexArray)(GLuint);
	GLuint(GLCALL* CreateShader)(GLenum);
	void(GLCALL* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
	void(GLCALL* CompileShader)(GLuint);
	void(GLCALL* GetShaderiv)(GLuint, GLenum, GLint*);
	void(GLCALL* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
	void(GLCALL* DeleteShader)(GLuint);
	GLuint(GLCALL* CreateProgram)(void);
	void(GLCALL* AttachShader)(GLuint, GLuint);
	void(GLCALL* LinkProgram)(GLuint);
	void(GLCALL* GetProgramiv)(GLuint, GLenum, GLint*);
	void(GLCALL* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
	void(GLCALL* UseProgram)(GLuint);
	void(GLCALL* DeleteProgram)(GLuint);
	GLint(GLCALL* GetUniformLocation)(GLuint, const GLchar*);
	void(GLCALL* Uniform1i)(GLint, GLint);
	void(GLCALL* Uniform1f)(GLint, GLfloat);
	void(GLCALL* Uniform2f)(GLint, GLfloat, GLfloat);
	void(GLCALL* Enable)(GLenum);
	void(GLCALL* Disable)(GLenum);
	void(GLCALL* BlendFunc)(GLenum, GLenum);
} gl;

// the functions' names, in the order of the members above
static const char* const kNames[] = {"glGetString", "glGetError", "glGetIntegerv", "glViewport", "glClearColor", "glClear", "glPixelStorei",
	"glGenTextures", "glDeleteTextures", "glBindTexture", "glTexImage2D", "glTexSubImage2D", "glTexParameteri", "glDrawArrays", "glActiveTexture",
	"glGenerateMipmap", "glGenFramebuffers", "glDeleteFramebuffers", "glBindFramebuffer", "glFramebufferTexture2D", "glCheckFramebufferStatus",
	"glGenVertexArrays", "glDeleteVertexArrays", "glBindVertexArray", "glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv",
	"glGetShaderInfoLog", "glDeleteShader", "glCreateProgram", "glAttachShader", "glLinkProgram", "glGetProgramiv", "glGetProgramInfoLog",
	"glUseProgram", "glDeleteProgram", "glGetUniformLocation", "glUniform1i", "glUniform1f", "glUniform2f", "glEnable", "glDisable",
	"glBlendFunc"};

// ---- the shader files the program carries ----------------------------------------------------------

#define FILES       14
#define CHAINS      12 // six modes, each with the smaller and the larger networks
#define CHAIN_FILES 8

enum
{
	F_CLAMP,
	F_DOWN2,
	F_DOWN4,
	F_RESTORE_S,
	F_RESTORE_M,
	F_RESTORE_VL,
	F_SOFT_S,
	F_SOFT_M,
	F_SOFT_VL,
	F_UP_S,
	F_UP_M,
	F_UP_VL,
	F_UPDENOISE_M,
	F_UPDENOISE_VL
};

static const char* const kFiles[FILES] = {"Anime4K_Clamp_Highlights.glsl", "Anime4K_AutoDownscalePre_x2.glsl",
	"Anime4K_AutoDownscalePre_x4.glsl", "Anime4K_Restore_CNN_S.glsl", "Anime4K_Restore_CNN_M.glsl", "Anime4K_Restore_CNN_VL.glsl",
	"Anime4K_Restore_CNN_Soft_S.glsl", "Anime4K_Restore_CNN_Soft_M.glsl", "Anime4K_Restore_CNN_Soft_VL.glsl", "Anime4K_Upscale_CNN_x2_S.glsl",
	"Anime4K_Upscale_CNN_x2_M.glsl", "Anime4K_Upscale_CNN_x2_VL.glsl", "Anime4K_Upscale_Denoise_CNN_x2_M.glsl",
	"Anime4K_Upscale_Denoise_CNN_x2_VL.glsl"};

/* The modes as Anime4K's own instructions chain the files for mpv: the
 * set for a slower graphics processor ("Fast") and the one for a faster
 * ("HQ"), filter n at 2 n and 2 n + 1. */
static const signed char kChains[CHAINS][CHAIN_FILES] = {
	{F_CLAMP, F_RESTORE_M, F_UP_M, F_DOWN2, F_DOWN4, F_UP_S, -1},                // A
	{F_CLAMP, F_RESTORE_VL, F_UP_VL, F_DOWN2, F_DOWN4, F_UP_M, -1},              //
	{F_CLAMP, F_SOFT_M, F_UP_M, F_DOWN2, F_DOWN4, F_UP_S, -1},                   // B
	{F_CLAMP, F_SOFT_VL, F_UP_VL, F_DOWN2, F_DOWN4, F_UP_M, -1},                 //
	{F_CLAMP, F_UPDENOISE_M, F_DOWN2, F_DOWN4, F_UP_S, -1},                      // C
	{F_CLAMP, F_UPDENOISE_VL, F_DOWN2, F_DOWN4, F_UP_M, -1},                     //
	{F_CLAMP, F_RESTORE_M, F_UP_M, F_RESTORE_S, F_DOWN2, F_DOWN4, F_UP_S, -1},   // A+A
	{F_CLAMP, F_RESTORE_VL, F_UP_VL, F_RESTORE_M, F_DOWN2, F_DOWN4, F_UP_M, -1}, //
	{F_CLAMP, F_SOFT_M, F_UP_M, F_DOWN2, F_DOWN4, F_SOFT_S, F_UP_S, -1},         // B+B
	{F_CLAMP, F_SOFT_VL, F_UP_VL, F_DOWN2, F_DOWN4, F_SOFT_M, F_UP_M, -1},       //
	{F_CLAMP, F_UPDENOISE_M, F_DOWN2, F_DOWN4, F_RESTORE_S, F_UP_S, -1},         // C+A
	{F_CLAMP, F_UPDENOISE_VL, F_DOWN2, F_DOWN4, F_RESTORE_M, F_UP_M, -1},        //
};

static const char* const kFilters[CHAINS / 2] = {"Anime4K A", "Anime4K B", "Anime4K C", "Anime4K A+A", "Anime4K B+B", "Anime4K C+A"};

static const unsigned char* gPack; // OS_UseShaders
static unsigned int gPackSize;

// a compiled pass
typedef struct Program
{
	GLuint id;                 // 0: not made yet
	int failed;                // .. and cannot be
	int units;                 // the textures it reads
	int bind[OS_GLHOOK_BINDS]; // which of the pass's BIND lines each unit takes its texture from
	GLint size[OS_GLHOOK_BINDS], pt[OS_GLHOOK_BINDS];
} Program_t;

// a shader file: its text out of the pack, its passes, what was compiled of them
typedef struct File
{
	char* text; // NULL: not read yet
	int passes; // -1: not there, or not understood
	OsGlPass_t pass[OS_GLHOOK_PASSES];
	Program_t prog[OS_GLHOOK_PASSES];
} File_t;

static File_t* gFile[FILES];

void OS_UseShaders(const unsigned char* pack, unsigned int size)
{
	gPack = pack;
	gPackSize = size;
}

static unsigned Le32(const unsigned char* p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

// a file of the pack by its name, as text with a NUL after it (free() it); NULL when it is not there
static char* PackText(const char* name, size_t* size)
{
	unsigned at = 12, count, i;
	size_t want = strlen(name);
	if(!gPack || gPackSize < 12 || memcmp(gPack, "OBGIPACK", 8) != 0)
		return NULL;
	count = Le32(gPack + 8);
	for(i = 0; i < count; i++)
	{
		unsigned nameLen, method, raw, packed;
		const unsigned char* entry;
		if(at + 1 > gPackSize || (nameLen = gPack[at]) == 0 || at + 1 + nameLen + 9 > gPackSize)
			return NULL;
		entry = gPack + at + 1;
		at += 1 + nameLen;
		method = gPack[at];
		raw = Le32(gPack + at + 1);
		packed = Le32(gPack + at + 5);
		at += 9;
		if(packed > gPackSize - at)
			return NULL;
		if(nameLen == want && memcmp(entry, name, want) == 0)
		{
			char* text = NULL;
			if(method == 8)
			{
				size_t got = 0;
				unsigned char* d = Deflate_InflateRaw(gPack + at, packed, raw, &got);
				if(d && got == raw && (text = (char*)malloc((size_t)raw + 1)) != NULL)
					memcpy(text, d, raw);
				free(d);
			}
			else if(method == 0 && packed == raw && (text = (char*)malloc((size_t)raw + 1)) != NULL)
				memcpy(text, gPack + at, raw);
			if(text)
			{
				text[raw] = 0;
				*size = raw;
			}
			return text;
		}
		at += packed;
	}
	return NULL;
}

// file `f`, read and taken apart at its first use; NULL when it cannot be had
static File_t* FileOf(int f)
{
	size_t size = 0;
	if(!gFile[f])
	{
		gFile[f] = (File_t*)calloc(1, sizeof *gFile[f]);
		if(!gFile[f])
			return NULL;
		gFile[f]->passes = -1;
		gFile[f]->text = PackText(kFiles[f], &size);
		if(gFile[f]->text)
			gFile[f]->passes = OsGlHook_Parse(gFile[f]->text, size, gFile[f]->pass, OS_GLHOOK_PASSES);
	}
	return gFile[f]->passes > 0 ? gFile[f] : NULL;
}

// ---- the state -----------------------------------------------------------------------------------

static int gOn;     // OsGl_Start took the context
static int gOld;    // GLSL 1.30 (OpenGL 3.0 .. 3.2); else 3.30
static int gEs;     // the context is OpenGL ES's (3.0 or later): GLSL ES 3.00
static int gFloats; // a pass can draw into a texture of floats: the shader chains are to be had
static int gMaxSize, gMaxUnits;
static char gRenderer[128];
static GLuint gVao, gFbo;
static GLuint gPic;          // the picture's texture
static int gPicW, gPicH;     // its size; 0: none yet
static GLuint gCopy, gCubic; // the two programs of the last draw
static GLint gCopyFlip, gCubicFlip, gCubicSize;
static GLuint gLayer;        // the layer's texture (OsGl_Layer),
static int gLayerW, gLayerH; // .. its size,
static GLuint gOver;         // .. and the program that draws it
static GLint gOverFlip;
// the plan that is set up, and its textures (slot 0 is gPic)
static int gPlanChain = -1, gPlanPicW, gPlanPicH, gPlanOutW, gPlanOutH;
static OsGlPlan_t gPlan;
static GLuint gSlot[OS_GLPLAN_SLOTS];
static int gChainFailed[CHAINS]; // given up for the run

int OsGl_Wanted(void)
{
	const char* e = getenv("BGI_GL");
	if(e && strcmp(e, "off") == 0)
		return OS_GL_OFF;
	if(e && strcmp(e, "old") == 0)
		return OS_GL_OLD;
	return e && strcmp(e, "force") == 0 ? OS_GL_FORCE : OS_GL_AUTO;
}

int OsGl_On(void)
{
	return gOn;
}

// ---- programs --------------------------------------------------------------------------------------

/* Three corners that cover the target; `uv` runs from (0, 0) to (1, 1)
 * over it, its first row at the bottom of what is drawn into - which for
 * a texture is its first row too - unless `flip` turns it over. */
static const char kVertex[] = "uniform float flip;\n"
							  "out vec2 uv;\n"
							  "void main() {\n"
							  "\tvec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
							  "\tuv = p;\n"
							  "\tgl_Position = vec4(p.x * 2.0 - 1.0, (p.y * 2.0 - 1.0) * flip, 0.0, 1.0);\n"
							  "}\n";

static const char kCopy[] = "in vec2 uv;\n"
							"out vec4 out_color;\n"
							"uniform sampler2D tex;\n"
							"void main() { out_color = vec4(texture(tex, uv).rgb, 1.0); }\n";

// the layer: its colours with its alpha, which the blending takes
static const char kOver[] = "in vec2 uv;\n"
							"out vec4 out_color;\n"
							"uniform sampler2D tex;\n"
							"void main() { out_color = texture(tex, uv); }\n";

// Catmull-Rom over four pixels each way, read at their centres
static const char kCubic[] = "in vec2 uv;\n"
							 "out vec4 out_color;\n"
							 "uniform sampler2D tex;\n"
							 "uniform vec2 size;\n"
							 "vec4 weights(float t) {\n"
							 "\tfloat t2 = t * t, t3 = t2 * t;\n"
							 "\treturn vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);\n"
							 "}\n"
							 "void main() {\n"
							 "\tvec2 p = uv * size - 0.5;\n"
							 "\tvec2 i = floor(p);\n"
							 "\tvec4 wx = weights(p.x - i.x), wy = weights(p.y - i.y);\n"
							 "\tvec3 c = vec3(0.0);\n"
							 "\tfor(int y = 0; y < 4; y++) {\n"
							 "\t\tvec3 row = vec3(0.0);\n"
							 "\t\tfor(int x = 0; x < 4; x++)\n"
							 "\t\t\trow += wx[x] * texture(tex, (i + vec2(float(x) - 0.5, float(y) - 0.5)) / size).rgb;\n"
							 "\t\tc += wy[y] * row;\n"
							 "\t}\n"
							 "\tout_color = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
							 "}\n";

/* The line every shader begins with.  OpenGL ES has no precision of its
 * own for a fragment shader's numbers and reads a texture at the lowest
 * unless told: both are the highest, which every ES 3.0 has. */
static const char* Version(void)
{
	if(gEs)
		return "#version 300 es\nprecision highp float;\nprecision highp sampler2D;\n";
	return gOld ? "#version 130\n" : "#version 330 core\n";
}

// a shader of `n` pieces; 0 when it does not compile (said on stderr with `what`)
static GLuint Shader(GLenum kind, const char* const* piece, const GLint* len, int n, const char* what)
{
	GLuint s = gl.CreateShader(kind);
	GLint ok = 0;
	if(!s)
		return 0;
	gl.ShaderSource(s, n, piece, len);
	gl.CompileShader(s);
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if(!ok)
	{
		char log[600];
		GLsizei got = 0;
		log[0] = 0;
		gl.GetShaderInfoLog(s, (GLsizei)sizeof log - 1, &got, log);
		log[got > 0 && got < (GLsizei)sizeof log ? got : 0] = 0;
		fprintf(stderr, "openbgi: OpenGL: the shader \"%s\" does not compile: %s\n", what, log);
		gl.DeleteShader(s);
		return 0;
	}
	return s;
}

// a program of the common vertex shader and a fragment shader of `n` pieces; 0 when it cannot be made
static GLuint Program(const char* const* piece, const GLint* len, int n, const char* what)
{
	const char* vs[2];
	GLuint v, f, p;
	GLint ok = 0;
	vs[0] = Version();
	vs[1] = kVertex;
	v = Shader(GL_VERTEX_SHADER, vs, NULL, 2, "corners");
	f = v ? Shader(GL_FRAGMENT_SHADER, piece, len, n, what) : 0;
	p = f ? gl.CreateProgram() : 0;
	if(p)
	{
		gl.AttachShader(p, v);
		gl.AttachShader(p, f);
		gl.LinkProgram(p);
		gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
		if(!ok)
		{
			fprintf(stderr, "openbgi: OpenGL: the shader \"%s\" does not link\n", what);
			gl.DeleteProgram(p);
			p = 0;
		}
	}
	if(v)
		gl.DeleteShader(v);
	if(f)
		gl.DeleteShader(f);
	return p;
}

static GLuint Simple(const char* body, const char* what)
{
	const char* piece[2];
	piece[0] = Version();
	piece[1] = body;
	return Program(piece, NULL, 2, what);
}

/* The program of a pass.  Before the pass's own code go, for every
 * texture it binds, what mpv gives a user shader: NAME_raw (the sampler),
 * NAME_pos (where the pixel being drawn lies in it), NAME_size, NAME_pt
 * (a pixel's step), NAME_tex(pos) and NAME_texOff(offset); HOOKED is
 * another name for the texture the pass hooks.  Every texture here covers
 * the whole picture, so the place is the same in all of them. */
static Program_t* PassProgram(File_t* file, int index)
{
	const OsGlPass_t* p = &file->pass[index];
	Program_t* g = &file->prog[index];
	const char* hooked = p->hook == OS_GLHOOK_PREKERNEL ? "PREKERNEL" : "MAIN";
	char real[OS_GLHOOK_BINDS][OS_GLHOOK_NAME];
	char head[OS_GLHOOK_BINDS * 420 + 512];
	const char* piece[4];
	GLint len[4];
	size_t at = 0;
	int i, k, alias = 0;
	if(g->id || g->failed)
		return g->id ? g : NULL;
	g->failed = 1;
	at += (size_t)snprintf(head + at, sizeof head - at, "in vec2 uv;\nout vec4 out_color;\n");
	for(i = 0; i < p->binds; i++)
	{
		const char* name = strcmp(p->bind[i], "HOOKED") == 0 ? hooked : p->bind[i];
		if(strcmp(p->bind[i], "HOOKED") == 0)
			alias = 1;
		if(at >= sizeof head - 600)
			return NULL;
		for(k = 0; k < g->units && strcmp(real[k], name) != 0; k++)
		{
		}
		if(k < g->units)
			continue; // (bound under both of its names)
		snprintf(real[g->units], OS_GLHOOK_NAME, "%s", name);
		g->bind[g->units++] = i;
		at += (size_t)snprintf(head + at, sizeof head - at,
			"uniform sampler2D %s_raw;\nuniform vec2 %s_size;\nuniform vec2 %s_pt;\n"
			"#define %s_pos uv\n#define %s_tex(pos) texture(%s_raw, pos)\n#define %s_texOff(off) %s_tex(%s_pos + %s_pt * vec2(off))\n",
			name, name, name, name, name, name, name, name, name, name);
	}
	if(alias && at < sizeof head - 400)
		at += (size_t)snprintf(head + at, sizeof head - at,
			"#define HOOKED_raw %s_raw\n#define HOOKED_pos %s_pos\n#define HOOKED_size %s_size\n#define HOOKED_pt %s_pt\n"
			"#define HOOKED_tex(pos) %s_tex(pos)\n#define HOOKED_texOff(off) %s_texOff(off)\n",
			hooked, hooked, hooked, hooked, hooked, hooked);
	if(at >= sizeof head - 1 || g->units > gMaxUnits)
		return NULL;
	piece[0] = Version();
	len[0] = (GLint)strlen(piece[0]);
	piece[1] = head;
	len[1] = (GLint)at;
	piece[2] = p->body;
	len[2] = (GLint)p->bodyLen;
	piece[3] = "\nvoid main() { out_color = hook(); }\n";
	len[3] = (GLint)strlen(piece[3]);
	g->id = Program(piece, len, 4, p->desc[0] ? p->desc : "a pass");
	if(!g->id)
		return NULL;
	gl.UseProgram(g->id);
	gl.Uniform1f(gl.GetUniformLocation(g->id, "flip"), 1.0f);
	for(k = 0; k < g->units; k++)
	{
		char name[OS_GLHOOK_NAME + 8];
		snprintf(name, sizeof name, "%s_raw", real[k]);
		gl.Uniform1i(gl.GetUniformLocation(g->id, name), k);
		snprintf(name, sizeof name, "%s_size", real[k]);
		g->size[k] = gl.GetUniformLocation(g->id, name);
		snprintf(name, sizeof name, "%s_pt", real[k]);
		g->pt[k] = gl.GetUniformLocation(g->id, name);
	}
	g->failed = 0;
	return g;
}

// ---- textures --------------------------------------------------------------------------------------

/* A texture of w x h, read bilinearly and held at its edges; of floats
 * for a pass's result, else of bytes without alpha - the picture's.
 * OpenGL ES takes the picture's pixels only as red, green, blue, alpha in
 * that order of bytes, where the engine's are blue, green, red and a byte
 * that says nothing (OsGl_Blit): the texture is one with alpha there, read
 * with red and blue changed over and its alpha as 1, so that every shader
 * sees what it sees of the other OpenGL's. */
static GLuint Texture(int w, int h, int floats)
{
	GLuint t = 0;
	gl.GenTextures(1, &t);
	if(!t)
		return 0;
	gl.BindTexture(GL_TEXTURE_2D, t);
	gl.TexImage2D(GL_TEXTURE_2D, 0, floats ? GL_RGBA16F : gEs ? GL_RGBA8
															  : GL_RGB8,
		w, h, 0, GL_RGBA, floats ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
	if(gEs && !floats)
	{
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
	}
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

// the plan's textures go (not the picture's own)
static void PlanDrop(void)
{
	int i;
	for(i = 1; i < OS_GLPLAN_SLOTS; i++)
		if(gSlot[i])
		{
			gl.DeleteTextures(1, &gSlot[i]);
			gSlot[i] = 0;
		}
	gPlanChain = -1;
}

/* w x h pixels of 15 bits (RGB555) as pixels of 32, for OpenGL ES, which
 * takes none of that layout: in a buffer kept for the next call; NULL
 * when memory ran out. */
static const uint32_t* Wide(const unsigned char* src, int pitch, int w, int h)
{
	static uint32_t* buf;
	static size_t room;
	size_t need = (size_t)w * (size_t)h;
	uint32_t* out;
	int x, y;
	if(need > room)
	{
		uint32_t* grown = (uint32_t*)realloc(buf, need * sizeof *buf);
		if(!grown)
			return NULL;
		buf = grown;
		room = need;
	}
	out = buf;
	for(y = 0; y < h; y++, src += pitch)
		for(x = 0; x < w; x++)
		{
			unsigned v = (unsigned)src[x * 2] | (unsigned)src[x * 2 + 1] << 8;
			unsigned r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
			*out++ = (uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 3 | g >> 2) << 8 | (uint32_t)(b << 3 | b >> 2);
		}
	return buf;
}

void OsGl_Blit(const OsSurface_t* s, int dstX, int dstY, int w, int h, int srcX, int srcY, int picW, int picH)
{
	int bytes = s->bpp == 16 ? 2 : 4;
	if(!gOn || !s->pixels || picW < 1 || picH < 1 || picW > gMaxSize || picH > gMaxSize || (s->bpp != 32 && s->bpp != 16))
		return;
	if(!gPic || picW != gPicW || picH != gPicH)
	{ // a picture of another size: a new texture, black
		void* black = calloc((size_t)picW * (size_t)picH, 4);
		PlanDrop();
		if(gPic)
			gl.DeleteTextures(1, &gPic);
		gPic = Texture(picW, picH, 0);
		gPicW = picW;
		gPicH = picH;
		gSlot[0] = gPic;
		if(gPic && black)
		{
			gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, picW, picH, GL_RGBA, GL_UNSIGNED_BYTE, black);
		}
		free(black);
	}
	// the part that lies in both
	if(dstX < 0)
	{
		w += dstX;
		srcX -= dstX;
		dstX = 0;
	}
	if(dstY < 0)
	{
		h += dstY;
		srcY -= dstY;
		dstY = 0;
	}
	if(srcX < 0)
	{
		w += srcX;
		dstX -= srcX;
		srcX = 0;
	}
	if(srcY < 0)
	{
		h += srcY;
		dstY -= srcY;
		srcY = 0;
	}
	if(w > s->width - srcX)
		w = s->width - srcX;
	if(h > s->height - srcY)
		h = s->height - srcY;
	if(w > gPicW - dstX)
		w = gPicW - dstX;
	if(h > gPicH - dstY)
		h = gPicH - dstY;
	if(!gPic || w <= 0 || h <= 0)
		return;
	gl.BindTexture(GL_TEXTURE_2D, gPic);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if(gEs)
	{ // the bytes as they lie in memory (a processor that keeps the low byte first, as every one OpenGL ES runs on here)
		const unsigned char* from = (const unsigned char*)s->pixels + (ptrdiff_t)srcY * s->pitch + (ptrdiff_t)srcX * bytes;
		int rowLength = s->pitch / 4;
		if(bytes == 2)
		{
			from = (const unsigned char*)Wide(from, s->pitch, w, h);
			rowLength = w;
			if(!from)
				return;
		}
		gl.PixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		gl.TexSubImage2D(GL_TEXTURE_2D, 0, dstX, dstY, w, h, GL_RGBA, GL_UNSIGNED_BYTE, from);
		gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return;
	}
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, s->pitch / bytes);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, dstX, dstY, w, h, GL_BGRA, bytes == 2 ? GL_UNSIGNED_SHORT_1_5_5_5_REV : GL_UNSIGNED_INT_8_8_8_8_REV,
		(const unsigned char*)s->pixels + (ptrdiff_t)srcY * s->pitch + (ptrdiff_t)srcX * bytes);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// ---- a chain ---------------------------------------------------------------------------------------

/* The chain's plan for the picture shown at outW x outH, with its
 * textures; 0 when it cannot be set up (the chain is given up then). */
static int PlanFor(int chain, int outW, int outH)
{
	const OsGlPass_t* files[CHAIN_FILES];
	int counts[CHAIN_FILES], n = 0, i;
	if(gPlanChain == chain && gPlanPicW == gPicW && gPlanPicH == gPicH && gPlanOutW == outW && gPlanOutH == outH)
		return 1;
	PlanDrop();
	for(i = 0; i < CHAIN_FILES && kChains[chain][i] >= 0; i++)
	{
		File_t* f = FileOf(kChains[chain][i]);
		if(!f)
			return 0;
		files[n] = f->pass;
		counts[n++] = f->passes;
	}
	if(!OsGlHook_Plan(files, counts, n, gPicW, gPicH, outW, outH, &gPlan))
		return 0;
	for(i = 1; i < gPlan.slots; i++)
	{
		if(gPlan.slot[i].w > gMaxSize || gPlan.slot[i].h > gMaxSize || !(gSlot[i] = Texture(gPlan.slot[i].w, gPlan.slot[i].h, 1)))
		{
			PlanDrop();
			return 0;
		}
	}
	gPlanChain = chain;
	gPlanPicW = gPicW;
	gPlanPicH = gPicH;
	gPlanOutW = outW;
	gPlanOutH = outH;
	return 1;
}

// the chain's passes over the picture; the texture with the result and its size, 0 when it could not be done
static GLuint RunChain(int chain, int outW, int outH, int* w, int* h)
{
	int i, k;
	if(gChainFailed[chain])
		return 0;
	if(!PlanFor(chain, outW, outH))
	{
		gChainFailed[chain] = 1;
		fprintf(stderr, "openbgi: OpenGL: the filter \"%s\" cannot be set up here: bicubic instead\n", kFilters[chain / 2]);
		return 0;
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, gFbo);
	for(i = 0; i < gPlan.steps; i++)
	{
		const OsGlStep_t* st = &gPlan.step[i];
		File_t* file = gFile[kChains[chain][st->file]];
		Program_t* g = PassProgram(file, st->pass);
		if(g)
		{
			gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gSlot[st->out], 0);
			if(gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				g = NULL;
		}
		if(!g)
		{
			gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
			gChainFailed[chain] = 1;
			fprintf(stderr, "openbgi: OpenGL: the filter \"%s\" does not run here: bicubic instead\n", kFilters[chain / 2]);
			return 0;
		}
		gl.Viewport(0, 0, st->w, st->h);
		gl.UseProgram(g->id);
		for(k = 0; k < g->units; k++)
		{
			int slot = st->in[g->bind[k]];
			gl.ActiveTexture(GL_TEXTURE0 + (GLenum)k);
			gl.BindTexture(GL_TEXTURE_2D, gSlot[slot]);
			if(g->size[k] >= 0)
				gl.Uniform2f(g->size[k], (GLfloat)gPlan.slot[slot].w, (GLfloat)gPlan.slot[slot].h);
			if(g->pt[k] >= 0)
				gl.Uniform2f(g->pt[k], 1.0f / (GLfloat)gPlan.slot[slot].w, 1.0f / (GLfloat)gPlan.slot[slot].h);
		}
		gl.DrawArrays(GL_TRIANGLES, 0, 3);
	}
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	*w = gPlan.slot[gPlan.result].w;
	*h = gPlan.slot[gPlan.result].h;
	return gSlot[gPlan.result];
}

// ---- the layer -------------------------------------------------------------------------------------

/* The layer's texture is one with alpha, a pixel of it a pixel of the
 * target, read without a filter.  Its bytes go in as the picture's do:
 * as they lie in memory with red and blue changed over when read in
 * OpenGL ES, named as they are in the other. */
void OsGl_Layer(const unsigned char* pixels, int w, int h, int pitch, int x, int y, int cw, int ch)
{
	if(!gOn)
		return;
	if(!pixels || w < 1 || h < 1 || w > gMaxSize || h > gMaxSize)
	{
		if(gLayer)
			gl.DeleteTextures(1, &gLayer);
		gLayer = 0;
		gLayerW = gLayerH = 0;
		return;
	}
	if(!gLayer || w != gLayerW || h != gLayerH)
	{ // a layer of another size: a new texture, and all of it
		if(gLayer)
			gl.DeleteTextures(1, &gLayer);
		gl.GenTextures(1, &gLayer);
		gLayerW = gLayerH = 0;
		if(!gLayer)
			return;
		gl.BindTexture(GL_TEXTURE_2D, gLayer);
		gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		if(gEs)
		{
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
		}
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gLayerW = w;
		gLayerH = h;
		x = y = 0;
		cw = w;
		ch = h;
	}
	if(x < 0)
	{
		cw += x;
		x = 0;
	}
	if(y < 0)
	{
		ch += y;
		y = 0;
	}
	if(cw > w - x)
		cw = w - x;
	if(ch > h - y)
		ch = h - y;
	if(cw <= 0 || ch <= 0)
		return;
	gl.BindTexture(GL_TEXTURE_2D, gLayer);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
	gl.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, cw, ch, gEs ? GL_RGBA : GL_BGRA, gEs ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8_REV,
		pixels + (ptrdiff_t)y * pitch + (ptrdiff_t)x * 4);
	gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// the layer over what the target holds, when it is the target's size (one of another size is of a surface that was)
static void LayerDraw(int targetW, int targetH)
{
	if(!gLayer || !gOver || gLayerW != targetW || gLayerH != targetH)
		return;
	gl.Viewport(0, 0, targetW, targetH);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, gLayer);
	gl.Enable(GL_BLEND);
	gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl.UseProgram(gOver);
	gl.Uniform1f(gOverFlip, -1.0f);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	gl.Disable(GL_BLEND);
}

// ---- the window ------------------------------------------------------------------------------------

int OsGl_Black(int targetW, int targetH)
{
	if(!gOn || targetW < 1 || targetH < 1)
		return 0;
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	gl.Viewport(0, 0, targetW, targetH);
	gl.ClearColor(0, 0, 0, 1);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	return 1;
}

int OsGl_Draw(int targetW, int targetH, int x, int y, int w, int h, int rule)
{
	int kind = rule & OPENBGI_SCALE_RULE, srcW = gPicW, srcH = gPicH, shrink, cubic;
	GLuint src = gPic;
	if(!OsGl_Black(targetW, targetH))
		return 0;
	if(!gPic || w < 1 || h < 1)
	{
		LayerDraw(targetW, targetH);
		return 1;
	}
	if(kind >= OPENBGI_SCALE_OWN && kind < OPENBGI_SCALE_OWN + CHAINS / 2 && gPack && gFloats)
	{
		GLuint made = RunChain((kind - OPENBGI_SCALE_OWN) * 2 + ((rule & OPENBGI_SCALE_HIGH) != 0), w, h, &srcW, &srcH);
		if(made)
			src = made;
		kind = OPENBGI_SCALE_BICUBIC; // (what is left to scale, or all of it when the chain is not to be had)
	}
	else if(kind >= OPENBGI_SCALE_OWN)
		kind = OPENBGI_SCALE_BICUBIC;
	shrink = w < srcW || h < srcH;
	cubic = kind == OPENBGI_SCALE_BICUBIC && !shrink && (w != srcW || h != srcH);
	gl.Viewport(x, targetH - y - h, w, h);
	gl.ActiveTexture(GL_TEXTURE0);
	gl.BindTexture(GL_TEXTURE_2D, src);
	/* by the nearest pixel, or between the nearest; a picture that is made
	 * smaller is read from smaller copies of itself, so that no pixel of
	 * it is passed over */
	if(shrink && kind != OPENBGI_SCALE_NEAREST)
	{
		gl.GenerateMipmap(GL_TEXTURE_2D);
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	else
		gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kind == OPENBGI_SCALE_NEAREST ? GL_NEAREST : GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, kind == OPENBGI_SCALE_NEAREST ? GL_NEAREST : GL_LINEAR);
	if(cubic)
	{
		gl.UseProgram(gCubic);
		gl.Uniform1f(gCubicFlip, -1.0f);
		gl.Uniform2f(gCubicSize, (GLfloat)srcW, (GLfloat)srcH);
	}
	else
	{
		gl.UseProgram(gCopy);
		gl.Uniform1f(gCopyFlip, -1.0f);
	}
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
	// (the passes read every texture bilinearly)
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	LayerDraw(targetW, targetH);
	return 1;
}

const char* OsGl_FilterName(int n)
{
	return gOn && gPack && gFloats && n >= 0 && n < CHAINS / 2 ? kFilters[n] : NULL;
}

// ---- start and stop --------------------------------------------------------------------------------

// whether `text` names a renderer that draws on the processor
static int Software(const char* text)
{
	static const char* const kWords[] = {"llvmpipe", "softpipe", "swrast", "software", "swiftshader", "gdi generic", "microsoft basic"};
	char low[160];
	size_t i;
	for(i = 0; text[i] && i < sizeof low - 1; i++)
		low[i] = text[i] >= 'A' && text[i] <= 'Z' ? (char)(text[i] - 'A' + 'a') : text[i];
	low[i] = 0;
	for(i = 0; i < sizeof kWords / sizeof kWords[0]; i++)
		if(strstr(low, kWords[i]))
			return 1;
	return 0;
}

// whether the version string is OpenGL ES's ("OpenGL ES 3.2 ..."); the numbers begin at *at then
static int IsEs(const char* version, const char** at)
{
	static const char kEs[] = "OpenGL ES ";
	int es = strncmp(version, kEs, sizeof kEs - 1) == 0;
	*at = es ? version + sizeof kEs - 1 : version;
	return es;
}

// whether `list`, names with a space between them, has `name`
static int Listed(const char* list, const char* name)
{
	size_t n = strlen(name);
	const char* at = list;
	while(at && (at = strstr(at, name)) != NULL)
	{
		if((at == list || at[-1] == ' ') && (at[n] == ' ' || !at[n]))
			return 1;
		at += n;
	}
	return 0;
}

/* Whether a context of this version and renderer will do; says why not
 * on stderr.  A context of OpenGL ES is taken whatever draws it: the
 * system that hands one over has no other way to show the picture. */
static int Fits(const char* version, const char* renderer, int* major, int* minor)
{
	const char* numbers;
	int es = IsEs(version, &numbers);
	*major = *minor = 0;
	sscanf(numbers, "%d.%d", major, minor);
	if(*major < 3)
	{
		fprintf(stderr, "openbgi: OpenGL %s of \"%s\" is older than 3.0: the picture is scaled by the processor\n", version, renderer);
		return 0;
	}
	if(es)
		return 1;
	if(Software(renderer) && OsGl_Wanted() == OS_GL_AUTO)
	{
		fprintf(stderr, "openbgi: OpenGL here draws on the processor (\"%s\"): the picture is scaled without it\n", renderer);
		return 0;
	}
	return 1;
}

int OsGl_Usable(OsGlLoader_t load)
{
	const unsigned char*(GLCALL * getString)(unsigned) = NULL;
	const char *version, *renderer;
	OsGlProc_t p;
	int major, minor;
	if(!load || OsGl_Wanted() == OS_GL_OFF || !(p = load("glGetString")))
		return 0;
	memcpy(&getString, &p, sizeof getString);
	version = (const char*)getString(GL_VERSION);
	renderer = (const char*)getString(GL_RENDERER);
	return version && renderer && Fits(version, renderer, &major, &minor);
}

int OsGl_Start(OsGlLoader_t load)
{
	OsGlProc_t found[sizeof kNames / sizeof kNames[0]];
	const char *version, *renderer, *numbers;
	int major = 0, minor = 0, i;
	OsGl_Stop();
	if(!load || OsGl_Wanted() == OS_GL_OFF || sizeof gl != sizeof found)
		return 0;
	for(i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); i++)
	{
		found[i] = load(kNames[i]);
		if(!found[i] && i < 3)
			return 0; // (not even the questions can be asked)
	}
	memcpy(&gl, found, sizeof gl); // (the members are function pointers one after the other, in the names' order)
	version = (const char*)gl.GetString(GL_VERSION);
	renderer = (const char*)gl.GetString(GL_RENDERER);
	if(!version || !renderer)
		return 0;
	snprintf(gRenderer, sizeof gRenderer, "%s", renderer);
	if(!Fits(version, renderer, &major, &minor))
		return 0;
	gEs = IsEs(version, &numbers);
	for(i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); i++)
		if(!found[i])
		{
			fprintf(stderr, "openbgi: OpenGL of \"%s\" has no %s: the picture is scaled by the processor\n", renderer, kNames[i]);
			return 0;
		}
	gOld = !gEs && ((major == 3 && minor < 3) || OsGl_Wanted() == OS_GL_OLD);
	/* a pass draws into a texture of 16-bit floats: every OpenGL from 3.0
	 * can, OpenGL ES from 3.2, or where it names the extension for it */
	gFloats = 1;
	if(gEs && (major == 3 && minor < 2))
	{
		const char* ext = (const char*)gl.GetString(GL_EXTENSIONS);
		gFloats = ext && (Listed(ext, "GL_EXT_color_buffer_half_float") || Listed(ext, "GL_EXT_color_buffer_float"));
	}
	gMaxSize = 2048;
	gMaxUnits = 16;
	gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &gMaxSize);
	gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &gMaxUnits);
	if(gMaxUnits > OS_GLHOOK_BINDS)
		gMaxUnits = OS_GLHOOK_BINDS;
	gl.GenVertexArrays(1, &gVao);
	gl.BindVertexArray(gVao);
	gl.GenFramebuffers(1, &gFbo);
	gCopy = Simple(kCopy, "copy");
	gCubic = Simple(kCubic, "bicubic");
	gOver = Simple(kOver, "layer");
	if(!gVao || !gFbo || !gCopy || !gCubic || !gOver)
	{
		gOn = 1; // (so that OsGl_Stop takes down what there is)
		OsGl_Stop();
		return 0;
	}
	gCopyFlip = gl.GetUniformLocation(gCopy, "flip");
	gCubicFlip = gl.GetUniformLocation(gCubic, "flip");
	gCubicSize = gl.GetUniformLocation(gCubic, "size");
	gOverFlip = gl.GetUniformLocation(gOver, "flip");
	memset(gChainFailed, 0, sizeof gChainFailed);
	gOn = 1;
	fprintf(stderr, "openbgi: the picture is shown through OpenGL%s %d.%d (\"%s\")%s\n", gEs ? " ES" : "", major, minor, renderer,
		gFloats ? "" : "; no filters beyond the three: it cannot draw into textures of floats");
	return 1;
}

void OsGl_Stop(void)
{
	int f, i;
	if(!gOn)
		return;
	PlanDrop();
	if(gPic)
		gl.DeleteTextures(1, &gPic);
	gPic = 0;
	gSlot[0] = 0;
	gPicW = gPicH = 0;
	if(gLayer)
		gl.DeleteTextures(1, &gLayer);
	gLayer = 0;
	gLayerW = gLayerH = 0;
	for(f = 0; f < FILES; f++)
		if(gFile[f])
		{
			for(i = 0; i < OS_GLHOOK_PASSES; i++)
			{
				if(gFile[f]->prog[i].id)
					gl.DeleteProgram(gFile[f]->prog[i].id);
				memset(&gFile[f]->prog[i], 0, sizeof gFile[f]->prog[i]);
			}
		}
	if(gCopy)
		gl.DeleteProgram(gCopy);
	if(gCubic)
		gl.DeleteProgram(gCubic);
	if(gOver)
		gl.DeleteProgram(gOver);
	if(gFbo)
		gl.DeleteFramebuffers(1, &gFbo);
	if(gVao)
		gl.DeleteVertexArrays(1, &gVao);
	gCopy = gCubic = gOver = gFbo = gVao = 0;
	gOn = 0;
}

#else

// a build without OpenGL: never on, so that a back end need not ask how it was built

void OS_UseShaders(const unsigned char* pack, unsigned int size)
{
	(void)pack;
	(void)size;
}

int OsGl_Wanted(void)
{
	return OS_GL_OFF;
}

int OsGl_Usable(OsGlLoader_t load)
{
	(void)load;
	return 0;
}

int OsGl_Start(OsGlLoader_t load)
{
	(void)load;
	return 0;
}

void OsGl_Stop(void)
{
}

int OsGl_On(void)
{
	return 0;
}

void OsGl_Blit(const OsSurface_t* s, int dstX, int dstY, int w, int h, int srcX, int srcY, int picW, int picH)
{
	(void)s;
	(void)dstX;
	(void)dstY;
	(void)w;
	(void)h;
	(void)srcX;
	(void)srcY;
	(void)picW;
	(void)picH;
}

void OsGl_Layer(const unsigned char* pixels, int w, int h, int pitch, int x, int y, int cw, int ch)
{
	(void)pixels;
	(void)w;
	(void)h;
	(void)pitch;
	(void)x;
	(void)y;
	(void)cw;
	(void)ch;
}

int OsGl_Draw(int targetW, int targetH, int x, int y, int w, int h, int rule)
{
	(void)targetW;
	(void)targetH;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)rule;
	return 0;
}

int OsGl_Black(int targetW, int targetH)
{
	(void)targetW;
	(void)targetH;
	return 0;
}

const char* OsGl_FilterName(int n)
{
	(void)n;
	return NULL;
}

#endif
