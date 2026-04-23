#include "WindowTracking.h"

#include "AppState.h"

#include "LoggingUI.h"
#include "MonitorConfig.h"

SavedWindowData::SavedWindowData()
{
	m_wndClass[0] = '\0';
	m_processPath[0] = '\0';
	m_windowTitle[0] = '\0';
	m_hwnd = NULL;
	m_processId = 0;
	m_nUnusedCount = 1;
	m_lastRestoreError = ERROR_SUCCESS;
	m_retryPending = FALSE;
}

static void ReadWindowIdentity(HWND hwnd, TCHAR* wndClass, size_t cchWndClass,
	TCHAR* processPath, size_t cchProcessPath,
	TCHAR* windowTitle, size_t cchWindowTitle,
	DWORD* processId)
{
	if (wndClass != NULL && cchWndClass > 0) {
		wndClass[0] = '\0';
	}
	if (processPath != NULL && cchProcessPath > 0) {
		processPath[0] = '\0';
	}
	if (windowTitle != NULL && cchWindowTitle > 0) {
		windowTitle[0] = '\0';
	}
	if (processId != NULL) {
		*processId = 0;
	}

	if (hwnd == NULL || !IsWindow(hwnd)) {
		return;
	}

	if (wndClass != NULL && cchWndClass > 0) {
		RealGetWindowClass(hwnd, wndClass, (int)cchWndClass);
	}
	if (windowTitle != NULL && cchWindowTitle > 0) {
		GetWindowText(hwnd, windowTitle, (int)cchWindowTitle);
	}

	DWORD localProcessId = 0;
	GetWindowThreadProcessId(hwnd, &localProcessId);
	if (processId != NULL) {
		*processId = localProcessId;
	}
	if (processPath == NULL || cchProcessPath == 0 || localProcessId == 0) {
		return;
	}

	HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, localProcessId);
	if (hProcess == NULL) {
		StringCchCopy(processPath, cchProcessPath, _T("<access denied>"));
		return;
	}

	DWORD cch = (DWORD)cchProcessPath;
	if (!QueryFullProcessImageName(hProcess, 0, processPath, &cch)) {
		StringCchCopy(processPath, cchProcessPath, _T("<access denied>"));
	}
	CloseHandle(hProcess);
}

BOOL SavedWindowData::SetData(HWND hwnd, UINT64 configHash)
{
	m_hwnd = hwnd;
	m_nUnusedCount = 0;
	ReadWindowIdentity(hwnd,
		m_wndClass, _countof(m_wndClass),
		m_processPath, _countof(m_processPath),
		m_windowTitle, _countof(m_windowTitle),
		&m_processId);

	WINDOWPLACEMENT wp = {};
	wp.length = sizeof(WINDOWPLACEMENT);
	if (!GetWindowPlacement(hwnd, &wp)) {
		LogWin32Error(_T("WARNING"), _T("GetWindowPlacement while capturing window"), GetLastError());
		return FALSE;
	}

	m_placements[configHash] = wp;
	while (m_placements.size() > MAX_CONFIGSLOTS) {
		m_placements.erase(m_placements.begin());
	}

	return TRUE;
}

BOOL SavedWindowData::HasPlacement(UINT64 configHash) const
{
	return m_placements.find(configHash) != m_placements.end();
}

