//
// User interface code
//

#include "patch.h"

#define ID_WINDOW_UPDATE_TIMER 1001
#define ID_THREAD_LIST_BOX     2000
#define ID_BUTTON_DUMP_STACK   9000
#define ID_BUTTON_DUMP_CODE    9001
#define ID_BUTTON_DUMP_LOCAL   9002
#define ID_BUTTON_HALT         9003
#define ID_BUTTON_STEP         9004

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

// Thread list
HWND gThreadListBox;
VMThread_t curThreadCopy;    // Copy of the thread to prevent redraw of unchanged fields (can cause flickering otherwise)
VMThread_t* curThreadPtr;    // Pointer to the actual thread being displayed
VMThread_t* threadList[256]; // Threads as show in the thread list
int gThreadListCount = 0;    // Number of threads in the list

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

void updateWindow()
{
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
								updateVMInfo(curThreadPtr);
							break;
						}
					}
			}
			break;

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
		x+200, *y, 100, 16,
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
		800, 600,
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
		FIXED_PITCH | FF_MODERN, "Tahoma"
	);
	SendMessage(gDebugHWND, WM_SETFONT, (WPARAM)hFont, true);

	//
	// Create thread info fields
	//
	int y = 10;
	createValueLabel(120, &y, &vmInfo.programIdLabel,          &vmInfo.programIdValue,          "programId"); 
	createValueLabel(120, &y, &vmInfo.threadIdLabel,           &vmInfo.threadIdValue,           "threadId"); 
	createValueLabel(120, &y, &vmInfo.nextThreadLabel,         &vmInfo.nextThreadValue,        "nextThread"); 
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
	gHaltButton      = makeButton("Halt",       10, btnY, 100, 30, ID_BUTTON_HALT);
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
		NULL, NULL
	);
	SendMessage(gThreadListBox, WM_SETFONT, (WPARAM)hFont, true);

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
