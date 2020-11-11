//
// User interface code
//

#include "patch.h"

#define ID_WINDOW_UPDATE_TIMER   1001
#define ID_THREAD_LIST_BOX       2000
#define ID_MEMORY_LIST_BOX       2001
#define ID_STACK_LIST_BOX        2002
#define ID_DISASSEMBLY_LIST_BOX  2003
#define ID_MEMORY_SCROLLBAR      3001
#define ID_STACK_SCROLLBAR       3002
#define ID_DISASSEMBLY_SCROLLBAR 3003
#define ID_BUTTON_DUMP_STACK     9000
#define ID_BUTTON_DUMP_CODE      9001
#define ID_BUTTON_DUMP_LOCAL     9002
#define ID_BUTTON_HALT           9003
#define ID_BUTTON_STEP           9004

HMODULE gDllHModule;
HWND gDebugHWND;
ATOM gHWndClassAtom;
HFONT hFont;
bool initialised = false;

HWND gGUIThreadCount;
HWND gGUITextArea;

// Buttons
HWND gDumpStackButton;
HWND gDumpCodeButton;
HWND gDumpLocalButton;
HWND gHaltButton;
HWND gStepButton;

// Disassembly
HWND gDisassemblyList;
HWND gDisassemblyScrollbar;
HWND gLocalMemoryList;
HWND gLocalMemoryScrollbar;
HWND gStackList;
HWND gStackScrollbar;

// Thread list
HWND gThreadListBox;
VMThread_t curThreadCopy;    // Copy of the thread to prevent redraw of unchanged fields (can cause flickering otherwise)
VMThread_t* curThreadPtr;    // Pointer to the actual thread being displayed
VMThread_t* threadList[256]; // Threads as show in the thread list
int gThreadListCount = 0;    // Number of threads in the list

// Program name
HWND gProgramLabel;

struct ThreadList;
typedef struct ThreadList
{
	VMThread_t* thread;
	uint32_t threadId;
	struct ThreadList* next;
	struct ThreadList* prev;
} ThreadList_t;

ThreadList_t vmThreads;
ThreadList_t* lastThread;
Disasm_t* disassembly;

void freeThreadEntry(ThreadList_t* entry)
{
	// Remove from list
	if(entry->next)
		entry->next->prev = entry->prev;
	if(entry->prev)
		entry->prev->next = entry->next;
	if(entry == lastThread)
		lastThread = entry->prev;
	free(entry);
}

ThreadList_t* addThreadEntry(VMThread_t* t)
{
	ThreadList_t* newEntry = (ThreadList_t*)malloc(sizeof(ThreadList_t));
	lastThread->next = newEntry;
	newEntry->prev = lastThread;
	newEntry->next = NULL;
	newEntry->thread = t;
	newEntry->threadId = t->threadId;
	return newEntry;
}

void updateThreadLists()
{
	if(!*gVMThread) return;

	// First time updating?
	if(vmThreads.thread == NULL)
	{
		vmThreads.thread = *gVMThread;
		vmThreads.threadId = vmThreads.thread->threadId;
		vmThreads.next = NULL;
		vmThreads.prev = NULL;
		lastThread = &vmThreads;
	}

	ThreadList_t* l;
	VMThread_t* t;

	// Check that no threads were deleted
	l = &vmThreads;
	while(l)
	{
		bool found = false;
		t = *gVMThread;
		while(t)
		{
			if(l->thread == t && l->threadId == t->threadId)
			{
				found = true;
				break;
			}
			t = t->nextThread;
		}
		ThreadList_t* n = l->next;
		if(!found)
		{
			// This entry was deleted.
			freeThreadEntry(l);
		}
		l = n;
	}

	// Check for new threads
	t = *gVMThread;
	while(t)
	{
		bool found = false;
		l = &vmThreads;
		ThreadList_t* entry;
		while(l)
		{
			if(l->thread == t && l->threadId == t->threadId)
			{
				entry = l;
				found = true;
				break;
			}
			l = l->next;
		}
		VMThread_t* n = t->nextThread;
		if(!found)
		{
			// This entry is new.
			l = addThreadEntry(t);
		}
		// Disassemble any new programs that may have appeared
		//disassemblePrograms(l);
		t = n;
	}
}

