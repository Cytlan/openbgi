/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * tests/frames.c - unit test of the frames of a run and of the requests
 *                  (src/vm/sched.c, src/sys/request.c, src/wait/wait_step.c;
 *                  include/openbgi.h, src/include/bgi/request.h); built as
 *                  test_frames by `make test`
 *
 * The engine is brought up without its window and its sound, a small boot
 * program is written to a scratch directory, and the test is the host: it
 * runs one frame after the other (Vm_Frame, what OpenBGI_RunFrame runs
 * once the game is up), moves the host's clock five milliseconds a frame,
 * and answers the requests itself.
 *
 * The groups of tests, in the order they run:
 *
 *   queue  - requests are handed to the host oldest first, each once; an
 *            answer is taken by the asker, a forgotten request goes with
 *            its answer; the count of the unanswered ones
 *   hold   - a box holds the engine: until it is answered no thread runs,
 *            frames do nothing, and the pass goes on with the next
 *            instruction of the thread that showed it; the boxes of two
 *            threads come in the order the original shows them
 *   answer - a question's button is the instruction's result, and the
 *            thread that asked holds the engine until it has it: the
 *            other thread runs only after the asking thread's turn
 *   retry  - a load that finds its file missing from the disc asks for
 *            the disc and runs its instruction again once the disc is
 *            there (after the original's 200 ms), with the same operands;
 *            "abort" ends the machine as the original's throw does
 *   hidden - painting switched off (OpenBGI_SetDrawing): a present that
 *            is due goes by and counts without the compositor; switched
 *            on again, the whole picture is asked for at once
 */
#include "bgi/core.h"
#include "bgi/vm.h"
#include "bgi/request.h"
#include "bgi/error.h"
#include "bgi/sys.h"
#include "bgi/file.h"
#include "bgi/gfx.h"
#include "bgi/display.h"
#include "bgi/sysobj/database.h"
#include "host/os.h"
#include "host/os_common.h"
#include "bgi/msg.h"
#include "bgi/engine.h"

#include "testhost.h"

#include <stdio.h>
#include <string.h>

#include "check.h"

// ---- the host -------------------------------------------------------------------

static uint32_t gNow = 1000; // the host's clock (ms)

static uint32_t HostTicksMs(void* user)
{
	BGI_UNUSED(user);
	return gNow;
}

static int HostPerfNs(void* user, int64_t* out)
{
	BGI_UNUSED(user);
	BGI_UNUSED(out);
	return 0;
}

static void HostLocalTime(void* user, OpenBGI_DateTime_t* out)
{
	BGI_UNUSED(user);
	memset(out, 0, sizeof *out);
	out->year = 2026;
	out->month = 10;
	out->day = 24;
}

// the clocks above, with the back end's fonts
static OpenBGI_Host_t gHost = {.size = sizeof(OpenBGI_Host_t), .api = OPENBGI_API_VERSION, .ticksMs = HostTicksMs, .perfNs = HostPerfNs, .localTime = HostLocalTime, .caps = OPENBGI_CAP_DIALOGS};

// one frame, five milliseconds after the last one; 1 while the run goes on
static int Frame(OpenBGI_t* o)
{
	OpenBGI_Frame_t f = {.size = sizeof f};
	gNow += 5;
	return Vm_Frame(o, &f);
}

/* Run frames until the engine asks something (1 and *q) or the run ends
 * (0); at most `max` frames. */
static int UntilAsked(OpenBGI_t* o, OpenBGI_Request_t* q, int max)
{
	while(max-- > 0)
	{
		if(OpenBGI_NextRequest(o, q))
			return 1;
		if(!Frame(o))
			return OpenBGI_NextRequest(o, q);
	}
	return OpenBGI_NextRequest(o, q);
}

// ---- programs -------------------------------------------------------------------

#define MAX_STRINGS 8

// a program under construction: code, then its strings, then a scratch area
typedef struct Prog
{
	uint8_t code[0x400];
	uint32_t len;
	const char* str[MAX_STRINGS]; // the strings, placed after the code
	uint32_t strFix[MAX_STRINGS]; // where each "05" displacement goes (its opcode's offset)
	int strOf[MAX_STRINGS];       // the string of each fix
	int fixes, strings;
	uint32_t label[4]; // label offsets
	uint32_t jump[4];  // the "06" of a jump to a label (its opcode's offset)
	int jumpTo[4], jumps;
} Prog_t;

static void P8(Prog_t* p, uint32_t v)
{
	p->code[p->len++] = (uint8_t)v;
}

static void P16(Prog_t* p, uint32_t v)
{
	P8(p, v & 0xff);
	P8(p, v >> 8);
}

static void P32(Prog_t* p, uint32_t v)
{
	P16(p, v & 0xffff);
	P16(p, v >> 16);
}

static void PushI(Prog_t* p, uint32_t v) // 02
{
	P8(p, 0x02);
	P32(p, v);
}

// push the code address of a string ("05"; fixed up by Finish)
static void PushStr(Prog_t* p, const char* s)
{
	int i;
	for(i = 0; i < p->strings && strcmp(p->str[i], s) != 0; i++)
		;
	if(i == p->strings)
		p->str[p->strings++] = s;
	p->strFix[p->fixes] = p->len;
	p->strOf[p->fixes++] = i;
	P8(p, 0x05);
	P16(p, 0);
}

// push the code offset of label n ("06"; fixed up by Finish)
static void PushLabel(Prog_t* p, int n)
{
	p->jump[p->jumps] = p->len;
	p->jumpTo[p->jumps++] = n;
	P8(p, 0x06);
	P16(p, 0);
}

static void Op2(Prog_t* p, uint32_t fam, uint32_t op)
{
	P8(p, fam);
	P8(p, op);
}

static void Box(Prog_t* p, const char* text) // B0 80: an information box
{
	PushStr(p, text);
	Op2(p, 0xb0, 0x80);
}

static void End(Prog_t* p) // 17 at the outermost frame: the thread ends
{
	P8(p, 0x17);
}

/* the strings after the code (at `scratch` bytes of room for a buffer
 * behind them: *scratchAt is its offset), the displacements fixed up,
 * written as a program file `name` (a 16-byte header, then the code) */
static void Finish(Prog_t* p, const char* name, uint32_t* scratchAt)
{
	uint32_t at[MAX_STRINGS];
	uint8_t file[16 + 0x800];
	uint32_t hdr[4] = {16, 0, 0, 0}, i, end;
	FILE* f;
	end = p->len;
	for(i = 0; i < (uint32_t)p->strings; i++)
	{
		at[i] = end;
		memcpy(p->code + end, p->str[i], strlen(p->str[i]) + 1);
		end += (uint32_t)strlen(p->str[i]) + 1;
	}
	for(i = 0; i < (uint32_t)p->fixes; i++)
	{
		uint32_t d = at[p->strOf[i]] - p->strFix[i];
		p->code[p->strFix[i] + 1] = (uint8_t)d;
		p->code[p->strFix[i] + 2] = (uint8_t)(d >> 8);
	}
	for(i = 0; i < (uint32_t)p->jumps; i++)
	{
		uint32_t d = p->label[p->jumpTo[i]] - p->jump[i];
		p->code[p->jump[i] + 1] = (uint8_t)d;
		p->code[p->jump[i] + 2] = (uint8_t)(d >> 8);
	}
	if(scratchAt)
		*scratchAt = end;
	end += 0x40; // the scratch area
	hdr[1] = end;
	memcpy(file, hdr, 16);
	memcpy(file + 16, p->code, end);
	f = fopen(name, "wb");
	if(f)
	{
		fwrite(file, 1, 16 + end, f);
		fclose(f);
	}
}

static void ProgInit(Prog_t* p)
{
	memset(p, 0, sizeof *p);
}

// ---- the engine -----------------------------------------------------------------

/* Engine_Init without the window and the sound: what the frames need */
static OpenBGI_t* EngineUp(void)
{
	OpenBGI_t* o;
	gHost.fontFiles = OS_FontFiles();
	gHost.fontRenderer = OS_FontRenderer();
	gHost.files = OsCommon_Files();
	gHost.registry = OsCommon_Registry();
	o = OpenBGI_New(&gHost);
	if(!o)
		return NULL;
	Gfx_Create(o, 0xffffffffu, 1);
	o->files.arcMgr = FileSet_New();
	SetGlobalMemSize(o, 4);
	o->mem.sysAreaSize = 0x40000;
	o->mem.sysArea = (uint8_t*)BGI_Calloc(0x100000);
	SysArea_Init(o);
	DynMem_Init(o);
	Vm_TablesInit(o);
	UserOp_Init(o);
	Loader_Start(o);
	Child_InitTable(o);
	Ptcl_InitGlobals(o);
	o->engine.engineUp = 1;
	SetBaseDir(o, NULL); // the scratch directory, the drive the OS layer presents as C:
	return o;
}

static void EngineDown(OpenBGI_t* o)
{
	Vm_Stop(o);
	Request_Clear(o);
	Engine_Shutdown(o);
	OpenBGI_Delete(o);
}

// ---- the tests ----------------------------------------------------------------------

static void TestQueue(void)
{
	OpenBGI_t* o = OpenBGI_New(&gHost);
	OpenBGI_Request_t q = {.size = sizeof q}, a = {.size = sizeof a}, b = {.size = sizeof b};
	uint32_t r1, r2, r3;
	int answer = -1;
	printf("queue\n");
	if(!o)
		return;
	CHECK(!OpenBGI_NextRequest(o, &q) && Request_Pending(o) == 0);
	r1 = Request_Box(o, "one", "c1", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	r2 = Request_Box(o, "two", "c2", OPENBGI_MB_YESNO, REQUEST_TREAT_NONE);
	CHECK(r1 && r2 && r1 != r2 && Request_Pending(o) == 2);
	CHECK(OpenBGI_NextRequest(o, &a) && a.id == r1 && a.kind == OPENBGI_ASK_MESSAGE && strcmp(a.u.message.text, "one") == 0);
	CHECK(OpenBGI_NextRequest(o, &b) && b.id == r2 && b.u.message.flags == OPENBGI_MB_YESNO && strcmp(b.u.message.caption, "c2") == 0);
	CHECK(!OpenBGI_NextRequest(o, &q)); // each is handed out once
	CHECK(!Request_Take(o, r2, &answer) && answer == -1);
	OpenBGI_Answer(o, r2, OPENBGI_IDYES);
	CHECK(Request_Pending(o) == 1);
	OpenBGI_Answer(o, r2, OPENBGI_IDNO); // a second answer is ignored
	CHECK(Request_Take(o, r2, &answer) && answer == OPENBGI_IDYES);
	Request_Forget(o, r1); // nobody waits for it
	OpenBGI_Answer(o, r1, OPENBGI_IDOK);
	CHECK(Request_Pending(o) == 0);
	CHECK(Request_Take(o, r1, &answer) && answer == 0); // gone with its answer
	r3 = Request_Box(o, "three", "", OPENBGI_MB_OK, REQUEST_TREAT_NONE);
	CHECK(r3 != r1 && r3 != r2 && Request_Pending(o) == 1);
	OpenBGI_Delete(o); // with one unanswered
}

/* T1 starts T2, shows A1, yields, shows A2, ends; T2 shows B1, yields,
 * shows B2, ends.  The boxes come A1 B1 A2 B2; while one is up the frames
 * do nothing. */
static void TestHold(void)
{
	Prog_t p;
	OpenBGI_t* o = EngineUp();
	OpenBGI_Request_t q = {.size = sizeof q};
	char seen[64] = "";
	int i, n = 0;
	printf("hold\n");
	if(!o)
		return;
	ProgInit(&p);
	PushStr(&p, "system.arc");
	PushStr(&p, "t2._bp");
	PushI(&p, 0x100);
	PushI(&p, 0x1000);
	PushI(&p, 0x1000);
	Op2(&p, 0x80, 0x44); // spawn T2 (behind T1 in the list: the same pass reaches it)
	Box(&p, "A1");
	Op2(&p, 0x80, 0x5f); // yield
	Box(&p, "A2");
	End(&p);
	Finish(&p, "ipl._bp", NULL);
	ProgInit(&p);
	Box(&p, "B1");
	Op2(&p, 0x80, 0x5f);
	Box(&p, "B2");
	End(&p);
	Finish(&p, "t2._bp", NULL);

	Vm_Start(o);
	while(n < 8 && UntilAsked(o, &q, 50))
	{
		CHECK(q.kind == OPENBGI_ASK_MESSAGE);
		strcat(seen, q.u.message.text);
		strcat(seen, " ");
		for(i = 0; i < 3; i++)
			CHECK(Frame(o) && Request_Pending(o) == 1); // held: nothing runs, nothing new is asked
		CHECK(!OpenBGI_NextRequest(o, &q));
		OpenBGI_Answer(o, q.id, OPENBGI_IDOK);
		n++;
	}
	CHECK(strcmp(seen, "A1 B1 A2 B2 ") == 0);
	for(i = 0; i < 10 && Frame(o); i++)
		;
	CHECK(i < 10); // the run is over: both threads ended
	EngineDown(o);
}

/* T1 starts T2 and asks Q (yes / no); yes shows Y, no shows N; T2 shows
 * T2.  Answered yes: Q Y T2; no: Q N T2. */
static void TestAnswer(int yes)
{
	Prog_t p;
	OpenBGI_t* o = EngineUp();
	OpenBGI_Request_t q = {.size = sizeof q};
	char seen[64] = "";
	int n = 0;
	if(!o)
		return;
	ProgInit(&p);
	PushStr(&p, "system.arc");
	PushStr(&p, "t2._bp");
	PushI(&p, 0x100);
	PushI(&p, 0x1000);
	PushI(&p, 0x1000);
	Op2(&p, 0x80, 0x44);
	PushStr(&p, "Q");
	PushI(&p, 1);
	Op2(&p, 0xb0, 0x81); // yes / no → 1 for yes
	PushLabel(&p, 0);
	P8(&p, 0x15); // jcc: == 0 → label 0
	P8(&p, 1);
	Box(&p, "Y");
	End(&p);
	p.label[0] = p.len;
	Box(&p, "N");
	End(&p);
	Finish(&p, "ipl._bp", NULL);
	ProgInit(&p);
	Box(&p, "T2");
	End(&p);
	Finish(&p, "t2._bp", NULL);

	Vm_Start(o);
	while(n < 8 && UntilAsked(o, &q, 50))
	{
		strcat(seen, q.u.message.text);
		strcat(seen, " ");
		if(strcmp(q.u.message.text, "Q") == 0)
		{
			CHECK((q.u.message.flags & 0xf) == OPENBGI_MB_YESNO);
			CHECK(o->vm.holder != NULL); // the asking thread holds the engine
			OpenBGI_Answer(o, q.id, yes ? OPENBGI_IDYES : OPENBGI_IDNO);
		}
		else
			OpenBGI_Answer(o, q.id, OPENBGI_IDOK);
		n++;
	}
	CHECK(strcmp(seen, yes ? "Q Y T2 " : "Q N T2 ") == 0);
	EngineDown(o);
}

/* The installer's choice dialog ("80 F1") is a request whose description
 * is the engine's own: a host answers 0 when it has no such dialog (or
 * the player cancelled), and the button's number and one otherwise.  The
 * script gets -1 for the first and the button for the second; it adds one
 * to what it got and shows Z when that is 0, else P. */
static void TestInstallerChoice(int answer, const char* expect)
{
	Prog_t p;
	OpenBGI_t* o = EngineUp();
	OpenBGI_Request_t q = {.size = sizeof q};
	char seen[64] = "";
	int n = 0, asked = 0;
	if(!o)
		return;
	ProgInit(&p);
	PushStr(&p, "choose"); // the text
	PushI(&p, 0);          // no first button: the form with two
	PushStr(&p, "a");
	PushStr(&p, "b");
	PushI(&p, 3); // none of them disabled
	PushI(&p, 0); // no extra button
	Op2(&p, 0x80, 0xf1);
	PushI(&p, 1);
	P8(&p, 0x20); // add
	PushLabel(&p, 0);
	P8(&p, 0x15); // jcc: == 0 → label 0
	P8(&p, 1);
	Box(&p, "P");
	End(&p);
	p.label[0] = p.len;
	Box(&p, "Z");
	End(&p);
	Finish(&p, "ipl._bp", NULL);

	Vm_Start(o);
	while(n < 4 && UntilAsked(o, &q, 50))
	{
		if(q.kind == OPENBGI_ASK_INSTALLER)
		{
			asked++;
			OpenBGI_Answer(o, q.id, answer);
		}
		else
		{
			strcat(seen, q.u.message.text);
			OpenBGI_Answer(o, q.id, OPENBGI_IDOK);
		}
		n++;
	}
	CHECK(asked == 1);
	CHECK(strcmp(seen, expect) == 0);
	EngineDown(o);
}

/* T1 loads x.dat ("80 30", a loose file) into its scratch area and shows
 * the size it got ("7A").  The file is missing from the base directory
 * and from the disc, so the disc is asked for; the test puts the file on
 * the disc and answers OK (or gives up: Cancel, then "abort? yes"). */
static void TestRetry(int giveUp)
{
	Prog_t p;
	OpenBGI_t* o = EngineUp();
	OpenBGI_Request_t q = {.size = sizeof q};
	uint32_t scratch = 0, asked;
	int i;
	if(!o)
		return;
	remove("alt/x.dat");
	TestHost_Mkdir("alt");
	sprintf(o->files.altDir, "%salt\\", o->files.baseDir); // the disc
	strcpy(o->files.installTitle, "Insert the disc");
	strcpy(o->files.installMsg, "Game");
	ProgInit(&p);
	P8(&p, 0x05); // the scratch area: fixed below
	P16(&p, 0);
	PushI(&p, 0); // no archive
	PushStr(&p, "x.dat");
	Op2(&p, 0x80, 0x30); // buf, arc, name → size
	P8(&p, 0x7a);        // the size in a box
	End(&p);
	Finish(&p, "ipl._bp", &scratch);
	{ // the scratch area's displacement, now that it is known, and the file again
		FILE* f;
		uint8_t file[16 + 0x800];
		size_t len;
		f = fopen("ipl._bp", "rb");
		len = f ? fread(file, 1, sizeof file, f) : 0;
		if(f)
			fclose(f);
		file[16 + 1] = (uint8_t)scratch;
		file[16 + 2] = (uint8_t)(scratch >> 8);
		f = fopen("ipl._bp", "wb");
		if(f)
		{
			fwrite(file, 1, len, f);
			fclose(f);
		}
	}

	Vm_Start(o);
	CHECK(UntilAsked(o, &q, 50));
	CHECK(q.kind == OPENBGI_ASK_MESSAGE && strcmp(q.u.message.text, "Insert the disc") == 0 && strcmp(q.u.message.caption, "Game") == 0);
	CHECK((q.u.message.flags & 0xf) == OPENBGI_MB_OKCANCEL);
	CHECK(o->vm.holder != NULL);
	if(giveUp)
	{
		OpenBGI_Answer(o, q.id, OPENBGI_IDCANCEL);
		CHECK(UntilAsked(o, &q, 5) && strcmp(q.u.message.text, MSG_ASK_ABORT) == 0 && strcmp(q.u.message.caption, "Game") == 0);
		OpenBGI_Answer(o, q.id, OPENBGI_IDYES);
		for(i = 0; i < 20 && Frame(o); i++)
			CHECK(!OpenBGI_NextRequest(o, &q));
		CHECK(i < 20 && !o->vm.sched.running); // the machine stopped, as after the original's throw
		EngineDown(o);
		return;
	}
	{
		FILE* f = fopen("alt/x.dat", "wb");
		if(f)
		{
			fputs("hello", f);
			fclose(f);
		}
	}
	OpenBGI_Answer(o, q.id, OPENBGI_IDOK);
	asked = gNow;
	CHECK(UntilAsked(o, &q, 100));
	CHECK(gNow - asked >= 200);                             // the original's pause before it looks again
	CHECK(strstr(q.u.message.text, "Number : 5 ") != NULL); // the instruction ran again, with its operands
	OpenBGI_Answer(o, q.id, OPENBGI_IDOK);
	for(i = 0; i < 10 && Frame(o); i++)
		;
	CHECK(i < 10);
	remove("alt/x.dat");
	EngineDown(o);
}

static void TestHidden(void)
{
	OpenBGI_t* o = EngineUp();
	uint32_t frames;
	printf("hidden\n");
	if(!o)
		return;
	o->display.displayEnabled = 1;
	o->present.presentAllowed = 1;
	o->present.frameInterval = 16;
	o->present.presentPending = 1;
	o->present.presentPendingFull = 0;
	o->present.nextPresentTime = 0;
	frames = o->present.frameCount;
	OpenBGI_SetDrawing(o, 0);
	CHECK(PresentFrame(o) == 0 && o->present.frameCount == frames + 1 && !o->present.presentPending && o->present.drawStale);
	CHECK(PresentFrame(o) == -1 && o->present.frameCount == frames + 1); // nothing is due
	OpenBGI_SetDrawing(o, 0);                                            // (said twice: as it was)
	CHECK(!o->present.presentPending && o->present.drawOff);
	OpenBGI_SetDrawing(o, 1);
	CHECK(o->present.presentPending && o->present.presentPendingFull && o->present.nextPresentTime == 0);
	CHECK(!o->present.drawOff && o->present.drawStale); // (the next present paints all of it)
	o->present.presentPending = o->present.presentPendingFull = 0;
	OpenBGI_SetDrawing(o, 1); // on already: nothing is asked for
	CHECK(!o->present.presentPending);
	EngineDown(o);
}

int main(void)
{
	char dir[0x400];
	TestHost_ScratchName(dir, sizeof dir, "openbgi_frames");
	if(TestHost_Mkdir(dir) != 0 || TestHost_Chdir(dir) != 0 || !OS_SetGameDir(dir))
	{
		printf("frames: no scratch directory\n");
		return 1;
	}
	OS_Init();
	TestQueue();
	TestHold();
	printf("answer\n");
	TestAnswer(1);
	TestAnswer(0);
	printf("installer\n");
	TestInstallerChoice(0, "Z"); // declined, or cancelled: -1
	TestInstallerChoice(1, "P"); // the first button: 0
	TestInstallerChoice(4, "P"); // the extra button: 3
	TestInstallerChoice(9, "Z"); // no answer of the dialog's: -1
	printf("retry\n");
	TestRetry(0);
	TestRetry(1);
	TestHidden();
	OS_Shutdown();
	TestHost_Chdir(TestHost_TempRoot());
	TestHost_RemoveTree(dir);
	if(gFails)
	{
		printf("frames: %d checks failed\n", gFails);
		return 1;
	}
	printf("ok\n");
	return 0;
}
