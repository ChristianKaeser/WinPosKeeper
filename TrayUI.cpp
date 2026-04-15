#include "TrayUI.h"

#include "LoggingUI.h"
#include "MonitorConfig.h"
#include "Persistence.h"
#include "WindowTracking.h"

static HMENU GetNotifyMenu(HWND hWnd)
{
	HMENU menu = GetMenu(hWnd);
	return GetSubMenu(menu, 1);
}

static void LayoutMainWindow(HWND hWnd, int cx, int cy)
{
	int iconX = cx - 4 - STATUS_ICON_SIZE;
	int iconY = 8;
	int statusWidth = iconX - 8;
	if (statusWidth < 100) {
		statusWidth = 100;
	}

	if (InstanceData::g_Instance._hStatus)
		MoveWindow(InstanceData::g_Instance._hStatus, 4, 4, statusWidth, STATUS_HEIGHT, TRUE);
	if (InstanceData::g_Instance._hStatusIcon)
		MoveWindow(InstanceData::g_Instance._hStatusIcon, iconX, iconY, STATUS_ICON_SIZE, STATUS_ICON_SIZE, TRUE);
	if (InstanceData::g_Instance._hLogList)
		MoveWindow(InstanceData::g_Instance._hLogList, 4, STATUS_HEIGHT + 8, cx - 8, cy - STATUS_HEIGHT - 12, TRUE);
}

static void UpdateMenuChecks(HWND hWnd)
{
	HMENU menu = GetNotifyMenu(hWnd);
	CheckMenuItem(menu, IDM_RESTORE_ON_DISCONNECT,
		InstanceData::g_Instance.RestoreOnDisconnect ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_AUTOSTART, IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_PERSIST_POSITIONS,
		InstanceData::g_Instance.PersistPositions ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_ENABLE_LOGGING,
		InstanceData::g_Instance.LoggingEnabled ? MF_CHECKED : MF_UNCHECKED);
}

void ShowMainWindow(HWND hWnd)
{
	ShowWindow(hWnd, SW_RESTORE);
	SetForegroundWindow(hWnd);
	UpdateWindow(hWnd);
}

