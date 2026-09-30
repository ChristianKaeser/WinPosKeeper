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

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	ResetStartupDiagnostics();

	if (InstanceData::g_Instance.AlreadyRunning) {
		ReportStartupFailure(_T("Single-instance startup guard"), ERROR_SUCCESS,
			_T("Another WinPosKeeper instance is already running. Use the tray icon or exit the existing instance first."));
		return FALSE;
	}

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