struct VMThreadLabels
{
	HWND programIdLabel;
	HWND programIdValue;
	HWND threadIdLabel;
	HWND threadIdValue;
	HWND nextThreadLabel;
	HWND nextThreadValue;
	HWND flagsLabel;
	HWND flagsValue;
	HWND stackPointerLabel;
	HWND stackPointerValue;
	HWND instructionPointerLabel;
	HWND instructionPointerValue;
	HWND programCounterLabel;
	HWND programCounterValue;
	HWND basePointerLabel;
	HWND basePointerValue;
	HWND stackSizeLabel;
	HWND stackSizeValue;
	HWND stackMemConfigLabel;
	HWND stackMemConfigValue;
	HWND stackLabel;
	HWND stackValue;
	HWND codeSpaceSizeLabel;
	HWND codeSpaceSizeValue;
	HWND codeSpaceMemConfigLabel;
	HWND codeSpaceMemConfigValue;
	HWND codeSpaceLabel;
	HWND codeSpaceValue;
	HWND programListLabel;
	HWND programListValue;
	HWND programCountLabel;
	HWND programCountValue;
	HWND codeSpaceTopLabel;
	HWND codeSpaceTopValue;
	HWND localMemSizeLabel;
	HWND localMemSizeValue;
	HWND localMemConfigLabel;
	HWND localMemConfigValue;
	HWND localMemLabel;
	HWND localMemValue;
	HWND unknownStructLabel;
	HWND unknownStructValue;
	HWND field_0x54Label;
	HWND field_0x54Value;
	HWND field_0x58Label;
	HWND field_0x58Value;
	HWND field_0x5cLabel;
	HWND field_0x5cValue;
	HWND field_0x60Label;
	HWND field_0x60Value;
};
struct VMThreadLabels vmInfo;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch(ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		case DLL_THREAD_ATTACH:
			gDllHModule = hModule;
			createDebugWindow();
			break;
		case DLL_THREAD_DETACH:
		case DLL_PROCESS_DETACH:
			shutdownDebugger();
			break;
	}
    return true;
}

uint8_t memoryViewBuffer[0x100];
off_t memoryOffset;
uint32_t prevMemorySize = -1;
void updateMemoryList(VMThread_t* thread)
{
	int size = thread->localMemSize / 16;
	if(size > 0x10)
		size = 0x10;

	// No local memory
	if(size == 0)
		SendMessage(gLocalMemoryList, LB_RESETCONTENT, 0, 0);

	SCROLLINFO s;
	s.cbSize = sizeof(SCROLLINFO);
	s.fMask = SIF_POS;
	GetScrollInfo(gLocalMemoryScrollbar, SB_CTL, &s);
	off_t newOffset = s.nPos * 16;

	uint8_t* ptr = thread->localMem + newOffset;

	// Memory size changed
	if(thread->localMemSize != prevMemorySize)
	{
		prevMemorySize = thread->localMemSize;
		s.fMask = SIF_PAGE | SIF_RANGE;
		s.nMin = 0;
		s.nMax = prevMemorySize/16;
		s.nPage = 16;
		s.nPos = 0;
		s.nTrackPos = 0;
		SetScrollInfo(gLocalMemoryScrollbar, SB_CTL, &s, true);
	}
	// Memory has not changed, no need to update.
	else if(size == 0 || (newOffset == memoryOffset && memcmp(ptr, &memoryViewBuffer[0], size*16) == 0))
		return;

	memoryOffset = newOffset;

	char memStr[64];

	SendMessage(gLocalMemoryList, LB_RESETCONTENT, 0, 0);
	memcpy(&memoryViewBuffer[0], ptr, size*16);

	for(int i = 0; i < size; i++)
	{
		uint8_t* ptr = &memoryViewBuffer[i*16];
		sprintf(
			&memStr[0],
			"%.8LX: %.2X%.2X%.2X%.2X %.2X%.2X%.2X%.2X %.2X%.2X%.2X%.2X %.2X%.2X%.2X%.2X",
			memoryOffset+(i*16),
			ptr[0],  ptr[1],  ptr[2],  ptr[3],
			ptr[4],  ptr[5],  ptr[6],  ptr[7],
			ptr[8],  ptr[9],  ptr[10], ptr[11],
			ptr[12], ptr[13], ptr[14], ptr[15]
		);
		SendMessage(gLocalMemoryList, LB_INSERTSTRING, i, (WPARAM)&memStr[0]);
	}
}