void AddTrayIcon(HWND hWnd)
{
	NOTIFYICONDATA icon = {};
	icon.cbSize = sizeof(icon);
	icon.hWnd = hWnd;
	icon.uID = 1;
	icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	icon.uCallbackMessage = WM_USER + 100;
	icon.hIcon = LoadAppIcon(hInst, TRUE);
	lstrcpy(icon.szTip, _T("Monitor Keeper"));
	Shell_NotifyIcon(NIM_DELETE, &icon);
	if (!Shell_NotifyIcon(NIM_ADD, &icon)) {
		LogWin32Error(_T("WARNING"), _T("Shell_NotifyIcon(NIM_ADD)"), GetLastError());
		return;
	}

	icon.uVersion = NOTIFYICON_VERSION_4;
	if (!Shell_NotifyIcon(NIM_SETVERSION, &icon)) {
		LogWin32Error(_T("WARNING"), _T("Shell_NotifyIcon(NIM_SETVERSION)"), GetLastError());
	}
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXW wcex = {};

	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadAppIcon(hInstance, FALSE);
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_MONITORKEEPER);
	wcex.lpszClassName = szWindowClass;
	wcex.hIconSm = LoadAppIcon(wcex.hInstance, TRUE);

	return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	UNREFERENCED_PARAMETER(nCmdShow);
	hInst = hInstance;

	INITCOMMONCONTROLSEX icex = {};
	icex.dwSize = sizeof(icex);
	icex.dwICC = ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icex);

	HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW & ~WS_VISIBLE,
		CW_USEDEFAULT, 0, 700, 500, nullptr, nullptr, hInstance, nullptr);
	if (!hWnd)
	{
		return FALSE;
	}

	InstanceData::g_Instance._MainWnd = hWnd;
	InstanceData::g_Instance._hStatus = CreateWindowEx(0, _T("STATIC"), _T(""),
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		4, 4, 646, STATUS_HEIGHT,
		hWnd, NULL, hInstance, NULL);
	InstanceData::g_Instance._hStatusIcon = CreateWindowEx(0, _T("STATIC"), NULL,
		WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
		654, 8, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
		hWnd, NULL, hInstance, NULL);
	InstanceData::g_Instance._hLogList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
		LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		4, STATUS_HEIGHT + 8, 690, 400,
		hWnd, NULL, hInstance, NULL);

	if (InstanceData::g_Instance._hStatus == NULL || InstanceData::g_Instance._hStatusIcon == NULL || InstanceData::g_Instance._hLogList == NULL) {
		LogWin32Error(_T("ERROR"), _T("CreateWindowEx for main window child controls"), GetLastError());
		return FALSE;
	}
	SendMessage(InstanceData::g_Instance._hStatusIcon, STM_SETIMAGE, IMAGE_ICON, (LPARAM)LoadAppIcon(hInstance, FALSE));

	HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	HDC hdc = GetDC(hWnd);
	int logFontHeight = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
	ReleaseDC(hWnd, hdc);
	InstanceData::g_Instance._hLogFont = CreateFont(
		logFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		FIXED_PITCH | FF_MODERN, _T("Consolas"));
	SendMessage(InstanceData::g_Instance._hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hLogList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hLogList, LB_SETHORIZONTALEXTENT, 4096, 0);

	LoadSettings(InstanceData::g_Instance.RestoreOnDisconnect,
		InstanceData::g_Instance.PersistPositions,
		InstanceData::g_Instance.LoggingEnabled);
	InstanceData::g_Instance.LoadFromDisk();

	ProcessDesktopWindows();
	InstanceData::g_Instance._ConfigHash = ComputeMonitorConfigHash();
	InstanceData::g_Instance._NumMonitors = GetCurrentMonitorCount();
	InstanceData::g_Instance._Hook = HookDisplayChange();
	WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

	AddTrayIcon(hWnd);
	SetTimer(hWnd, PERSIST_TIMER_ID, PERSIST_TIMER_MS, PersistTimerCallback);
	UpdateMenuChecks(hWnd);
	UpdateLoggingUiState();
	UpdateStatusPanel();
	RECT clientRect = {};
	GetClientRect(hWnd, &clientRect);
	LayoutMainWindow(hWnd, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
	LOG_EVENT(_T("INFO"), _T("MonitorKeeper started"));

	return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DISPLAYCHANGE:
		{
			TCHAR monitorSummary[1024];
			GetCurrentMonitorSummary(monitorSummary, _countof(monitorSummary));
			LOG_EVENTF(_T("CONFIG"), _T("WM_DISPLAYCHANGE received: bpp=%u primary=%dx%d, %s"),
				(UINT)wParam, LOWORD(lParam), HIWORD(lParam), monitorSummary);
		}
		KillTimer(hWnd, CAPTURE_TIMER_ID);
		KillTimer(hWnd, DISPLAY_SETTLE_TIMER_ID);
		InstanceData::g_Instance.InChangingState = true;
		SetTimer(hWnd, DISPLAY_SETTLE_TIMER_ID, DISPLAY_SETTLE_MS, TimerCallback);
		break;
	case WM_COMMAND:
		{
			int wmId = LOWORD(wParam);
			switch (wmId)
			{
			case IDM_ABOUT:
				DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
				break;
			case IDM_EXIT:
				DestroyWindow(hWnd);
				break;
			case IDM_SHOWWINDOW:
				ShowMainWindow(hWnd);
				break;
			case IDM_RESTORE_ON_DISCONNECT:
				InstanceData::g_Instance.RestoreOnDisconnect = !InstanceData::g_Instance.RestoreOnDisconnect;
				UpdateMenuChecks(hWnd);
				SaveSettings(InstanceData::g_Instance.RestoreOnDisconnect,
					InstanceData::g_Instance.PersistPositions,
					InstanceData::g_Instance.LoggingEnabled);
				LOG_EVENT(_T("INFO"), InstanceData::g_Instance.RestoreOnDisconnect
					? _T("Restore-on-disconnect enabled")
					: _T("Restore-on-disconnect disabled"));
				UpdateStatusPanel();
				break;
			case IDM_AUTOSTART:
				{
					BOOL enabled = IsAutostartEnabled();
					SetAutostart(!enabled);
					UpdateMenuChecks(hWnd);
					LOG_EVENT(_T("INFO"), !enabled ? _T("Autostart enabled") : _T("Autostart disabled"));
					UpdateStatusPanel();
				}
				break;
			case IDM_PERSIST_POSITIONS:
				InstanceData::g_Instance.PersistPositions = !InstanceData::g_Instance.PersistPositions;
				UpdateMenuChecks(hWnd);
				SaveSettings(InstanceData::g_Instance.RestoreOnDisconnect,
					InstanceData::g_Instance.PersistPositions,
					InstanceData::g_Instance.LoggingEnabled);
				LOG_EVENT(_T("INFO"), InstanceData::g_Instance.PersistPositions
					? _T("Disk persistence enabled")
					: _T("Disk persistence disabled"));
				if (InstanceData::g_Instance.PersistPositions) {
					InstanceData::g_Instance.SaveToDisk(_T("persistence enabled from menu"));
				}
				UpdateStatusPanel();
				break;
			case IDM_ENABLE_LOGGING:
				if (InstanceData::g_Instance.LoggingEnabled) {
					LogEvent(_T("INFO"), _T("Event logging disabled"));
					InstanceData::g_Instance.LoggingEnabled = FALSE;
				}
				else {
					InstanceData::g_Instance.LoggingEnabled = TRUE;
					LogEvent(_T("INFO"), _T("Event logging enabled"));
				}
				UpdateMenuChecks(hWnd);
				SaveSettings(InstanceData::g_Instance.RestoreOnDisconnect,
					InstanceData::g_Instance.PersistPositions,
					InstanceData::g_Instance.LoggingEnabled);
				UpdateLoggingUiState();
				UpdateStatusPanel();
				break;
			case IDM_CLEAR_LOG:
				SendMessage(InstanceData::g_Instance._hLogList, LB_RESETCONTENT, 0, 0);
				LOG_EVENT(_T("INFO"), _T("Log cleared"));
				break;
			case IDM_COPY_LOG_ENTRY:
				{
					int index = (int)SendMessage(InstanceData::g_Instance._hLogList, LB_GETCURSEL, 0, 0);
					if (index == LB_ERR) {
						LOG_EVENT(_T("WARNING"), _T("Copy Entry requested with no selected log row"));
						break;
					}
					CopyTextToClipboard(hWnd, GetLogEntryText(InstanceData::g_Instance._hLogList, index));
				}
				break;
			case IDM_COPY_ALL_LOG:
				CopyTextToClipboard(hWnd, GetAllLogText(InstanceData::g_Instance._hLogList));
				break;
			default:
				return DefWindowProc(hWnd, message, wParam, lParam);
			}
		}
		break;
	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		return 0;
	case WM_CONTEXTMENU:
		if ((HWND)wParam == InstanceData::g_Instance._hLogList) {
			ShowLogContextMenu(hWnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
			return 0;
		}
		break;
	case (WM_USER + 100):
		{
			UINT nMsg = LOWORD(lParam);
			if (nMsg == WM_LBUTTONDBLCLK || nMsg == NIN_SELECT || nMsg == NIN_KEYSELECT) {
				ShowMainWindow(hWnd);
			}
			else if (nMsg == WM_CONTEXTMENU || nMsg == WM_RBUTTONUP) {
				int x = GET_X_LPARAM(wParam);
				int y = GET_Y_LPARAM(wParam);
				HMENU menu = GetNotifyMenu(hWnd);
				SetForegroundWindow(hWnd);
				TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, 0, hWnd, NULL);
				PostMessage(hWnd, WM_NULL, 0, 0);
			}
		}
		break;
	case WM_SIZE:
		{
			int cx = LOWORD(lParam);
			int cy = HIWORD(lParam);
			LayoutMainWindow(hWnd, cx, cy);
		}
		break;
	case WM_TIMER:
		if (wParam == VERIFY_TIMER_ID) {
			KillTimer(hWnd, VERIFY_TIMER_ID);
			VerifyRestoredWindows();
		}
		break;
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			BeginPaint(hWnd, &ps);
			EndPaint(hWnd, &ps);
		}
		break;
	case WM_DESTROY:
		{
			InstanceData::g_Instance.SaveToDisk(_T("application exit"));
			NOTIFYICONDATA icon = {};
			icon.cbSize = sizeof(icon);
			icon.hWnd = hWnd;
			icon.uID = 1;
			Shell_NotifyIcon(NIM_DELETE, &icon);
			PostQuitMessage(0);
		}
		break;
	default:
		if (message == WM_TASKBARCREATED && WM_TASKBARCREATED != 0) {
			LOG_EVENT(_T("INFO"), _T("Explorer restarted - re-adding tray icon"));
			AddTrayIcon(hWnd);
			return 0;
		}
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)LoadAppIcon(hInst, FALSE));
		SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)LoadAppIcon(hInst, TRUE));
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}