BOOL SavedWindowData::RestoreWindow(UINT64 configHash)
{
	TCHAR szTempClass[40];
	DWORD actualProcessId = 0;
	m_lastRestoreError = ERROR_SUCCESS;
	m_retryPending = FALSE;
	auto it = m_placements.find(configHash);
	if (it == m_placements.end()) return FALSE;

	if (!IsWindow(m_hwnd)) {
		TCHAR identity[512];
		FormatWindowIdentity(NULL, m_processId, m_wndClass, m_processPath, m_windowTitle, identity, _countof(identity));
		LOG_EVENTF(_T("WARNING"), _T("Skipped restore: %s, saved HWND is no longer valid"), identity);
		return FALSE;
	}

	GetWindowThreadProcessId(m_hwnd, &actualProcessId);
	if (m_processId != 0 && actualProcessId != m_processId) {
		TCHAR identity[512];
		FormatWindowIdentity(m_hwnd, m_processId, m_wndClass, m_processPath, m_windowTitle, identity, _countof(identity));
		LOG_EVENTF(_T("WARNING"),
			_T("Skipped restore: saved pid=%lu, current pid=%lu, %s"),
			m_processId, actualProcessId, identity);
		return FALSE;
	}

	if (it->second.length != sizeof(WINDOWPLACEMENT))
		return FALSE;

	WINDOWPLACEMENT actual = {};
	actual.length = sizeof(WINDOWPLACEMENT);
	if (GetWindowPlacement(m_hwnd, &actual)) {
		if (!WindowPlacementNeedsRestore(it->second, actual)) {
			return FALSE;
		}
	}
	else {
		LogWin32Error(_T("WARNING"), _T("GetWindowPlacement while deciding restore necessity"), GetLastError());
	}

	RealGetWindowClass(m_hwnd, szTempClass, sizeof(szTempClass) / sizeof(TCHAR));
	if (lstrcmp(szTempClass, m_wndClass) != 0) {
		TCHAR identity[512];
		FormatWindowIdentity(m_hwnd, m_processId, m_wndClass, m_processPath, m_windowTitle, identity, _countof(identity));
		LOG_EVENTF(_T("WARNING"),
			_T("Skipped restore: saved class=\"%s\", current class=\"%s\", %s"),
			m_wndClass, szTempClass, identity);
		return FALSE;
	}

	WINDOWPLACEMENT place = it->second;
	TCHAR identity[512];
	FormatWindowIdentity(m_hwnd, m_processId, m_wndClass, m_processPath, m_windowTitle, identity, _countof(identity));
	LOG_EVENTF(_T("RESTORE"),
		_T("%s -> (%d,%d %dx%d) %s"),
		identity,
		place.rcNormalPosition.left, place.rcNormalPosition.top,
		place.rcNormalPosition.right - place.rcNormalPosition.left,
		place.rcNormalPosition.bottom - place.rcNormalPosition.top,
		TranslateShowCommand(place.showCmd));

	if (place.showCmd == SW_MAXIMIZE) {
		place.showCmd = SW_SHOWNOACTIVATE;
		if (!SetWindowPlacement(m_hwnd, &place)) {
			m_lastRestoreError = GetLastError();
			LogWin32Error(_T("WARNING"), _T("Initial SetWindowPlacement for maximized window"), m_lastRestoreError);
		}
		place.showCmd = SW_MAXIMIZE;
	}
	else if (place.showCmd == SW_MINIMIZE || place.showCmd == SW_SHOWMINIMIZED) {
		place.showCmd = SW_SHOWMINNOACTIVE;
	}
	else if (place.showCmd == SW_NORMAL) {
		place.showCmd = SW_SHOWNOACTIVATE;
	}
	place.flags &= ~WPF_SETMINPOSITION;
	place.flags |= WPF_ASYNCWINDOWPLACEMENT;

	if (!SetWindowPlacement(m_hwnd, &place)) {
		m_lastRestoreError = GetLastError();
		LogWin32Error(_T("WARNING"), _T("SetWindowPlacement while restoring window"), m_lastRestoreError);
		return FALSE;
	}
	m_lastRestoreError = ERROR_SUCCESS;
	return TRUE;
}

ConfigSnapshotInfo::ConfigSnapshotInfo()
{
	lastSavedUtc.dwLowDateTime = 0;
	lastSavedUtc.dwHighDateTime = 0;
	windowCount = 0;
}

static void UpdateSnapshotForCurrentConfig(UINT64 configHash, DWORD windowCount)
{
	auto& inst = InstanceData::g_Instance;
	ConfigSnapshotInfo& snapshot = inst._ConfigSnapshots[configHash];
	snapshot.lastSavedUtc = inst._LastCaptureUtc;
	snapshot.windowCount = windowCount;
	GetCurrentMonitorLayout(snapshot.monitorLayout);
}