uint32_t prevStackSize = -1;
void updateStackList(VMThread_t* thread)
{
	SendMessage(gStackList, LB_RESETCONTENT, 0, 0);
	
	SCROLLINFO s;
	s.cbSize = sizeof(SCROLLINFO);
	s.fMask = SIF_POS;
	GetScrollInfo(gStackScrollbar, SB_CTL, &s);

	// Set scrollbar size
	if(thread->stackPointer != prevStackSize)
	{
		int diff = thread->stackPointer - prevStackSize;
		if(!s.nPos || diff + s.nPos < 0)
			s.nPos = 0;
		else
			s.nPos += diff;
		s.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
		s.nMin = 0;
		s.nMax = thread->stackPointer;
		s.nPage = 34;
		s.nTrackPos = 0;
		SetScrollInfo(gStackScrollbar, SB_CTL, &s, true);
		prevStackSize = thread->stackPointer;
	}

	// Don't attempt to add any stack elements if there are none
	if(thread->stackSize == 0 || thread->stackPointer == 0)
		return;

	uint32_t stackPointer = thread->stackPointer - 1;
	
	stackPointer += s.nPos;

	char memStr[32];
	for(int i = 0; i < 34; i++)
	{
		sprintf(
			&memStr[0],
			"%.8LX: %.8LX",
			stackPointer,
			thread->stack[stackPointer]
		);
		SendMessage(gStackList, LB_INSERTSTRING, i, (WPARAM)&memStr[0]);
		if(stackPointer == 0)
			break;
		stackPointer--;
	}
}

VMProgramList_t* getCurrentProgram(VMThread_t* thread)
{
	VMProgramList_t* p = thread->programList;
	while(p)
	{
		if(thread->programCounter >= p->location &&
		   thread->programCounter < p->location + p->size)
			return p;
		p = p->prevProgram;
	}
	return NULL;
}

DisasmLine_t* findCurrentLine(VMThread_t* thread, Disasm_t* disasm)
{
	uint32_t ppos = thread->programCounter;
	DisasmLine_t* start = disasm->disasm;
	DisasmLine_t* cur = disasm->disasm;
	int count = 0;
	while(cur)
	{
		if(cur->location == ppos)
			break;
		count++;
		if(count > 7)
			start = start->next;
		cur = cur->next;
	}
	return start;
}

void updateDisassembly(VMThread_t* curThreadPtr)
{
	VMProgramList_t* p = getCurrentProgram(curThreadPtr);
	SendMessage(gDisassemblyList, LB_RESETCONTENT, 0, 0);
	if(p == NULL)
	{
		return;
	}
	if(!disassembly || disassembly->program != p)
	{
		if(disassembly)
			freeDisassemblies(disassembly);
		disassembly = disassembleProgram(curThreadPtr, p);
	}

	uint32_t ppos = curThreadPtr->programCounter;
	DisasmLine_t* line = findCurrentLine(curThreadPtr, disassembly);
	int highlight = 0;
	for(int i = 0; i < 16; i++)
	{
		if(!line) break;
		SendMessage(gDisassemblyList, LB_INSERTSTRING, i, (WPARAM)line->str);
		if(line->location == ppos)
			highlight = i + 1;
		line = line->next;
	}
	SendMessage(gDisassemblyList, LB_SETCURSEL, highlight, 0);
}

