#include "TrayUI.h"

#include "AppState.h"

#include "LoggingUI.h"
#include "MonitorConfig.h"
#include "Persistence.h"
#include "WindowTracking.h"

static HMENU GetMainOptionsMenu(HWND hWnd)
{
	HMENU menu = GetMenu(hWnd);
	return GetSubMenu(menu, 0);
}

enum MainWindowTabPage {
	MainWindowTabLog = 0,
	MainWindowTabLayouts = 1,
	MainWindowTabReadme = 2,
	MainWindowTabSettings = 3,
	MainWindowTabHistory = 4,
};

static int GetSelectedMainTab()
{
	HWND hTab = InstanceData::g_Instance._hMainTab;
	if (hTab == NULL) {
		return MainWindowTabLog;
	}

	int selection = TabCtrl_GetCurSel(hTab);
	return selection >= 0 ? selection : MainWindowTabLog;
}

static RECT GetMainTabContentRect(HWND hWnd)
{
	RECT rect = { 4, STATUS_HEIGHT + 8, 4, STATUS_HEIGHT + 8 };
	HWND hTab = InstanceData::g_Instance._hMainTab;
	if (hTab == NULL) {
		return rect;
	}

	GetClientRect(hTab, &rect);
	TabCtrl_AdjustRect(hTab, FALSE, &rect);
	MapWindowPoints(hTab, hWnd, reinterpret_cast<LPPOINT>(&rect), 2);
	return rect;
}

static void UpdateMainMenuChecks(HWND hWnd);

static void ScheduleDisplaySettleAfterUnlock(HWND hWnd)
{
	KillTimer(hWnd, DISPLAY_SETTLE_TIMER_ID);
	SetTimer(hWnd, DISPLAY_SETTLE_TIMER_ID, DISPLAY_SETTLE_MS, TimerCallback);
}

static void ShowChildControl(HWND hWnd, BOOL show)
{
	if (hWnd != NULL) {
		ShowWindow(hWnd, show ? SW_SHOW : SW_HIDE);
	}
}

static void SaveCurrentSettings()
{
	auto& inst = InstanceData::g_Instance;
	SaveSettings(inst.RestoreOnDisconnect,
		inst.PersistPositions,
		inst.LoggingEnabled,
		inst._RestoreRetryDelayMs,
		inst._RestoreRetryLimit,
		inst._HistoryTrackingEnabled);
}