InstanceData::InstanceData()
{
	_Hook = NULL;
	_ConfigHash = 0;
	_NumMonitors = 0;
	_WindowData.resize(32);
	_ConfigIds.clear();
	_NextConfigId = 1;
	_ConfigSnapshots.clear();
	_MainWnd = NULL;
	_hMainTab = NULL;
	_hStatus = NULL;
	_hStatusIcon = NULL;
	_hLogList = NULL;
	_hConfigList = NULL;
	_hConfigSummary = NULL;
	_hPlacementList = NULL;
	_hReadmeView = NULL;
	_hLogFont = NULL;
	InChangingState = false;
	RestoreOnDisconnect = true;
	PersistPositions = false;
	LoggingEnabled = true;
	_RestoreRetryCount = 0;
	_AwaitingRestoreRetry = FALSE;
	_InspectorSelectedConfigHash = 0;
	_LastCaptureUtc.dwLowDateTime = 0;
	_LastCaptureUtc.dwHighDateTime = 0;
	_LastPersistUtc.dwLowDateTime = 0;
	_LastPersistUtc.dwHighDateTime = 0;

#define APPLICATION_INSTANCE_MUTEX_NAME L"{f7ef2518-1a96-11ec-9621-0242ac130002}"
	_MutexSingleInstance = ::CreateMutex(NULL, TRUE, APPLICATION_INSTANCE_MUTEX_NAME);
	AlreadyRunning = ::GetLastError() == ERROR_ALREADY_EXISTS;
}

InstanceData::~InstanceData()
{
	Shutdown();
}

void InstanceData::Shutdown()
{
	if (_Hook != NULL) UnhookWinEvent(_Hook);
	_Hook = NULL;

	_WindowData.clear();
	_ConfigIds.clear();
	_NextConfigId = 1;
	_ConfigSnapshots.clear();
	_InspectorConfigHashes.clear();
	_InspectorSelectedConfigHash = 0;
	_ReadmeText.clear();

	if (_hLogFont != NULL)
	{
		DeleteObject(_hLogFont);
		_hLogFont = NULL;
	}

	if (_MutexSingleInstance)
	{
		::ReleaseMutex(_MutexSingleInstance);
		::CloseHandle(_MutexSingleInstance);
		_MutexSingleInstance = NULL;
	}
}

void InstanceData::TagWindowsUnused()
{
	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd != NULL && wd.m_nUnusedCount < 100)
		{
			wd.m_nUnusedCount++;
		}
	}
}

int InstanceData::CountTrackedWindows() const
{
	int trackedWindows = 0;
	for (const auto& wd : _WindowData)
	{
		if (wd.m_hwnd != NULL && wd.m_nUnusedCount <= 2)
		{
			trackedWindows++;
		}
	}
	return trackedWindows;
}

int InstanceData::CountSavedWindowRecords() const
{
	int count = 0;
	for (const auto& wd : _WindowData)
	{
		if (wd.m_wndClass[0] != '\0' && !wd.m_placements.empty())
		{
			count++;
		}
	}
	return count;
}

int InstanceData::CountTotalPlacements() const
{
	int count = 0;
	for (const auto& wd : _WindowData)
	{
		count += (int)wd.m_placements.size();
	}
	return count;
}

int InstanceData::CountPlacementsForConfig(UINT64 configHash) const
{
	int count = 0;
	for (const auto& wd : _WindowData)
	{
		if (wd.HasPlacement(configHash))
		{
			count++;
		}
	}
	return count;
}

int InstanceData::GetOrCreateConfigId(UINT64 configHash)
{
	auto it = _ConfigIds.find(configHash);
	if (it != _ConfigIds.end()) {
		return it->second;
	}

	int configId = _NextConfigId++;
	_ConfigIds[configHash] = configId;
	return configId;
}

BOOL InstanceData::TryGetSnapshotInfo(UINT64 configHash, ConfigSnapshotInfo& info) const
{
	auto it = _ConfigSnapshots.find(configHash);
	if (it == _ConfigSnapshots.end())
		return FALSE;
	info = it->second;
	return TRUE;
}

int InstanceData::RestoreWindowPositions(UINT64 configHash)
{
	ConfigSnapshotInfo info;
	TCHAR timeText[64];
	int configId = GetOrCreateConfigId(configHash);
	int storedPositions = CountPlacementsForConfig(configHash);
	int currentWindows = CountTrackedWindows();
	int attempted = 0;

	if (TryGetSnapshotInfo(configHash, info)) {
		FormatFileTimeLocal(&info.lastSavedUtc, timeText, _countof(timeText));
		storedPositions = (int)info.windowCount;
	}
	else {
		StringCchCopy(timeText, _countof(timeText), _T("unknown"));
	}

	TCHAR summary[512];
	StringCchPrintf(summary, _countof(summary),
		_T("Applying config #%d captured %s with %d stored positions; currently tracking %d window(s)"),
		configId, timeText, storedPositions, currentWindows);
	LOG_EVENT(_T("RESTORE"), summary);
	_RestoreRetryCount = 0;
	_AwaitingRestoreRetry = FALSE;
	for (auto& wd : _WindowData)
	{
		wd.m_lastRestoreError = ERROR_SUCCESS;
		wd.m_retryPending = FALSE;
	}

	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd != NULL && wd.m_nUnusedCount <= 2)
		{
			if (wd.RestoreWindow(configHash)) {
				attempted++;
			}
		}
	}

	return attempted;
}

