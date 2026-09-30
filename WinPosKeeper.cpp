// WinPosKeeper.cpp
//
// Based on MonitorKeeper by Garr Godfrey (https://github.com/hunkydoryrepair/MonitorKeeper).
// License: MIT License
//
// Entry point and shared globals for the WinPosKeeper application.

#include "AppState.h"

#include "LoggingUI.h"
#include "TrayUI.h"

HINSTANCE hInst = NULL;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
UINT WM_TASKBARCREATED = 0;

InstanceData InstanceData::g_Instance;

// Brings up the main window of the instance that is already running. Returns FALSE if that
// instance has no main window yet (it is still starting up).
static BOOL ActivateRunningInstance()
{
	HWND existing = FindWindow(szWindowClass, NULL);
	if (existing == NULL) {
		return FALSE;
	}

	DWORD processId = 0;
	GetWindowThreadProcessId(existing, &processId);
	AllowSetForegroundWindow(processId);
	PostMessage(existing, WM_COMMAND, MAKEWPARAM(IDM_SHOWWINDOW, 0), 0);
	return TRUE;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	ResetStartupDiagnostics();

	if (LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING) == 0) {
		DWORD error = GetLastError();
		ReportStartupFailure(_T("LoadStringW(IDS_APP_TITLE)"),
			error != ERROR_SUCCESS ? error : ERROR_RESOURCE_NAME_NOT_FOUND,
			_T("Could not load the application title resource."));
		return FALSE;
	}
	if (LoadStringW(hInstance, IDC_WINPOSKEEPER, szWindowClass, MAX_LOADSTRING) == 0) {
		DWORD error = GetLastError();
		ReportStartupFailure(_T("LoadStringW(IDC_WINPOSKEEPER)"),
			error != ERROR_SUCCESS ? error : ERROR_RESOURCE_NAME_NOT_FOUND,
			_T("Could not load the main window class name resource."));
		return FALSE;
	}

	if (InstanceData::g_Instance.AlreadyRunning) {
		if (!ActivateRunningInstance()) {
			MessageBox(NULL, _T("WinPosKeeper is already running. Use its tray icon to open it."),
				szTitle, MB_OK | MB_ICONINFORMATION);
		}
		return 0;
	}

	if (MyRegisterClass(hInstance) == 0) {
		DWORD error = GetLastError();
		ReportStartupFailure(_T("RegisterClassExW"), error,
			_T("The main window class could not be registered."));
		return FALSE;
	}

	if (!InitInstance(hInstance, nCmdShow))
	{
		return FALSE;
	}

	HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_WINPOSKEEPER));
	MSG msg;

	while (GetMessage(&msg, nullptr, 0, 0))
	{
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int)msg.wParam;
}