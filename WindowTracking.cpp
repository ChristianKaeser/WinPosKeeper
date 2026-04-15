#include "WindowTracking.h"

#include "AppState.h"

#include "LoggingUI.h"
#include "MonitorConfig.h"

SavedWindowData::SavedWindowData()
{
	m_wndClass[0] = '\0';
	m_hwnd = NULL;
	m_nUnusedCount = 1;
}

BOOL SavedWindowData::SetData(HWND hwnd, UINT64 configHash)
{
	m_hwnd = hwnd;
	m_nUnusedCount = 0;
	RealGetWindowClass(hwnd, m_wndClass, sizeof(m_wndClass) / sizeof(TCHAR));

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
	auto it = m_placements.find(configHash);
	if (it == m_placements.end()) return FALSE;

	if (!IsWindow(m_hwnd)) {
		TCHAR identity[512];
		FormatWindowIdentity(NULL, m_wndClass, identity, _countof(identity));
		LOG_EVENTF(_T("WARNING"), _T("Skipped restore: %s, saved HWND is no longer valid"), identity);
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
		FormatWindowIdentity(m_hwnd, m_wndClass, identity, _countof(identity));
		LOG_EVENTF(_T("WARNING"),
			_T("Skipped restore: saved class=\"%s\", current class=\"%s\", %s"),
			m_wndClass, szTempClass, identity);
		return FALSE;
	}

	WINDOWPLACEMENT place = it->second;
	TCHAR identity[512];
	FormatWindowIdentity(m_hwnd, m_wndClass, identity, _countof(identity));
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
			LogWin32Error(_T("WARNING"), _T("Initial SetWindowPlacement for maximized window"), GetLastError());
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
		LogWin32Error(_T("WARNING"), _T("SetWindowPlacement while restoring window"), GetLastError());
		return FALSE;
	}
	return TRUE;
}

ConfigSnapshotInfo::ConfigSnapshotInfo()
{
	lastSavedUtc.dwLowDateTime = 0;
	lastSavedUtc.dwHighDateTime = 0;
	windowCount = 0;
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
	_hStatus = NULL;
	_hStatusIcon = NULL;
	_hLogList = NULL;
	_hLogFont = NULL;
	InChangingState = false;
	RestoreOnDisconnect = true;
	PersistPositions = false;
	LoggingEnabled = true;
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

SavedWindowData* InstanceData::FindWindowSlot(HWND hwnd)
{
	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd == hwnd) {
			return &wd;
		}
	}

	for (auto& wd : _WindowData)
	{
		if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) {
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

void FormatWindowIdentity(HWND hwnd, LPCTSTR fallbackClass, TCHAR* buffer, size_t cchBuffer)
{
	TCHAR className[256] = _T("");
	TCHAR title[256] = _T("");
	TCHAR exePath[MAX_PATH] = _T("<unknown>");
	DWORD processId = 0;

	if (hwnd != NULL && IsWindow(hwnd)) {
		RealGetWindowClass(hwnd, className, _countof(className));
		GetWindowText(hwnd, title, _countof(title));
		GetWindowThreadProcessId(hwnd, &processId);
		if (processId != 0) {
			HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
			if (hProcess != NULL) {
				DWORD cch = _countof(exePath);
				if (!QueryFullProcessImageName(hProcess, 0, exePath, &cch)) {
					StringCchCopy(exePath, _countof(exePath), _T("<access denied>"));
				}
				CloseHandle(hProcess);
			}
			else {
				StringCchCopy(exePath, _countof(exePath), _T("<access denied>"));
			}
		}
	}

	if (className[0] == '\0' && fallbackClass != NULL) {
		StringCchCopy(className, _countof(className), fallbackClass);
	}
	if (title[0] == '\0') {
		StringCchCopy(title, _countof(title), _T("<untitled>"));
	}

	StringCchPrintf(buffer, cchBuffer,
		_T("class=\"%s\" pid=%lu title=\"%s\" exe=\"%s\""),
		className[0] ? className : _T("<unknown>"),
		processId,
		title,
		exePath);
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
			SavedWindowData* pData = InstanceData::g_Instance.FindWindowSlot(hwnd);
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
	ConfigSnapshotInfo& snapshot = InstanceData::g_Instance._ConfigSnapshots[currentHash];
	snapshot.lastSavedUtc = InstanceData::g_Instance._LastCaptureUtc;
	snapshot.windowCount = (DWORD)savedCount;
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

void VerifyRestoredWindows()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	int warnCount = 0;
	for (auto& wd : inst._WindowData) {
		if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) continue;
		auto it = wd.m_placements.find(configHash);
		if (it == wd.m_placements.end()) continue;
		if (!IsWindow(wd.m_hwnd)) continue;

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
			FormatWindowIdentity(wd.m_hwnd, wd.m_wndClass, identity, _countof(identity));
			StringCchPrintf(sz, _countof(sz),
				_T("%s: expected (%d,%d %dx%d) got (%d,%d %dx%d)"),
				identity,
				expected.left, expected.top,
				expected.right - expected.left, expected.bottom - expected.top,
				got.left, got.top,
				got.right - got.left, got.bottom - got.top);
			LOG_EVENT(_T("WARNING"), sz);
			warnCount++;
		}
	}
	if (warnCount == 0) {
		LOG_EVENT(_T("VERIFY"), _T("All windows at expected positions"));
	}
	else {
		LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position"), warnCount);
	}
	inst.InChangingState = false;
	UpdateStatusPanel();
}