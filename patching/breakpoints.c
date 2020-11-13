//
// Breakpoints
//

#include "patch.h"

#define ID_BREAKPOINT_ADD     1000
#define ID_BREAKPOINT_REMOVE  1001
#define ID_BREAKPOINT_LIST    1002
#define ID_BREAKPOINT_EXECUTE 1004
#define ID_BREAKPOINT_OPCODE  1005
#define ID_BREAKPOINT_READ    1006
#define ID_BREAKPOINT_WRITE   1007
#define ID_BREAKPOINT_NAME    1008
#define ID_BREAKPOINT_VALUE   1009

HWND gBreakpointWindow;
ATOM gBreakpointAtom;

// Buttons
HWND gBreakpointAdd;
HWND gBreakpointRemove;

// Radio buttons
HWND gBreakpointTypeExecute; // When execution reaches a certain address
HWND gBreakpointTypeOpcode;  // When a certain opcode is executed
HWND gBreakpointTypeRead;    // When a certain address is read from
HWND gBreakpointTypeWrite;   // When a certain address is written to

// Textboxes
HWND gBreakpointName;       // Breakpoint name
HWND gBreakpointNameLabel;  // Breakpoint name label
HWND gBreakpointValue;      // Breakpoint opcode/address (Depending on type)
HWND gBreakpointValueLabel; // Breakpoint opcode/address label

// List
HWND gBreakpointList; // List of breakpoints

// Actual breakpoints
Breakpoint_t gBreakpoints[32];

// Current breakpoint being edited
Breakpoint_t* curBreakpoint;

// List index to breakpoint index
int gBreakpointListIndices[32];

WNDPROC orgValueProc = NULL;

uint32_t parseHex(char* str)
{
	uint32_t val = 0;
	char c = *str;
	str++;
	while(c)
	{
		if(c >= 'A')
			val = (val << 4) | (c - 'A' + 0xA);
		else
			val = (val << 4) | (c - '0');
		c = *str;
		str++;
	}
	return val;
}

LRESULT CALLBACK ValueProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg)
	{
		// Only allow hex characters
		case WM_CHAR:
			if(!((wParam >= '0' && wParam <= '9') ||
			   (wParam >= 'A' && wParam <= 'F') ||
			   (wParam >= 'a' && wParam <= 'f') ||
			   wParam == VK_RETURN ||
			   wParam == VK_DELETE ||
			   wParam == VK_BACK))
			{
				return 0;
			}
			break;
	}
	return CallWindowProc(orgValueProc, hwnd, msg, wParam, lParam);
}

LRESULT WINAPI BreakpointProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg)
	{
		case WM_CLOSE:
			hideBreakpointsWindow();
			break;

		case WM_TIMER:
			break;

		case WM_CREATE:
			break;

		case WM_COMMAND:
			switch(LOWORD(wParam))
			{
				case ID_BREAKPOINT_NAME:
					if(HIWORD(wParam) == EN_CHANGE)
					{
						GetWindowText(gBreakpointName, &curBreakpoint->name[0], 64);
						populateBreakpointList();
					}
					break;

				case ID_BREAKPOINT_VALUE:
					if(HIWORD(wParam) == EN_CHANGE)
					{
						char valueText[10];
						GetWindowText(gBreakpointValue, &valueText[0], 9);
						uint32_t value = parseHex(&valueText[0]);
						curBreakpoint->value = value;
						populateBreakpointList();
					}
					break;

				case ID_BREAKPOINT_ADD:
					curBreakpoint = getFreeBreakpoint();
					if(!curBreakpoint)
						break;
					curBreakpoint->inUse = true;
					curBreakpoint->type = 1;
					curBreakpoint->enabled = false;
					curBreakpoint->name[0] = '\0';
					curBreakpoint->value = 0;
					setBreakpointsWindow(curBreakpoint);
					break;
				case ID_BREAKPOINT_EXECUTE:
				case ID_BREAKPOINT_OPCODE:
				case ID_BREAKPOINT_READ:
				case ID_BREAKPOINT_WRITE:
					switch(HIWORD(wParam))
					{
						case BN_CLICKED:
							if(LOWORD(wParam) == ID_BREAKPOINT_EXECUTE)
							{
								curBreakpoint->type = 1;
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_EXECUTE, BM_SETCHECK, 1, 0);
							}
							else
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_EXECUTE, BM_SETCHECK, 0, 0);
							if(LOWORD(wParam) == ID_BREAKPOINT_OPCODE)
							{
								curBreakpoint->type = 2;
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_OPCODE, BM_SETCHECK, 1, 0);
							}
							else
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_OPCODE, BM_SETCHECK, 0, 0);
							if(LOWORD(wParam) == ID_BREAKPOINT_READ)
							{
								curBreakpoint->type = 3;
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_READ, BM_SETCHECK, 1, 0);
							}
							else
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_READ, BM_SETCHECK, 0, 0);
							if(LOWORD(wParam) == ID_BREAKPOINT_WRITE)
							{
								curBreakpoint->type = 4;
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_WRITE, BM_SETCHECK, 1, 0);
							}
							else
								SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_WRITE, BM_SETCHECK, 0, 0);
							populateBreakpointList();
							break;
					}
					break;

				case ID_BREAKPOINT_LIST:
					switch(HIWORD(wParam))
					{
						case LBN_SELCHANGE:
						{
							int index = SendMessage(gBreakpointList, LB_GETCARETINDEX, 0, 0);
							setBreakpointsWindow(&gBreakpoints[gBreakpointListIndices[index]]);
							populateBreakpointList();
							break;
						}
					}
					break;
			}

		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}