void updateWindow()
{
	//updateThreadLists();
	if(*gVMThread)
	{
		bool mustUpdate = false;
		int oldCount = gThreadListCount;
		gThreadListCount = 0;
		VMThread_t* thread = *gVMThread;

		// Get list of threads
		while(thread)
		{
			// If this entry had changed, we must redraw
			if(threadList[gThreadListCount] != thread)
				mustUpdate = true;
			threadList[gThreadListCount] = thread;
			gThreadListCount++;
			thread = thread->nextThread;
		}
		// If the total count has changed, we must redraw
		mustUpdate = mustUpdate || oldCount != gThreadListCount;

		// Re-draw the list if anything has changed
		if(mustUpdate)
		{
			int selectedThread = -1;

			// Delete previous content
			SendMessage(gThreadListBox, LB_RESETCONTENT, 0, 0);

			// Create new list
			for(int i = 0; i < gThreadListCount; i++)
			{
				char listText[64];
				sprintf(&listText[0], "Thread %d", threadList[i]->threadId);
				SendMessage(gThreadListBox, LB_INSERTSTRING, i, (LPARAM)&listText[0]);

				// Get selected thread list index
				if(threadList[i] == curThreadPtr)
					selectedThread = i;
			}

			// If the current thread was not found, it must be deleted. Reset to the global thread.
			if(selectedThread == -1)
			{
				curThreadPtr = *gVMThread;
				SendMessage(gThreadListBox, LB_SETCURSEL, 0, 0);
			}
			// Otherwise, highlight the current thread
			else
				SendMessage(gThreadListBox, LB_SETCURSEL, selectedThread, 0);
		}
	}
	else
		curThreadPtr = NULL;

	// Update fields
	if(curThreadPtr)
	{
		updateVMInfo(curThreadPtr);
		updateMemoryList(curThreadPtr);
		updateStackList(curThreadPtr);
		updateDisassembly(curThreadPtr);
	}
}

bool dumpStack()
{
	FILE* dumpFile = fopen("stack.bin", "wb");
	if(!dumpFile)
		return false;

	fwrite(curThreadPtr->stack, 4, curThreadPtr->stackSize, dumpFile);
	fclose(dumpFile);
	return true;
}

bool dumpCode()
{
	FILE* dumpFile = fopen("code.bin", "wb");
	if(!dumpFile)
		return false;

	fwrite(curThreadPtr->codeSpace, 1, curThreadPtr->codeSpaceSize, dumpFile);
	fclose(dumpFile);
	return true;
}

bool dumpLocal()
{
	FILE* dumpFile = fopen("local.bin", "wb");
	if(!dumpFile)
		return false;

	fwrite(curThreadPtr->localMem, 1, curThreadPtr->localMemSize, dumpFile);
	fclose(dumpFile);
	return true;
}

void doHaltExecution(int stopped)
{
	gHaltExecution = stopped;
	SetWindowTextA(gHaltButton, stopped ? "Run" : "Halt");
}

LRESULT WINAPI DLLWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg)
	{
		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		case WM_TIMER:
			if(wParam == ID_WINDOW_UPDATE_TIMER)
			{
				updateWindow();
			}

		case WM_CREATE:
			break;
		
		case WM_COMMAND:
			switch(LOWORD(wParam))
			{
				case ID_BUTTON_DUMP_STACK:
					if(dumpStack())
						MessageBoxA(NULL, "Stack was dumped to file", "Dump", 0);
					else
						MessageBoxA(NULL, "Failed to dump stack", "Dump", 0);
					break;

				case ID_BUTTON_DUMP_CODE:
					if(dumpCode())
						MessageBoxA(NULL, "Code was dumped to file", "Dump", 0);
					else
						MessageBoxA(NULL, "Failed to dump code", "Dump", 0);
					break;

				case ID_BUTTON_DUMP_LOCAL:
					if(dumpLocal())
						MessageBoxA(NULL, "Local memory was dumped to file", "Dump", 0);
					else
						MessageBoxA(NULL, "Failed to dump local memory", "Dump", 0);
					break;

				case ID_BUTTON_HALT:
					doHaltExecution(gHaltExecution ^ 1);
					break;

				case ID_BUTTON_STEP:
					if(!gHaltExecution)
						doHaltExecution(1);
					gStepThread = curThreadPtr->threadId;
					gStepExecution = 1;
					break;

				case ID_THREAD_LIST_BOX:
					switch(HIWORD(wParam))
					{
						case LBN_SELCHANGE:
						{
							int index = SendMessage(gThreadListBox, LB_GETCARETINDEX, 0, 0);
							curThreadPtr = threadList[index];
							if(curThreadPtr)
								updateWindow();
							break;
						}
					}
			}
			break;

		case WM_VSCROLL:
		{
			SCROLLINFO si;
			si.cbSize = sizeof(si);
			si.fMask = SIF_ALL;
			GetScrollInfo((HWND)lParam, SB_CTL, &si);

			// Save the position for comparison later on.
			int yPos = si.nPos;
			switch (LOWORD(wParam))
			{
				// User clicked the top arrow.
				case SB_LINEUP:
					si.nPos -= 1;
					break;

				// User clicked the bottom arrow.
				case SB_LINEDOWN:
					si.nPos += 1;
					break;

				// User clicked the scroll bar shaft above the scroll box.
				case SB_PAGEUP:
					si.nPos -= si.nPage;
					break;

				// User clicked the scroll bar shaft below the scroll box.
				case SB_PAGEDOWN:
					si.nPos += si.nPage;
					break;

				// User dragged the scroll box.
				case SB_THUMBTRACK:
					si.nPos = si.nTrackPos;
					break;

				default:
					break;
			}

			// Set the position and then retrieve it. Due to adjustments
			// by Windows it may not be the same as the value set.
			si.fMask = SIF_POS;
			SetScrollInfo((HWND)lParam, SB_CTL, &si, TRUE);
			GetScrollInfo((HWND)lParam, SB_CTL, &si);

			// If the position has changed, scroll window and update it.
			if(si.nPos != yPos)
			{
				
			}
			break;
		}

		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	return 0;
}