void CancelPendingRestores()
{
	auto& inst = InstanceData::g_Instance;
	inst._RestoreRetryCount = 0;
	inst._AwaitingRestoreRetry = FALSE;
	for (auto& wd : inst._WindowData)
	{
		wd.m_retryPending = FALSE;
		wd.m_lastRestoreError = ERROR_SUCCESS;
	}
}

SavedWindowData* InstanceData::FindWindowSlot(HWND hwnd, DWORD processId, LPCTSTR wndClass)
{
	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd == hwnd && wd.m_processId == processId &&
			wndClass != NULL && wd.m_wndClass[0] != '\0' && lstrcmp(wd.m_wndClass, wndClass) == 0) {
			return &wd;
		}
	}

	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd == NULL && wd.m_wndClass[0] == '\0' && wd.m_placements.empty()) {
			return &wd;
		}
	}

	for (auto& wd : _WindowData)
	{
		if ((wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) && wd.m_placements.empty()) {
			return &wd;
		}
	}

	size_t oldSize = _WindowData.size();
	_WindowData.resize(oldSize + 32);
	return &_WindowData[oldSize];
}

LPCTSTR TranslateShowCommand(int nShowCmd)
{
	switch (nShowCmd)
	{
	case SW_RESTORE:
	case SW_SHOWNORMAL:
		return _T("SW_SHOWNORMAL");
	case SW_MAXIMIZE:
		return _T("SW_MAXIMIZE");
	case SW_MINIMIZE:
	case SW_SHOWMINIMIZED:
		return _T("SW_MINIMIZE");
	case SW_SHOWNOACTIVATE:
		return _T("SW_SHOWNOACTIVATE");
	case SW_SHOWMINNOACTIVE:
		return _T("SW_SHOWMINNOACTIVE");
	default:
		return _T("Unknown");
	}
}

void FormatWindowIdentity(HWND hwnd, DWORD fallbackProcessId, LPCTSTR fallbackClass, LPCTSTR fallbackProcessPath,
	LPCTSTR fallbackWindowTitle, TCHAR* buffer, size_t cchBuffer)
{
	TCHAR className[256] = _T("");
	TCHAR title[256] = _T("");
	TCHAR exePath[MAX_PATH] = _T("");
	DWORD processId = 0;

	ReadWindowIdentity(hwnd,
		className, _countof(className),
		exePath, _countof(exePath),
		title, _countof(title),
		&processId);

	if (className[0] == '\0' && fallbackClass != NULL) {
		StringCchCopy(className, _countof(className), fallbackClass);
	}
	if (processId == 0 && fallbackProcessId != 0) {
		processId = fallbackProcessId;
	}
	if (exePath[0] == '\0' && fallbackProcessPath != NULL && fallbackProcessPath[0] != '\0') {
		StringCchCopy(exePath, _countof(exePath), fallbackProcessPath);
	}
	if (title[0] == '\0' && fallbackWindowTitle != NULL && fallbackWindowTitle[0] != '\0') {
		StringCchCopy(title, _countof(title), fallbackWindowTitle);
	}
	if (title[0] == '\0') {
		StringCchCopy(title, _countof(title), _T("<untitled>"));
	}
	if (exePath[0] == '\0') {
		StringCchCopy(exePath, _countof(exePath), _T("<unknown>"));
	}

	StringCchPrintf(buffer, cchBuffer,
		_T("pid=%6lu  %-55s | class \"%-25s\" | \"%s\""),
		processId,
		exePath,
		className[0] ? className : _T("<unknown>"),
		title);
}

int NormalizeShowCommandForCompare(int showCmd)
{
	switch (showCmd)
	{
	case SW_RESTORE:
	case SW_SHOWNORMAL:
	case SW_SHOWNOACTIVATE:
		return SW_SHOWNORMAL;
	case SW_MINIMIZE:
	case SW_SHOWMINIMIZED:
	case SW_SHOWMINNOACTIVE:
		return SW_SHOWMINIMIZED;
	default:
		return showCmd;
	}
}