bool createBreakpointsWindow()
{
	memset(&gBreakpoints[0], 0, sizeof(Breakpoint_t) * 32);

	WNDCLASSEXA wclass;
	wclass.cbSize        = sizeof(WNDCLASSEXA);
	wclass.style         = CS_OWNDC;
	wclass.lpfnWndProc   = BreakpointProc;
	wclass.cbClsExtra    = 0;
	wclass.cbWndExtra    = 0;
	wclass.hInstance     = gDllHModule;
	wclass.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
	wclass.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wclass.hbrBackground = (HBRUSH)COLOR_APPWORKSPACE;
	wclass.lpszMenuName  = NULL;
	wclass.lpszClassName = "classPatchBreakpoints";
	wclass.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);
	gBreakpointAtom = RegisterClassExA(&wclass);
	if(!gBreakpointAtom)
	{
		fatalError("RegisterClassExA");
		return false;
	}

	gBreakpointWindow = CreateWindowExA(
		0,
		(LPCSTR)gBreakpointAtom,
		"Breakpoints",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT,
		500, 230,
		NULL, NULL,
		gDllHModule, NULL
	);
	if(!gBreakpointWindow)
	{
		fatalError("CreateWindowExA");
		return false;
	}

	// Breakpoint list
	gBreakpointList = CreateWindowEx(
		WS_EX_CLIENTEDGE,
		"LISTBOX", NULL,
		WS_VISIBLE | WS_CHILD | LBS_STANDARD | LBS_NOTIFY,
		10, 10,
		250, 190,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_LIST,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointList, WM_SETFONT, (WPARAM)gFont, true);

	// Buttons
	gBreakpointAdd =    makeButton("Add",    gBreakpointWindow, 270, 160, 100, 30, ID_BREAKPOINT_ADD);
	gBreakpointRemove = makeButton("Remove", gBreakpointWindow, 375, 160, 100, 30, ID_BREAKPOINT_REMOVE);

	// Radio buttons
	gBreakpointTypeExecute = CreateWindow(
		"BUTTON", "Execute",
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
		270, 10,
		100, 20,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_EXECUTE,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointTypeExecute, WM_SETFONT, (WPARAM)gFont, true);
	gBreakpointTypeOpcode = CreateWindow(
		"BUTTON", "Opcode",
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
		270, 30,
		100, 20,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_OPCODE,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointTypeOpcode, WM_SETFONT, (WPARAM)gFont, true);
	gBreakpointTypeRead = CreateWindow(
		"BUTTON", "Read",
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
		370, 10,
		100, 20,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_READ,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointTypeRead, WM_SETFONT, (WPARAM)gFont, true);
	gBreakpointTypeWrite = CreateWindow(
		"BUTTON", "Write",
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
		370, 30,
		100, 20,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_WRITE,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointTypeWrite, WM_SETFONT, (WPARAM)gFont, true);

	// Inputs
	gBreakpointNameLabel = CreateWindow(
		"STATIC", "Name",
		WS_CHILD | WS_VISIBLE,
		270, 60,
		50, 16,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_NAME,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointNameLabel, WM_SETFONT, (WPARAM)gFont, true);
	gBreakpointName = CreateWindowEx(
		WS_EX_CLIENTEDGE,
		"EDIT", NULL,
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT,
		270, 76,
		150, 25,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_NAME,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointName, WM_SETFONT, (WPARAM)gFont, true);

	gBreakpointValueLabel = CreateWindow(
		"STATIC", "Value",
		WS_CHILD | WS_VISIBLE,
		270, 105,
		50, 16,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_NAME,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointValueLabel, WM_SETFONT, (WPARAM)gFont, true);
	gBreakpointValue = CreateWindowEx(
		WS_EX_CLIENTEDGE,
		"EDIT", NULL,
		WS_TABSTOP | WS_VISIBLE | WS_CHILD | WS_BORDER | ES_RIGHT | ES_UPPERCASE,
		270, 121,
		150, 25,
		gBreakpointWindow,
		(HMENU)ID_BREAKPOINT_VALUE,
		gDllHModule, NULL
	);
	SendMessage(gBreakpointValue, WM_SETFONT, (WPARAM)gFont, true);
	orgValueProc = SetWindowLongPtr(gBreakpointValue, GWLP_WNDPROC, ValueProc);
	SendMessage(gBreakpointValue, EM_SETLIMITTEXT, 8, 0);

	disableBreakpointsInputs();

	return true;
}