void createValueLabel(int x, int* y, HWND* label, HWND* value, char* text)
{
	*label = CreateWindowA(
		"static", text,
		WS_CHILD | WS_VISIBLE,
		x, *y, 200, 16,
		gDebugHWND, (HMENU)1, NULL, NULL
	);
	*value = CreateWindowA(
		"static", "?",
		WS_CHILD | WS_VISIBLE,
		x+200, *y, 70, 16,
		gDebugHWND, (HMENU)1, NULL, NULL
	);
	if(!*label || !*value)
		fatalError(text);
	SendMessage(*label, WM_SETFONT, (WPARAM)hFont, true);
	SendMessage(*value, WM_SETFONT, (WPARAM)hFont, true);
	*y += 16;
}

void updateValue(HWND hwnd, uint32_t value)
{
	char str[32];
	sprintf(&str[0], "%.8LX", value);
	SetWindowTextA(hwnd, &str[0]);
}

void updateVMInfo(VMThread_t* thread)
{
	if(curThreadCopy.programId != thread->programId)
		updateValue(vmInfo.programIdValue,          (uint32_t)thread->programId);
	if(curThreadCopy.threadId != thread->threadId)
		updateValue(vmInfo.threadIdValue,           (uint32_t)thread->threadId);
	if(curThreadCopy.nextThread != thread->nextThread)
		updateValue(vmInfo.nextThreadValue,        (uint32_t)thread->nextThread);
	if(curThreadCopy.flags != thread->flags)
		updateValue(vmInfo.flagsValue,              (uint32_t)thread->flags);
	if(curThreadCopy.stackPointer != thread->stackPointer)
		updateValue(vmInfo.stackPointerValue,       (uint32_t)thread->stackPointer);
	if(curThreadCopy.instructionPointer != thread->instructionPointer)
		updateValue(vmInfo.instructionPointerValue, (uint32_t)thread->instructionPointer);
	if(curThreadCopy.programCounter != thread->programCounter)
		updateValue(vmInfo.programCounterValue,     (uint32_t)thread->programCounter);
	if(curThreadCopy.basePointer != thread->basePointer)
		updateValue(vmInfo.basePointerValue,        (uint32_t)thread->basePointer);
	if(curThreadCopy.stackSize != thread->stackSize)
		updateValue(vmInfo.stackSizeValue,          (uint32_t)thread->stackSize);
	if(curThreadCopy.stackMemConfig != thread->stackMemConfig)
		updateValue(vmInfo.stackMemConfigValue,     (uint32_t)thread->stackMemConfig);
	if(curThreadCopy.stack != thread->stack)
		updateValue(vmInfo.stackValue,              (uint32_t)thread->stack);
	if(curThreadCopy.codeSpaceSize != thread->codeSpaceSize)
		updateValue(vmInfo.codeSpaceSizeValue,      (uint32_t)thread->codeSpaceSize);
	if(curThreadCopy.codeSpaceMemConfig != thread->codeSpaceMemConfig)
		updateValue(vmInfo.codeSpaceMemConfigValue, (uint32_t)thread->codeSpaceMemConfig);
	if(curThreadCopy.codeSpace != thread->codeSpace)
		updateValue(vmInfo.codeSpaceValue,          (uint32_t)thread->codeSpace);
	if(curThreadCopy.programList != thread->programList)
		updateValue(vmInfo.programListValue,        (uint32_t)thread->programList);
	if(curThreadCopy.programCount != thread->programCount)
		updateValue(vmInfo.programCountValue,       (uint32_t)thread->programCount);
	if(curThreadCopy.codeSpaceTop != thread->codeSpaceTop)
		updateValue(vmInfo.codeSpaceTopValue,       (uint32_t)thread->codeSpaceTop);
	if(curThreadCopy.localMemSize != thread->localMemSize)
		updateValue(vmInfo.localMemSizeValue,       (uint32_t)thread->localMemSize);
	if(curThreadCopy.localMemConfig != thread->localMemConfig)
		updateValue(vmInfo.localMemConfigValue,     (uint32_t)thread->localMemConfig);
	if(curThreadCopy.localMem != thread->localMem)
		updateValue(vmInfo.localMemValue,           (uint32_t)thread->localMem);
	if(curThreadCopy.unknownStruct != thread->unknownStruct)
		updateValue(vmInfo.unknownStructValue,      (uint32_t)thread->unknownStruct);
	if(curThreadCopy.field_0x54 != thread->field_0x54)
		updateValue(vmInfo.field_0x54Value,         (uint32_t)thread->field_0x54);
	if(curThreadCopy.field_0x58 != thread->field_0x58)
		updateValue(vmInfo.field_0x58Value,         (uint32_t)thread->field_0x58);
	if(curThreadCopy.field_0x5c != thread->field_0x5c)
		updateValue(vmInfo.field_0x5cValue,         (uint32_t)thread->field_0x5c);
	if(curThreadCopy.field_0x60 != thread->field_0x60)
		updateValue(vmInfo.field_0x60Value,         (uint32_t)thread->field_0x60);

	// Find program name
	char* programName = NULL;
	VMProgramList_t* program = thread->programList;
	while(program)
	{
		if(program->location <= thread->instructionPointer &&
		   program->location + program->size >= thread->instructionPointer)
		{
			programName = program->filename;
			break;
		}
		program = program->prevProgram;
	}
	char pStr[128];
	snprintf(&pStr[0], 128, "Program: %s", programName ? programName : "?");
	SetWindowTextA(gProgramLabel, &pStr[0]);

	// Copy the current state of the thread
	memcpy(&curThreadCopy, thread, sizeof(VMThread_t));
}

