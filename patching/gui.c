//
// User interface code
//

#include "patch.h"

HMODULE gDllHModule;
HWND gDebugHWND;
ATOM gHWndClassAtom;
bool initialised = false;

HWND gGUIThreadCount;
HWND gGUITextArea;
char txtbuf[4096];

#define TIMER1 1001

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

LRESULT WINAPI DLLWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg)
	{
		case WM_DESTROY:
			PostQuitMessage(0);
			break;

		case WM_TIMER:
			if(wParam == TIMER1)
			{
				int threadCount = countThreads();
				sprintf(&txtbuf[0], "Threads: %d", threadCount);
				SetWindowTextA(gGUIThreadCount, &txtbuf[0]);
				//sprintf(&txtbuf[0], "IP: %.8LX\nIP: %.8LX\nIP: %.8LX\nIP: %.8LX\n", getThreadIP(0), getThreadIP(1), getThreadIP(2), getThreadIP(3));
				if(*gVMThread)
				{
					printThreadInfo(&txtbuf[0], *gVMThread);
					SendMessage(gGUITextArea, WM_SETTEXT, 0, &txtbuf[0]);
				}
			}
		// handle other messages.

		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
	}

	return 0;
}

bool createDebugWindow()
{
	if(initialised)
		return true;

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
		(LPCTSTR)gHWndClassAtom,
		"Debugger",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		800, 600,
		NULL, NULL,
		gDllHModule, NULL
	);
	if(!gHWndClassAtom)
	{
		fatalError("CreateWindowExA");
		return false;
	}

	gGUIThreadCount = CreateWindowA(
		"static", "Threads: ?",
		WS_CHILD | WS_VISIBLE,
		10, 10, 128, 16, 
		gDebugHWND, (HMENU)1, NULL, NULL
	);
	gGUITextArea = CreateWindowEx(
		0, "EDIT",
		NULL,
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | DS_FIXEDSYS, 
		10, 26, 400, 500,
		gDebugHWND, (HMENU)1000, NULL, NULL
	);

    ShowWindow(gDebugHWND, SW_SHOW);
    UpdateWindow(gDebugHWND);

    SetTimer(gDebugHWND, TIMER1, 100, NULL);

    initialised = true;
}

void shutdownDebugger()
{
    DestroyWindow(gDebugHWND);
    UnregisterClassA((LPCTSTR)gHWndClassAtom, gDllHModule);
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