BOOL WindowPlacementNeedsRestore(const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual)
{
	if (actual.length != sizeof(WINDOWPLACEMENT)) {
		return TRUE;
	}

	if (NormalizeShowCommandForCompare(expected.showCmd) != NormalizeShowCommandForCompare(actual.showCmd)) {
		return TRUE;
	}

	return abs(expected.rcNormalPosition.left - actual.rcNormalPosition.left) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.top - actual.rcNormalPosition.top) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.right - actual.rcNormalPosition.right) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.bottom - actual.rcNormalPosition.bottom) > PLACEMENT_TOLERANCE;
}

BOOL CALLBACK SaveWindowsCallback(HWND hwnd, LPARAM lParam)
{
	UINT64 configHash = InstanceData::g_Instance._ConfigHash;

	if (IsWindowVisible(hwnd) && GetParent(hwnd) == NULL) {
		DWORD dwStyle = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
		DWORD dwExStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
		if (((dwStyle & (WS_OVERLAPPEDWINDOW)) != 0 ||
			(dwExStyle & WS_EX_APPWINDOW) != 0) &&
			(dwExStyle & (WS_EX_NOACTIVATE)) == 0)
		{
			TCHAR wndClass[40];
			TCHAR processPath[MAX_PATH];
			TCHAR windowTitle[256];
			DWORD processId = 0;
			ReadWindowIdentity(hwnd,
				wndClass, _countof(wndClass),
				processPath, _countof(processPath),
				windowTitle, _countof(windowTitle),
				&processId);
			SavedWindowData* pData = InstanceData::g_Instance.FindWindowSlot(hwnd, processId, wndClass);
			if (pData->SetData(hwnd, configHash))
			{
				int* pCount = reinterpret_cast<int*>(lParam);
				if (pCount) (*pCount)++;
			}
		}
	}
	return TRUE;
}

void ProcessDesktopWindows()
{
	UINT64 currentHash = ComputeMonitorConfigHash();
	if (currentHash != InstanceData::g_Instance._ConfigHash)
	{
		return;
	}
	InstanceData::g_Instance.TagWindowsUnused();
	int savedCount = 0;
	EnumDesktopWindows(NULL, SaveWindowsCallback, (LPARAM)&savedCount);
	GetSystemTimeAsFileTime(&InstanceData::g_Instance._LastCaptureUtc);
	UpdateSnapshotForCurrentConfig(currentHash, (DWORD)savedCount);
	UpdateStatusPanel();
}

VOID CALLBACK SaveTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(dwTime);
	KillTimer(hwnd, idEvent);
	if (!InstanceData::g_Instance.InChangingState) {
		ProcessDesktopWindows();
	}
}

void RetryPendingRestores()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	int pendingCount = 0;
	int successfulCalls = 0;
	int accessDeniedCount = 0;

	for (auto& wd : inst._WindowData) {
		if (!wd.m_retryPending) {
			continue;
		}

		pendingCount++;
		wd.m_retryPending = FALSE;
		if (wd.RestoreWindow(configHash)) {
			successfulCalls++;
		}
		else if (wd.m_lastRestoreError == ERROR_ACCESS_DENIED) {
			accessDeniedCount++;
		}
	}

	inst._AwaitingRestoreRetry = FALSE;
	if (pendingCount == 0) {
		inst.InChangingState = false;
		UpdateStatusPanel();
		return;
	}

	if (accessDeniedCount > 0) {
		LOG_EVENTF(_T("RESTORE"),
			_T("Retry %d/%d reapplied %d mismatched window(s); %d SetWindowPlacement call(s) succeeded and %d access-denied window(s) will be ignored"),
			inst._RestoreRetryCount, RESTORE_RETRY_LIMIT, pendingCount, successfulCalls, accessDeniedCount);
	}
	else {
		LOG_EVENTF(_T("RESTORE"),
			_T("Retry %d/%d reapplied %d mismatched window(s); %d SetWindowPlacement call(s) succeeded"),
			inst._RestoreRetryCount, RESTORE_RETRY_LIMIT, pendingCount, successfulCalls);
	}

	SetTimer(inst._MainWnd, VERIFY_TIMER_ID, VERIFY_TIMER_MS, NULL);
	UpdateStatusPanel();
}