static void SetCheckboxValue(HWND hWnd, BOOL checked)
{
	if (hWnd != NULL) {
		SendMessage(hWnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
	}
}

static BOOL GetCheckboxValue(HWND hWnd)
{
	return hWnd != NULL && SendMessage(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void SetIntegerControlValue(HWND hWnd, int value)
{
	if (hWnd != NULL) {
		TCHAR buffer[32];
		StringCchPrintf(buffer, _countof(buffer), _T("%d"), value);
		SetWindowText(hWnd, buffer);
	}
}

static void SyncSettingsControlsFromState()
{
	auto& inst = InstanceData::g_Instance;
	SetCheckboxValue(inst._hSettingsRestoreCheck, inst.RestoreOnDisconnect);
	SetCheckboxValue(inst._hSettingsAutostartCheck, IsAutostartEnabled());
	SetCheckboxValue(inst._hSettingsPersistCheck, inst.PersistPositions);
	SetCheckboxValue(inst._hSettingsLoggingCheck, inst.LoggingEnabled);
	SetCheckboxValue(inst._hSettingsHistoryCheck, inst._HistoryTrackingEnabled);
	SetIntegerControlValue(inst._hSettingsDelayEdit, inst._RestoreRetryDelayMs);
	SetIntegerControlValue(inst._hSettingsRetryEdit, inst._RestoreRetryLimit);
}

static BOOL TryReadSettingsInteger(HWND hWndOwner, HWND hEdit, LPCTSTR label,
	int minValue, int maxValue, int* valueOut)
{
	TCHAR buffer[32];
	GetWindowText(hEdit, buffer, _countof(buffer));

	TCHAR* endPtr = NULL;
	long parsed = _tcstol(buffer, &endPtr, 10);
	if (buffer[0] == '\0' || endPtr == NULL || *endPtr != '\0' || parsed < minValue || parsed > maxValue) {
		TCHAR message[256];
		StringCchPrintf(message, _countof(message),
			_T("%s must be a whole number between %d and %d."),
			label, minValue, maxValue);
		MessageBox(hWndOwner, message, _T("WinPosKeeper Settings"), MB_OK | MB_ICONWARNING);
		SetFocus(hEdit);
		SendMessage(hEdit, EM_SETSEL, 0, -1);
		return FALSE;
	}

	*valueOut = (int)parsed;
	return TRUE;
}

static void ApplySettingsFromControls(HWND hWnd)
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hSettingsDelayEdit == NULL || inst._hSettingsRetryEdit == NULL) {
		return;
	}

	int delayMs = inst._RestoreRetryDelayMs;
	int retryLimit = inst._RestoreRetryLimit;
	if (!TryReadSettingsInteger(hWnd, inst._hSettingsDelayEdit, _T("Verification pass delay"),
		MIN_RESTORE_RETRY_DELAY_MS, MAX_RESTORE_RETRY_DELAY_MS, &delayMs)) {
		return;
	}
	if (!TryReadSettingsInteger(hWnd, inst._hSettingsRetryEdit, _T("Additional verification passes"),
		MIN_RESTORE_RETRY_LIMIT, MAX_RESTORE_RETRY_LIMIT, &retryLimit)) {
		return;
	}

	BOOL newRestoreOnDisconnect = GetCheckboxValue(inst._hSettingsRestoreCheck);
	BOOL newAutostart = GetCheckboxValue(inst._hSettingsAutostartCheck);
	BOOL newPersistPositions = GetCheckboxValue(inst._hSettingsPersistCheck);
	BOOL newLoggingEnabled = GetCheckboxValue(inst._hSettingsLoggingCheck);
	BOOL newHistoryTrackingEnabled = GetCheckboxValue(inst._hSettingsHistoryCheck);
	BOOL oldAutostart = IsAutostartEnabled();
	BOOL oldLoggingEnabled = inst.LoggingEnabled;
	BOOL historyWasEnabled = inst._HistoryTrackingEnabled;
	BOOL persistenceWasDisabled = !inst.PersistPositions && newPersistPositions;
	BOOL historyJustEnabled = !historyWasEnabled && newHistoryTrackingEnabled;
	BOOL historyJustDisabled = historyWasEnabled && !newHistoryTrackingEnabled;

	BOOL changed =
		inst.RestoreOnDisconnect != newRestoreOnDisconnect ||
		oldAutostart != newAutostart ||
		inst.PersistPositions != newPersistPositions ||
		inst.LoggingEnabled != newLoggingEnabled ||
		inst._HistoryTrackingEnabled != newHistoryTrackingEnabled ||
		inst._RestoreRetryDelayMs != delayMs ||
		inst._RestoreRetryLimit != retryLimit;
	if (!changed) {
		SyncSettingsControlsFromState();
		return;
	}

	TCHAR summary[512];
	StringCchPrintf(summary, _countof(summary),
		_T("Settings applied: restore-on-disconnect=%s, autostart=%s, same-session disk recovery=%s, logging=%s, window history=%s, verification delay=%d ms, additional verification passes=%d"),
		newRestoreOnDisconnect ? _T("ON") : _T("OFF"),
		newAutostart ? _T("ON") : _T("OFF"),
		newPersistPositions ? _T("ON") : _T("OFF"),
		newLoggingEnabled ? _T("ON") : _T("OFF"),
		newHistoryTrackingEnabled ? _T("ON") : _T("OFF"),
		delayMs,
		retryLimit);

	if (oldLoggingEnabled && !newLoggingEnabled) {
		LogEvent(_T("INFO"), summary);
	}

	inst.RestoreOnDisconnect = newRestoreOnDisconnect;
	inst.PersistPositions = newPersistPositions;
	inst._RestoreRetryDelayMs = delayMs;
	inst._RestoreRetryLimit = retryLimit;
	inst._HistoryTrackingEnabled = newHistoryTrackingEnabled;
	if (oldAutostart != newAutostart) {
		SetAutostart(newAutostart);
	}
	inst.LoggingEnabled = newLoggingEnabled;
	if (historyJustDisabled) {
		ClearWindowHistoryTracking();
	}

	SaveCurrentSettings();
	UpdateMainMenuChecks(hWnd);
	UpdateLoggingUiState();
	SyncSettingsControlsFromState();
	if (inst.LoggingEnabled) {
		LogEvent(_T("INFO"), summary);
	}
	if (historyJustEnabled) {
		PrimeWindowHistoryTracking(_T("tracking enabled"));
	}
	if (persistenceWasDisabled) {
		inst.SaveToDisk(_T("persistence enabled from settings"));
	}
	UpdateStatusPanel();
}

static void UpdateMainTabVisibility(HWND hWnd)
{
	UNREFERENCED_PARAMETER(hWnd);
	int selectedTab = GetSelectedMainTab();
	BOOL showLog = selectedTab == MainWindowTabLog ? TRUE : FALSE;
	BOOL showLayouts = selectedTab == MainWindowTabLayouts ? TRUE : FALSE;
	BOOL showReadme = selectedTab == MainWindowTabReadme ? TRUE : FALSE;
	BOOL showSettings = selectedTab == MainWindowTabSettings ? TRUE : FALSE;
	BOOL showHistory = selectedTab == MainWindowTabHistory ? TRUE : FALSE;

	ShowChildControl(InstanceData::g_Instance._hLogList, showLog);
	ShowChildControl(InstanceData::g_Instance._hConfigList, showLayouts);
	ShowChildControl(InstanceData::g_Instance._hConfigSummary, showLayouts);
	ShowChildControl(InstanceData::g_Instance._hPlacementList, showLayouts);
	ShowChildControl(InstanceData::g_Instance._hReadmeView, showReadme);
	ShowChildControl(InstanceData::g_Instance._hSettingsIntro, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsRestoreCheck, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsAutostartCheck, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsPersistCheck, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsLoggingCheck, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsHistoryCheck, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsDelayLabel, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsDelayEdit, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsRetryLabel, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsRetryEdit, showSettings);
	ShowChildControl(InstanceData::g_Instance._hSettingsApplyButton, showSettings);
	ShowChildControl(InstanceData::g_Instance._hHistoryWindowList, showHistory);
	ShowChildControl(InstanceData::g_Instance._hHistorySummary, showHistory);
	ShowChildControl(InstanceData::g_Instance._hHistoryTimelineList, showHistory);
	ShowChildControl(InstanceData::g_Instance._hHistoryExportButton, showHistory);

	if (showLayouts) {
		RefreshPlacementInspector();
	}
	if (showReadme) {
		RefreshReadmeView();
	}
	if (showSettings) {
		SyncSettingsControlsFromState();
	}
	if (showHistory) {
		PurgeClosedWindowHistoryEntries();
		RefreshWindowHistoryInspector();
	}
}

static void LayoutMainWindow(HWND hWnd, int cx, int cy)
{
	int iconX = cx - 4 - STATUS_ICON_SIZE;
	int iconY = 4;
	int statusWidth = iconX - 8;
	if (statusWidth < 100) {
		statusWidth = 100;
	}

	if (InstanceData::g_Instance._hStatus)
		MoveWindow(InstanceData::g_Instance._hStatus, 4, 4, statusWidth, STATUS_HEIGHT, TRUE);
	if (InstanceData::g_Instance._hStatusIcon)
		MoveWindow(InstanceData::g_Instance._hStatusIcon, iconX, iconY, STATUS_ICON_SIZE, STATUS_ICON_SIZE, TRUE);

	int tabTop = STATUS_HEIGHT + 8;
	int tabHeight = cy - tabTop - 4;
	if (tabHeight < 80) {
		tabHeight = 80;
	}
	if (InstanceData::g_Instance._hMainTab) {
		MoveWindow(InstanceData::g_Instance._hMainTab, 4, tabTop, cx - 8, tabHeight, TRUE);
	}

	RECT contentRect = GetMainTabContentRect(hWnd);
	int contentWidth = contentRect.right - contentRect.left;
	int contentHeight = contentRect.bottom - contentRect.top;
	if (contentWidth < 100) {
		contentWidth = 100;
	}
	if (contentHeight < 80) {
		contentHeight = 80;
	}

	if (InstanceData::g_Instance._hLogList) {
		MoveWindow(InstanceData::g_Instance._hLogList,
			contentRect.left, contentRect.top, contentWidth, contentHeight, TRUE);
	}
	if (InstanceData::g_Instance._hReadmeView) {
		MoveWindow(InstanceData::g_Instance._hReadmeView,
			contentRect.left, contentRect.top, contentWidth, contentHeight, TRUE);
	}
	if (InstanceData::g_Instance._hSettingsIntro) {
		int introHeight = min(120, max(84, contentHeight / 3));
		int rowHeight = 22;
		int y = contentRect.top;
		int labelWidth = min(260, max(200, contentWidth / 2));
		int editWidth = 64;

		MoveWindow(InstanceData::g_Instance._hSettingsIntro,
			contentRect.left, y, contentWidth, introHeight, TRUE);
		y += introHeight + 10;

		MoveWindow(InstanceData::g_Instance._hSettingsRestoreCheck,
			contentRect.left, y, contentWidth, rowHeight, TRUE);
		y += rowHeight + 2;
		MoveWindow(InstanceData::g_Instance._hSettingsAutostartCheck,
			contentRect.left, y, contentWidth, rowHeight, TRUE);
		y += rowHeight + 2;
		MoveWindow(InstanceData::g_Instance._hSettingsPersistCheck,
			contentRect.left, y, contentWidth, rowHeight, TRUE);
		y += rowHeight + 2;
		MoveWindow(InstanceData::g_Instance._hSettingsLoggingCheck,
			contentRect.left, y, contentWidth, rowHeight, TRUE);
		y += rowHeight + 2;
		MoveWindow(InstanceData::g_Instance._hSettingsHistoryCheck,
			contentRect.left, y, contentWidth, rowHeight, TRUE);
		y += rowHeight + 10;

		MoveWindow(InstanceData::g_Instance._hSettingsDelayLabel,
			contentRect.left, y + 3, labelWidth, rowHeight, TRUE);
		MoveWindow(InstanceData::g_Instance._hSettingsDelayEdit,
			contentRect.left + labelWidth + 8, y, editWidth, rowHeight + 2, TRUE);
		y += rowHeight + 8;
		MoveWindow(InstanceData::g_Instance._hSettingsRetryLabel,
			contentRect.left, y + 3, labelWidth, rowHeight, TRUE);
		MoveWindow(InstanceData::g_Instance._hSettingsRetryEdit,
			contentRect.left + labelWidth + 8, y, editWidth, rowHeight + 2, TRUE);
		y += rowHeight + 12;
		MoveWindow(InstanceData::g_Instance._hSettingsApplyButton,
			contentRect.left, y, 132, rowHeight + 6, TRUE);
	}
	if (InstanceData::g_Instance._hHistoryWindowList) {
		int buttonHeight = 28;
		int minTimelineHeight = 96;
		int summaryHeight = min(96, max(60, contentHeight / 6));
		int selectorHeight = contentHeight - summaryHeight - buttonHeight - 24 - minTimelineHeight;
		selectorHeight = min(288, max(96, selectorHeight));
		int y = contentRect.top;

		MoveWindow(InstanceData::g_Instance._hHistoryWindowList,
			contentRect.left, y, contentWidth, selectorHeight, TRUE);
		y += selectorHeight + 8;
		MoveWindow(InstanceData::g_Instance._hHistorySummary,
			contentRect.left, y, contentWidth, summaryHeight, TRUE);
		y += summaryHeight + 8;
		MoveWindow(InstanceData::g_Instance._hHistoryExportButton,
			contentRect.left, y, 132, buttonHeight, TRUE);
		y += buttonHeight + 8;
		MoveWindow(InstanceData::g_Instance._hHistoryTimelineList,
			contentRect.left, y, contentWidth, max(80, contentRect.bottom - y), TRUE);
	}

	int selectorHeight = min(88, max(56, contentHeight / 6));
	int summaryHeight = min(110, max(78, contentHeight / 4));
	int detailsTop = contentRect.top + selectorHeight + 8;
	int detailsHeight = contentHeight - selectorHeight - 8;
	if (detailsHeight < 80) {
		detailsHeight = 80;
	}
	int placementsTop = detailsTop + summaryHeight + 8;
	int placementsHeight = detailsHeight - summaryHeight - 8;
	if (placementsHeight < 80) {
		placementsHeight = 80;
		summaryHeight = max(60, detailsHeight - placementsHeight - 8);
		placementsTop = detailsTop + summaryHeight + 8;
	}
	if (InstanceData::g_Instance._hConfigList) {
		MoveWindow(InstanceData::g_Instance._hConfigList,
			contentRect.left, contentRect.top, contentWidth, selectorHeight, TRUE);
	}
	if (InstanceData::g_Instance._hConfigSummary) {
		MoveWindow(InstanceData::g_Instance._hConfigSummary,
			contentRect.left, detailsTop, contentWidth, summaryHeight, TRUE);
	}
	if (InstanceData::g_Instance._hPlacementList) {
		MoveWindow(InstanceData::g_Instance._hPlacementList,
			contentRect.left, placementsTop, contentWidth, placementsHeight, TRUE);
	}
}

static void ApplyMenuChecks(HMENU menu)
{
	CheckMenuItem(menu, IDM_RESTORE_ON_DISCONNECT,
		InstanceData::g_Instance.RestoreOnDisconnect ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_AUTOSTART, IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_PERSIST_POSITIONS,
		InstanceData::g_Instance.PersistPositions ? MF_CHECKED : MF_UNCHECKED);
	CheckMenuItem(menu, IDM_ENABLE_LOGGING,
		InstanceData::g_Instance.LoggingEnabled ? MF_CHECKED : MF_UNCHECKED);
}

static void UpdateMainMenuChecks(HWND hWnd)
{
	ApplyMenuChecks(GetMainOptionsMenu(hWnd));
}

static void ShowTrayContextMenu(HWND hWnd, int x, int y)
{
	HMENU trayMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_TRAYMENU));
	if (trayMenu == NULL) {
		LogWin32Error(_T("WARNING"), _T("LoadMenu for tray popup"), GetLastError());
		return;
	}

	HMENU popup = GetSubMenu(trayMenu, 0);
	if (popup == NULL) {
		DestroyMenu(trayMenu);
		LOG_EVENT(_T("WARNING"), _T("Tray popup menu resource is missing its root submenu"));
		return;
	}

	ApplyMenuChecks(popup);
	SetForegroundWindow(hWnd);
	TrackPopupMenu(popup, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, 0, hWnd, NULL);
	DestroyMenu(trayMenu);
	PostMessage(hWnd, WM_NULL, 0, 0);
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
	lstrcpy(icon.szTip, _T("WinPosKeeper"));
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
	icex.dwICC = ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
	InitCommonControlsEx(&icex);

	HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW & ~WS_VISIBLE,
		CW_USEDEFAULT, 0, 700, 500, nullptr, nullptr, hInstance, nullptr);
	if (!hWnd)
	{
		return FALSE;
	}

	InstanceData::g_Instance._MainWnd = hWnd;
	InstanceData::g_Instance._hMainTab = CreateWindowEx(0, WC_TABCONTROL, _T(""),
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		4, STATUS_HEIGHT + 8, 690, 400,
		hWnd, (HMENU)IDC_MAIN_TAB, hInstance, NULL);
	InstanceData::g_Instance._hStatus = CreateWindowEx(0, _T("STATIC"), _T(""),
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		4, 4, 646, STATUS_HEIGHT,
		hWnd, NULL, hInstance, NULL);
	InstanceData::g_Instance._hStatusIcon = CreateWindowEx(0, _T("STATIC"), NULL,
		WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
		654, 4, STATUS_ICON_SIZE, STATUS_ICON_SIZE,
		hWnd, NULL, hInstance, NULL);
	InstanceData::g_Instance._hLogList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
		LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		4, STATUS_HEIGHT + 8, 690, 400,
		hWnd, NULL, hInstance, NULL);
	InstanceData::g_Instance._hConfigList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		4, STATUS_HEIGHT + 8, 220, 400,
		hWnd, (HMENU)IDC_CONFIG_LIST, hInstance, NULL);
	InstanceData::g_Instance._hConfigSummary = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T(""),
		WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
		232, STATUS_HEIGHT + 8, 462, 92,
		hWnd, (HMENU)IDC_CONFIG_SUMMARY, hInstance, NULL);
	InstanceData::g_Instance._hPlacementList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		232, STATUS_HEIGHT + 108, 462, 296,
		hWnd, (HMENU)IDC_PLACEMENT_LIST, hInstance, NULL);
	InstanceData::g_Instance._hReadmeView = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T(""),
		WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
		4, STATUS_HEIGHT + 8, 690, 400,
		hWnd, (HMENU)IDC_README_VIEW, hInstance, NULL);
	InstanceData::g_Instance._hSettingsIntro = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"),
		_T("The tray/menu toggles are mirrored here together with the restore verification timing.\r\n\r\n")
		_T("Verification delay is the pause before each full verification pass. Additional verification passes controls how many extra full recheck/reapply cycles can run after the initial restore. Windows that enter size/move are skipped for the remaining passes.\r\n\r\n")
		_T("Disk persistence only keeps same-session restart recovery data for still-running windows. Window history keeps an in-memory HWND timeline and can export merged TSV diagnostics."),
		WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
		4, STATUS_HEIGHT + 8, 690, 120,
		hWnd, (HMENU)IDC_SETTINGS_INTRO, hInstance, NULL);
	InstanceData::g_Instance._hSettingsRestoreCheck = CreateWindowEx(0, _T("BUTTON"),
		_T("Allow restore attempts when the monitor count drops (disconnect / undock)"),
		WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		4, STATUS_HEIGHT + 136, 690, 20,
		hWnd, (HMENU)IDC_SETTINGS_RESTORE_CHECK, hInstance, NULL);
	InstanceData::g_Instance._hSettingsAutostartCheck = CreateWindowEx(0, _T("BUTTON"),
		_T("Start WinPosKeeper with Windows"),
		WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		4, STATUS_HEIGHT + 160, 690, 20,
		hWnd, (HMENU)IDC_SETTINGS_AUTOSTART_CHECK, hInstance, NULL);
	InstanceData::g_Instance._hSettingsPersistCheck = CreateWindowEx(0, _T("BUTTON"),
		_T("Keep same-session restart recovery data on disk for app restarts"),
		WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		4, STATUS_HEIGHT + 184, 690, 20,
		hWnd, (HMENU)IDC_SETTINGS_PERSIST_CHECK, hInstance, NULL);
	InstanceData::g_Instance._hSettingsLoggingCheck = CreateWindowEx(0, _T("BUTTON"),
		_T("Keep diagnostic messages in the Log tab"),
		WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		4, STATUS_HEIGHT + 208, 690, 20,
		hWnd, (HMENU)IDC_SETTINGS_LOGGING_CHECK, hInstance, NULL);
	InstanceData::g_Instance._hSettingsHistoryCheck = CreateWindowEx(0, _T("BUTTON"),
		_T("Keep in-memory window history and enable TSV export for investigation"),
		WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
		4, STATUS_HEIGHT + 232, 690, 20,
		hWnd, (HMENU)IDC_SETTINGS_HISTORY_CHECK, hInstance, NULL);
	InstanceData::g_Instance._hSettingsDelayLabel = CreateWindowEx(0, _T("STATIC"),
		_T("Verification pass delay (ms):"),
		WS_CHILD,
		4, STATUS_HEIGHT + 260, 240, 20,
		hWnd, (HMENU)IDC_SETTINGS_DELAY_LABEL, hInstance, NULL);
	InstanceData::g_Instance._hSettingsDelayEdit = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T("2000"),
		WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
		248, STATUS_HEIGHT + 256, 64, 24,
		hWnd, (HMENU)IDC_SETTINGS_DELAY_EDIT, hInstance, NULL);
	InstanceData::g_Instance._hSettingsRetryLabel = CreateWindowEx(0, _T("STATIC"),
		_T("Additional full verification passes:"),
		WS_CHILD,
		4, STATUS_HEIGHT + 288, 240, 20,
		hWnd, (HMENU)IDC_SETTINGS_RETRY_LABEL, hInstance, NULL);
	InstanceData::g_Instance._hSettingsRetryEdit = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T("4"),
		WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
		248, STATUS_HEIGHT + 284, 64, 24,
		hWnd, (HMENU)IDC_SETTINGS_RETRY_EDIT, hInstance, NULL);
	InstanceData::g_Instance._hSettingsApplyButton = CreateWindowEx(0, _T("BUTTON"), _T("Apply Settings"),
		WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		4, STATUS_HEIGHT + 316, 132, 28,
		hWnd, (HMENU)IDC_SETTINGS_APPLY, hInstance, NULL);
	InstanceData::g_Instance._hHistoryWindowList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		4, STATUS_HEIGHT + 8, 690, 96,
		hWnd, (HMENU)IDC_HISTORY_WINDOW_LIST, hInstance, NULL);
	InstanceData::g_Instance._hHistorySummary = CreateWindowEx(WS_EX_CLIENTEDGE, _T("EDIT"), _T(""),
		WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
		4, STATUS_HEIGHT + 112, 690, 110,
		hWnd, (HMENU)IDC_HISTORY_SUMMARY, hInstance, NULL);
	InstanceData::g_Instance._hHistoryExportButton = CreateWindowEx(0, _T("BUTTON"), _T("Export TSV..."),
		WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
		4, STATUS_HEIGHT + 230, 132, 28,
		hWnd, (HMENU)IDC_HISTORY_EXPORT, hInstance, NULL);
	InstanceData::g_Instance._hHistoryTimelineList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
		WS_CHILD | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
		4, STATUS_HEIGHT + 266, 690, 200,
		hWnd, (HMENU)IDC_HISTORY_TIMELINE_LIST, hInstance, NULL);

	if (InstanceData::g_Instance._hMainTab == NULL || InstanceData::g_Instance._hStatus == NULL ||
		InstanceData::g_Instance._hStatusIcon == NULL || InstanceData::g_Instance._hLogList == NULL ||
		InstanceData::g_Instance._hConfigList == NULL || InstanceData::g_Instance._hConfigSummary == NULL ||
		InstanceData::g_Instance._hPlacementList == NULL || InstanceData::g_Instance._hReadmeView == NULL ||
		InstanceData::g_Instance._hSettingsIntro == NULL || InstanceData::g_Instance._hSettingsRestoreCheck == NULL ||
		InstanceData::g_Instance._hSettingsAutostartCheck == NULL || InstanceData::g_Instance._hSettingsPersistCheck == NULL ||
		InstanceData::g_Instance._hSettingsLoggingCheck == NULL || InstanceData::g_Instance._hSettingsHistoryCheck == NULL ||
		InstanceData::g_Instance._hSettingsDelayLabel == NULL ||
		InstanceData::g_Instance._hSettingsDelayEdit == NULL || InstanceData::g_Instance._hSettingsRetryLabel == NULL ||
		InstanceData::g_Instance._hSettingsRetryEdit == NULL || InstanceData::g_Instance._hSettingsApplyButton == NULL ||
		InstanceData::g_Instance._hHistoryWindowList == NULL || InstanceData::g_Instance._hHistorySummary == NULL ||
		InstanceData::g_Instance._hHistoryExportButton == NULL || InstanceData::g_Instance._hHistoryTimelineList == NULL) {
		LogWin32Error(_T("ERROR"), _T("CreateWindowEx for main window child controls"), GetLastError());
		return FALSE;
	}

	if (WTSRegisterSessionNotification(hWnd, NOTIFY_FOR_THIS_SESSION)) {
		InstanceData::g_Instance._SessionNotificationsRegistered = TRUE;
	}
	else {
		LogWin32Error(_T("WARNING"), _T("WTSRegisterSessionNotification"), GetLastError());
	}

	TCITEM tie = {};
	tie.mask = TCIF_TEXT;
	tie.pszText = const_cast<LPTSTR>(_T("Log"));
	TabCtrl_InsertItem(InstanceData::g_Instance._hMainTab, MainWindowTabLog, &tie);
	tie.pszText = const_cast<LPTSTR>(_T("Layouts"));
	TabCtrl_InsertItem(InstanceData::g_Instance._hMainTab, MainWindowTabLayouts, &tie);
	tie.pszText = const_cast<LPTSTR>(_T("README"));
	TabCtrl_InsertItem(InstanceData::g_Instance._hMainTab, MainWindowTabReadme, &tie);
	tie.pszText = const_cast<LPTSTR>(_T("Settings"));
	TabCtrl_InsertItem(InstanceData::g_Instance._hMainTab, MainWindowTabSettings, &tie);
	tie.pszText = const_cast<LPTSTR>(_T("History"));
	TabCtrl_InsertItem(InstanceData::g_Instance._hMainTab, MainWindowTabHistory, &tie);
	TabCtrl_SetCurSel(InstanceData::g_Instance._hMainTab, MainWindowTabLog);
	InstanceData::g_Instance._InspectorSelectedConfigHash = 0;
	SendMessage(InstanceData::g_Instance._hStatusIcon, STM_SETIMAGE, IMAGE_ICON,
		(LPARAM)LoadAppIconSized(hInstance, STATUS_ICON_SIZE, STATUS_ICON_SIZE));

	HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	HDC hdc = GetDC(hWnd);
	int logFontHeight = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
	ReleaseDC(hWnd, hdc);
	InstanceData::g_Instance._hLogFont = CreateFont(
		logFontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		FIXED_PITCH | FF_MODERN, _T("Consolas"));
	SendMessage(InstanceData::g_Instance._hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hMainTab, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hLogList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hConfigList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hConfigSummary, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hPlacementList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hReadmeView, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsIntro, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsRestoreCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsAutostartCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsPersistCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsLoggingCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsHistoryCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsDelayLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsDelayEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsRetryLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsRetryEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hSettingsApplyButton, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hHistoryWindowList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hHistorySummary, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hHistoryTimelineList, WM_SETFONT,
		(WPARAM)(InstanceData::g_Instance._hLogFont != NULL ? InstanceData::g_Instance._hLogFont : hFont), TRUE);
	SendMessage(InstanceData::g_Instance._hHistoryExportButton, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(InstanceData::g_Instance._hLogList, LB_SETHORIZONTALEXTENT, 4096, 0);
	SendMessage(InstanceData::g_Instance._hConfigList, LB_SETHORIZONTALEXTENT, 12288, 0);
	SendMessage(InstanceData::g_Instance._hPlacementList, LB_SETHORIZONTALEXTENT, 8192, 0);
	SendMessage(InstanceData::g_Instance._hHistoryWindowList, LB_SETHORIZONTALEXTENT, 12288, 0);
	SendMessage(InstanceData::g_Instance._hHistoryTimelineList, LB_SETHORIZONTALEXTENT, 16384, 0);
	EnableWindow(InstanceData::g_Instance._hHistoryExportButton, FALSE);

	LoadSettings(InstanceData::g_Instance.RestoreOnDisconnect,
		InstanceData::g_Instance.PersistPositions,
		InstanceData::g_Instance.LoggingEnabled,
		InstanceData::g_Instance._RestoreRetryDelayMs,
		InstanceData::g_Instance._RestoreRetryLimit,
		InstanceData::g_Instance._HistoryTrackingEnabled);
	SyncSettingsControlsFromState();
	InstanceData::g_Instance.LoadFromDisk();

	InstanceData::g_Instance._ConfigHash = ComputeMonitorConfigHash();
	InstanceData::g_Instance._NumMonitors = GetCurrentMonitorCount();
	ProcessDesktopWindows();
	PrimeWindowHistoryTracking(_T("startup snapshot"));
	InstanceData::g_Instance._Hook = HookDisplayChange();
	InstanceData::g_Instance._MoveSizeHook = HookWindowMoveSize();
	WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

	AddTrayIcon(hWnd);
	SetTimer(hWnd, PERSIST_TIMER_ID, PERSIST_TIMER_MS, PersistTimerCallback);
	UpdateMainMenuChecks(hWnd);
	UpdateLoggingUiState();
	UpdateStatusPanel();
	RECT clientRect = {};
	GetClientRect(hWnd, &clientRect);
	LayoutMainWindow(hWnd, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
	RefreshReadmeView();
	UpdateMainTabVisibility(hWnd);
	LOG_EVENT(_T("INFO"), _T("WinPosKeeper started"));

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
		KillTimer(hWnd, VERIFY_TIMER_ID);
		CancelPendingRestores();
		InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock = FALSE;
		InstanceData::g_Instance.InChangingState = true;
		if (InstanceData::g_Instance._SessionLocked) {
			if (!InstanceData::g_Instance._DeferredDisplayChangeUntilUnlock) {
				LOG_EVENT(_T("SESSION"), _T("WM_DISPLAYCHANGE will be applied after session unlock"));
			}
			InstanceData::g_Instance._DeferredDisplayChangeUntilUnlock = TRUE;
		}
		else {
			InstanceData::g_Instance._DeferredDisplayChangeUntilUnlock = FALSE;
			ScheduleDisplaySettleAfterUnlock(hWnd);
		}
		break;
	case WM_WTSSESSION_CHANGE:
		if (wParam == WTS_SESSION_LOCK) {
			if (!InstanceData::g_Instance._SessionLocked) {
				InstanceData::g_Instance._SessionLocked = TRUE;
				LOG_EVENT(_T("SESSION"), _T("Session locked"));
			}
			return 0;
		}
		if (wParam == WTS_SESSION_UNLOCK) {
			BOOL wasLocked = InstanceData::g_Instance._SessionLocked;
			InstanceData::g_Instance._SessionLocked = FALSE;
			if (wasLocked) {
				LOG_EVENT(_T("SESSION"), _T("Session unlocked"));
			}
			if (InstanceData::g_Instance._DeferredDisplayChangeUntilUnlock) {
				InstanceData::g_Instance._DeferredDisplayChangeUntilUnlock = FALSE;
				LOG_EVENT(_T("SESSION"), _T("Applying deferred display change after session unlock"));
				ScheduleDisplaySettleAfterUnlock(hWnd);
			}
			else if (InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock) {
				InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock = FALSE;
				LOG_EVENT(_T("SESSION"), _T("Resuming deferred restore verification after session unlock"));
				SetTimer(hWnd, VERIFY_TIMER_ID, 1, NULL);
			}
			return 0;
		}
		break;
	case WM_COMMAND:
		{
			if ((HWND)lParam == InstanceData::g_Instance._hConfigList && HIWORD(wParam) == LBN_SELCHANGE) {
				RefreshPlacementInspector();
				return 0;
			}
			if ((HWND)lParam == InstanceData::g_Instance._hHistoryWindowList && HIWORD(wParam) == LBN_SELCHANGE) {
				RefreshWindowHistoryInspector();
				return 0;
			}
			if (LOWORD(wParam) == IDC_SETTINGS_APPLY && HIWORD(wParam) == BN_CLICKED) {
				ApplySettingsFromControls(hWnd);
				return 0;
			}
			if (LOWORD(wParam) == IDC_HISTORY_EXPORT && HIWORD(wParam) == BN_CLICKED) {
				ExportSelectedWindowHistory(hWnd);
				return 0;
			}

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
			case IDM_HIDEWINDOW:
				ShowWindow(hWnd, SW_HIDE);
				break;
			case IDM_RESTORE_ON_DISCONNECT:
				InstanceData::g_Instance.RestoreOnDisconnect = !InstanceData::g_Instance.RestoreOnDisconnect;
				UpdateMainMenuChecks(hWnd);
				SaveCurrentSettings();
				SyncSettingsControlsFromState();
				LOG_EVENT(_T("INFO"), InstanceData::g_Instance.RestoreOnDisconnect
					? _T("Restore-on-disconnect enabled")
					: _T("Restore-on-disconnect disabled"));
				UpdateStatusPanel();
				break;
			case IDM_AUTOSTART:
				{
					BOOL enabled = IsAutostartEnabled();
					SetAutostart(!enabled);
					UpdateMainMenuChecks(hWnd);
					SyncSettingsControlsFromState();
					LOG_EVENT(_T("INFO"), !enabled ? _T("Autostart enabled") : _T("Autostart disabled"));
					UpdateStatusPanel();
				}
				break;
			case IDM_PERSIST_POSITIONS:
				InstanceData::g_Instance.PersistPositions = !InstanceData::g_Instance.PersistPositions;
				UpdateMainMenuChecks(hWnd);
				SaveCurrentSettings();
				SyncSettingsControlsFromState();
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
				UpdateMainMenuChecks(hWnd);
				SaveCurrentSettings();
				SyncSettingsControlsFromState();
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
	case WM_NOTIFY:
		if (((LPNMHDR)lParam)->idFrom == IDC_MAIN_TAB && ((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
			UpdateMainTabVisibility(hWnd);
			return 0;
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
				ShowTrayContextMenu(hWnd, x, y);
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
			if (InstanceData::g_Instance._SessionLocked) {
				if (!InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock) {
					LOG_EVENT(_T("SESSION"), _T("Restore verification deferred until session unlock"));
				}
				InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock = TRUE;
				return 0;
			}
			InstanceData::g_Instance._DeferredRestoreVerifyUntilUnlock = FALSE;
			if (InstanceData::g_Instance._AwaitingRestoreRetry) {
				RetryPendingRestores();
			}
			else {
				VerifyRestoredWindows();
			}
		}
		else if (wParam == HISTORY_FLUSH_TIMER_ID) {
			KillTimer(hWnd, HISTORY_FLUSH_TIMER_ID);
			FlushPendingWindowHistoryEntries();
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
			if (InstanceData::g_Instance._SessionNotificationsRegistered) {
				if (!WTSUnRegisterSessionNotification(hWnd)) {
					LogWin32Error(_T("WARNING"), _T("WTSUnRegisterSessionNotification"), GetLastError());
				}
				InstanceData::g_Instance._SessionNotificationsRegistered = FALSE;
			}
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