HWND makeButton(char* title, int x, int y, int width, int height, int id)
{
	HWND button = CreateWindow(
		"BUTTON", title,
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
		x, y,
		width, height,
		gDebugHWND,
		(HMENU)id,
		NULL, NULL
	);
	SendMessage(button, WM_SETFONT, (WPARAM)hFont, true);
	return button;
}

bool createDebugWindow()
{
	if(initialised)
		return true;

	memset(&curThreadCopy, 0xFF, sizeof(VMThread_t));

	WNDCLASSEXA wclass;
	wclass.cbSize        = sizeof(WNDCLASSEXA);
	wclass.style         = CS_OWNDC;
	wclass.lpfnWndProc   = DLLWindowProc;
	wclass.cbClsExtra    = 0;
	wclass.cbWndExtra    = 0;
	wclass.hInstance     = gDllHModule;
	wclass.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	wclass.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wclass.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
	wclass.lpszMenuName  = NULL;
	wclass.lpszClassName = "classPatchDebugger";
	wclass.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
	gHWndClassAtom = RegisterClassExA(&wclass);
	if(!gHWndClassAtom)
	{
		fatalError("RegisterClassExA");
		return false;
	}

	gDebugHWND = CreateWindowExA(
		0,
		(LPCSTR)gHWndClassAtom,
		"Debugger",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		1000, 600,
		NULL, NULL,
		gDllHModule, NULL
	);
	if(!gDebugHWND)
	{
		fatalError("CreateWindowExA");
		return false;
	}

	hFont = CreateFontA(
		16, 0,
		0, 0, FW_DONTCARE,
		false, false, false,
		ANSI_CHARSET,
		OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		FIXED_PITCH | FF_MODERN, NULL
	);
	SendMessage(gDebugHWND, WM_SETFONT, (WPARAM)hFont, true);

	//
	// Create thread info fields
	//
	int y = 10;
	createValueLabel(120, &y, &vmInfo.programIdLabel,          &vmInfo.programIdValue,          "programId");
	createValueLabel(120, &y, &vmInfo.threadIdLabel,           &vmInfo.threadIdValue,           "threadId");
	createValueLabel(120, &y, &vmInfo.nextThreadLabel,         &vmInfo.nextThreadValue,         "nextThread");
	createValueLabel(120, &y, &vmInfo.flagsLabel,              &vmInfo.flagsValue,              "flags");
	createValueLabel(120, &y, &vmInfo.stackPointerLabel,       &vmInfo.stackPointerValue,       "stackPointer");
	createValueLabel(120, &y, &vmInfo.instructionPointerLabel, &vmInfo.instructionPointerValue, "instructionPointer");
	createValueLabel(120, &y, &vmInfo.programCounterLabel,     &vmInfo.programCounterValue,     "programCounter");
	createValueLabel(120, &y, &vmInfo.basePointerLabel,        &vmInfo.basePointerValue,        "basePointer");
	createValueLabel(120, &y, &vmInfo.stackSizeLabel,          &vmInfo.stackSizeValue,          "stackSize");
	createValueLabel(120, &y, &vmInfo.stackMemConfigLabel,     &vmInfo.stackMemConfigValue,     "stackMemConfig");
	createValueLabel(120, &y, &vmInfo.stackLabel,              &vmInfo.stackValue,              "stack");
	createValueLabel(120, &y, &vmInfo.codeSpaceSizeLabel,      &vmInfo.codeSpaceSizeValue,      "codeSpaceSize");
	createValueLabel(120, &y, &vmInfo.codeSpaceMemConfigLabel, &vmInfo.codeSpaceMemConfigValue, "codeSpaceMemConfig");
	createValueLabel(120, &y, &vmInfo.codeSpaceLabel,          &vmInfo.codeSpaceValue,          "codeSpace");
	createValueLabel(120, &y, &vmInfo.programListLabel,        &vmInfo.programListValue,        "programList");
	createValueLabel(120, &y, &vmInfo.programCountLabel,       &vmInfo.programCountValue,       "programCount");
	createValueLabel(120, &y, &vmInfo.codeSpaceTopLabel,       &vmInfo.codeSpaceTopValue,       "codeSpaceTop");
	createValueLabel(120, &y, &vmInfo.localMemSizeLabel,       &vmInfo.localMemSizeValue,       "localMemSize");
	createValueLabel(120, &y, &vmInfo.localMemConfigLabel,     &vmInfo.localMemConfigValue,     "localMemConfig");
	createValueLabel(120, &y, &vmInfo.localMemLabel,           &vmInfo.localMemValue,           "localMem");
	createValueLabel(120, &y, &vmInfo.unknownStructLabel,      &vmInfo.unknownStructValue,      "unknownStruct");
	createValueLabel(120, &y, &vmInfo.field_0x54Label,         &vmInfo.field_0x54Value,         "field_0x54");
	createValueLabel(120, &y, &vmInfo.field_0x58Label,         &vmInfo.field_0x58Value,         "field_0x58");
	createValueLabel(120, &y, &vmInfo.field_0x5cLabel,         &vmInfo.field_0x5cValue,         "field_0x5c");
	createValueLabel(120, &y, &vmInfo.field_0x60Label,         &vmInfo.field_0x60Value,         "field_0x60");

	gProgramLabel = CreateWindowA(
		"static", "Program: ?",
		WS_CHILD | WS_VISIBLE,
		120, y, 270, 16,
		gDebugHWND, (HMENU)1, NULL, NULL
	);
	SendMessage(gProgramLabel, WM_SETFONT, (WPARAM)hFont, true);

	//
	// Create buttons
	//
	int btnY = 310;
	gDumpStackButton = makeButton("Dump Stack", 10, btnY, 100, 30, ID_BUTTON_DUMP_STACK);
	btnY += 40;
	gDumpLocalButton = makeButton("Dump Code",  10, btnY, 100, 30, ID_BUTTON_DUMP_CODE);
	btnY += 40;
	gDumpLocalButton = makeButton("Dump Local", 10, btnY, 100, 30, ID_BUTTON_DUMP_LOCAL);
	btnY += 40;
	gHaltButton      = makeButton(gHaltExecution ? "Run" : "Halt", 10, btnY, 100, 30, ID_BUTTON_HALT);
	btnY += 40;
	gStepButton      = makeButton("Step",       10, btnY, 100, 30, ID_BUTTON_STEP);
	btnY += 40;

	//
	// Create thread list
	//
	gThreadListBox = CreateWindow(
		"LISTBOX", NULL,
		WS_VISIBLE | WS_CHILD | LBS_STANDARD | LBS_NOTIFY,
		10, 10,
		100, 300,
		gDebugHWND,
		(HMENU)ID_THREAD_LIST_BOX,
		gDllHModule, NULL
	);
	SendMessage(gThreadListBox, WM_SETFONT, (WPARAM)hFont, true);

	// Memory view
	gLocalMemoryList = CreateWindow(
		"LISTBOX", NULL,
		WS_VISIBLE | WS_CHILD | LBS_STANDARD | LBS_NOTIFY | WS_VSCROLL,
		400, 298,
		380, 258,
		gDebugHWND,
		(HMENU)ID_MEMORY_LIST_BOX,
		gDllHModule, NULL
	);
	SendMessage(gLocalMemoryList, WM_SETFONT, (WPARAM)hFont, true);
	gLocalMemoryScrollbar = CreateWindow(
		"SCROLLBAR", NULL,
		WS_VISIBLE | WS_CHILD | SBS_VERT,
		400+380, 298,
		18, 258,
		gDebugHWND,
		(HMENU)ID_MEMORY_SCROLLBAR,
		gDllHModule, NULL
	);
	
	// Stack view
	gStackList = CreateWindow(
		"LISTBOX", NULL,
		WS_VISIBLE | WS_CHILD | LBS_STANDARD | LBS_NOTIFY | WS_VSCROLL,
		803, 10,
		150, 546,
		gDebugHWND,
		(HMENU)ID_STACK_LIST_BOX,
		gDllHModule, NULL
	);
	SendMessage(gStackList, WM_SETFONT, (WPARAM)hFont, true);
	gStackScrollbar = CreateWindow(
		"SCROLLBAR", NULL,
		WS_VISIBLE | WS_CHILD | SBS_VERT,
		803+150, 10,
		18, 546,
		gDebugHWND,
		(HMENU)ID_STACK_SCROLLBAR,
		gDllHModule, NULL
	);

	// Disassembly view
	gDisassemblyList = CreateWindow(
		"LISTBOX", NULL,
		WS_VISIBLE | WS_CHILD | LBS_STANDARD | LBS_NOTIFY | WS_VSCROLL,
		400, 10,
		380, 258,
		gDebugHWND,
		(HMENU)ID_DISASSEMBLY_LIST_BOX,
		gDllHModule, NULL
	);
	SendMessage(gDisassemblyList, WM_SETFONT, (WPARAM)hFont, true);
	gDisassemblyScrollbar = CreateWindow(
		"SCROLLBAR", NULL,
		WS_VISIBLE | WS_CHILD | SBS_VERT,
		400+380, 10,
		18, 258,
		gDebugHWND,
		(HMENU)ID_DISASSEMBLY_SCROLLBAR,
		gDllHModule, NULL
	);

	//
	// Show window
	//
    ShowWindow(gDebugHWND, SW_SHOW);
    UpdateWindow(gDebugHWND);

    // A timer to regularily look for state updates
    SetTimer(gDebugHWND, ID_WINDOW_UPDATE_TIMER, 100, NULL);

    initialised = true;
}

void shutdownDebugger()
{
    DestroyWindow(gDebugHWND);
    UnregisterClassA((LPCSTR)gHWndClassAtom, gDllHModule);
}

void fatalError(char* error)
{
	char errBuf[1024];
	DWORD lastError = GetLastError();
	FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		lastError,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		&errBuf[0],
		1024,
		NULL
	);
	MessageBox(NULL, errBuf, error, 0);
	ExitProcess(1);
}