void disableBreakpointsInputs()
{
	EnableWindow(gBreakpointRemove, false);
	EnableWindow(gBreakpointTypeExecute, false);
	EnableWindow(gBreakpointTypeOpcode, false);
	EnableWindow(gBreakpointTypeRead, false);
	EnableWindow(gBreakpointTypeWrite, false);
	EnableWindow(gBreakpointName, false);
	EnableWindow(gBreakpointNameLabel, false);
	EnableWindow(gBreakpointValue, false);
	EnableWindow(gBreakpointValueLabel, false);
	populateBreakpointList();
}

void showBreakpointsWindow()
{
	ShowWindow(gBreakpointWindow, SW_SHOW);
	populateBreakpointList();
}

void hideBreakpointsWindow()
{
	ShowWindow(gBreakpointWindow, SW_HIDE);
}

void populateBreakpointList()
{
	// Delete previous content
	SendMessage(gBreakpointList, LB_RESETCONTENT, 0, 0);

	char str[128];
	int index = 0;
	int selectedIndex = 0;
	for(int i = 0; i < 32; i++)
	{
		if(!gBreakpoints[i].inUse)
			continue;
		char type;
		switch(gBreakpoints[i].type)
		{
			case 1: type = 'E'; break;
			case 2: type = 'O'; break;
			case 3: type = 'R'; break;
			case 4: type = 'W'; break;
			case 5: type = '?'; break;
		}
		snprintf(&str[0], 128, "%c: %.8LX %s", type, gBreakpoints[i].value, &gBreakpoints[i].name[0]);
		gBreakpointListIndices[index] = i;
		if(&gBreakpoints[i] == curBreakpoint)
			selectedIndex = index;
		SendMessage(gBreakpointList, LB_INSERTSTRING, index++, (LPARAM)&str[0]);
	}
	SendMessage(gBreakpointList, LB_SETCURSEL, selectedIndex, 0);
}

Breakpoint_t* getFreeBreakpoint()
{
	for(int i = 0; i < 32; i++)
	{
		if(!gBreakpoints[i].inUse)
			return &gBreakpoints[i];
	}
	return NULL;
}

void setBreakpointsWindow(Breakpoint_t* breakpoint)
{
	curBreakpoint = breakpoint;

	EnableWindow(gBreakpointRemove, true);
	EnableWindow(gBreakpointTypeExecute, true);
	EnableWindow(gBreakpointTypeOpcode, true);
	EnableWindow(gBreakpointTypeRead, true);
	EnableWindow(gBreakpointTypeWrite, true);
	EnableWindow(gBreakpointName, true);
	EnableWindow(gBreakpointNameLabel, true);
	EnableWindow(gBreakpointValue, true);
	EnableWindow(gBreakpointValueLabel, true);

	if(breakpoint->type == 1)
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_EXECUTE, BM_SETCHECK, 1, 0);
	else
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_EXECUTE, BM_SETCHECK, 0, 0);
	if(breakpoint->type == 2)
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_OPCODE, BM_SETCHECK, 1, 0);
	else
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_OPCODE, BM_SETCHECK, 0, 0);
	if(breakpoint->type == 3)
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_READ, BM_SETCHECK, 1, 0);
	else
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_READ, BM_SETCHECK, 0, 0);
	if(breakpoint->type == 4)
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_WRITE, BM_SETCHECK, 1, 0);
	else
		SendDlgItemMessage(gBreakpointWindow, ID_BREAKPOINT_WRITE, BM_SETCHECK, 0, 0);

	if(breakpoint->name)
		SetWindowTextA(gBreakpointName, breakpoint->name);
	else
		SetWindowTextA(gBreakpointName, "");
	char value[32];
	snprintf(&value[0], 32, "%.LX", breakpoint->value);
	SetWindowTextA(gBreakpointValue, &value[0]);

	populateBreakpointList();
}