void VerifyRestoredWindows()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	int mismatchCount = 0;
	int ignoredAccessDenied = 0;
	std::vector<std::basic_string<TCHAR>> mismatchDetails;
	for (auto& wd : inst._WindowData) {
		if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) continue;
		auto it = wd.m_placements.find(configHash);
		if (it == wd.m_placements.end()) continue;
		if (!IsWindow(wd.m_hwnd)) continue;
		if (wd.m_lastRestoreError == ERROR_ACCESS_DENIED) {
			wd.m_retryPending = FALSE;
			ignoredAccessDenied++;
			continue;
		}

		WINDOWPLACEMENT actual = {};
		actual.length = sizeof(actual);
		if (!GetWindowPlacement(wd.m_hwnd, &actual)) {
			LogWin32Error(_T("WARNING"), _T("GetWindowPlacement while verifying restore"), GetLastError());
			continue;
		}

		const RECT& expected = it->second.rcNormalPosition;
		const RECT& got = actual.rcNormalPosition;
		int dx = abs(expected.left - got.left);
		int dy = abs(expected.top - got.top);
		int dw = abs((expected.right - expected.left) - (got.right - got.left));
		int dh = abs((expected.bottom - expected.top) - (got.bottom - got.top));

		if (dx > PLACEMENT_TOLERANCE || dy > PLACEMENT_TOLERANCE ||
			dw > PLACEMENT_TOLERANCE || dh > PLACEMENT_TOLERANCE) {
			TCHAR identity[512];
			TCHAR sz[1024];
			FormatWindowIdentity(wd.m_hwnd, wd.m_processId, wd.m_wndClass, wd.m_processPath, wd.m_windowTitle, identity, _countof(identity));
			StringCchPrintf(sz, _countof(sz),
				_T("%s: expected (%d,%d %dx%d) got (%d,%d %dx%d)"),
				identity,
				expected.left, expected.top,
				expected.right - expected.left, expected.bottom - expected.top,
				got.left, got.top,
				got.right - got.left, got.bottom - got.top);
			mismatchCount++;
			wd.m_retryPending = TRUE;
			mismatchDetails.push_back(sz);
		}
		else {
			wd.m_retryPending = FALSE;
		}
	}
	if (mismatchCount == 0) {
		if (ignoredAccessDenied > 0) {
			LOG_EVENTF(_T("VERIFY"), _T("All retry-eligible windows reached expected positions; %d access-denied window(s) were excluded"),
				ignoredAccessDenied);
		}
		else {
			LOG_EVENT(_T("VERIFY"), _T("All windows at expected positions"));
		}
		inst._RestoreRetryCount = 0;
		inst._AwaitingRestoreRetry = FALSE;
		inst.InChangingState = false;
		ProcessDesktopWindows();
		return;
	}
	else if (inst._RestoreRetryCount < RESTORE_RETRY_LIMIT) {
		inst._RestoreRetryCount++;
		inst._AwaitingRestoreRetry = TRUE;
		if (ignoredAccessDenied > 0) {
			LOG_EVENTF(_T("VERIFY"),
				_T("%d window(s) still mismatched; waiting %d seconds before retry %d/%d. Ignoring %d access-denied window(s)"),
				mismatchCount, VERIFY_TIMER_MS / 1000, inst._RestoreRetryCount, RESTORE_RETRY_LIMIT, ignoredAccessDenied);
		}
		else {
			LOG_EVENTF(_T("VERIFY"),
				_T("%d window(s) still mismatched; waiting %d seconds before retry %d/%d"),
				mismatchCount, VERIFY_TIMER_MS / 1000, inst._RestoreRetryCount, RESTORE_RETRY_LIMIT);
		}
		SetTimer(inst._MainWnd, VERIFY_TIMER_ID, VERIFY_TIMER_MS, NULL);
		UpdateStatusPanel();
		return;
	}
	else {
		for (const auto& detail : mismatchDetails) {
			LOG_EVENT(_T("WARNING"), detail.c_str());
		}
		if (ignoredAccessDenied > 0) {
			LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position; %d access-denied window(s) were excluded"),
				mismatchCount, ignoredAccessDenied);
		}
		else {
			LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position"), mismatchCount);
		}
	}
	inst._RestoreRetryCount = 0;
	inst._AwaitingRestoreRetry = FALSE;
	inst.InChangingState = false;
	UpdateStatusPanel();
}