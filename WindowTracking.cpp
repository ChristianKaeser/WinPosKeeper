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
	m_skipRetryPasses = FALSE;
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

static void FormatRectTransitionForLog(const RECT* fromRect, const RECT* toRect, TCHAR* buffer, size_t cchBuffer);
static BOOL IsTrackableTopLevelWindow(HWND hwnd);
static void TrimWindowHistory(WindowHistoryData& history);
static void MarkWindowHistorySelfAction(HWND hwnd);
static BOOL ShouldPreserveActualShowCommandDuringRestore(int expectedShowCmd, int actualShowCmd);
static void RecordWindowHistorySnapshot(HWND hwnd, LPCTSTR sourceOverride, LPCTSTR detail, BOOL forceCapture);
static void RecordWindowHistoryMarker(HWND hwnd, LPCTSTR sourceOverride, LPCTSTR detail);
static BOOL FlushPendingWindowHistoryEntriesInternal(BOOL refreshUi);
static BOOL PurgeClosedWindowHistoryEntriesInternal(BOOL refreshUi);
static void ScheduleWindowHistoryFlushTimer();
static SavedWindowData* FindTrackedWindowByHwnd(HWND hwnd);
static void SetRestorePhaseActive(BOOL active);

WindowHistoryEntry::WindowHistoryEntry()
{
	ZeroMemory(&recordedUtc, sizeof(recordedUtc));
	SetRectEmpty(&rect);
	showCmd = SW_SHOWNORMAL;
	hasPlacement = FALSE;
	configId = 0;
	sequence = 0;
}

HistoryLogEntry::HistoryLogEntry()
{
	ZeroMemory(&recordedUtc, sizeof(recordedUtc));
	configId = 0;
	sequence = 0;
}

WindowHistoryData::WindowHistoryData()
{
	hwndValue = 0;
	processId = 0;
	ZeroMemory(&lastRecordedUtc, sizeof(lastRecordedUtc));
	ZeroMemory(&lastPlacement, sizeof(lastPlacement));
	hasLastPlacement = FALSE;
	allowCaptureDuringRestore = FALSE;
	inSizeMove = FALSE;
	rateLimitUntilTick = 0;
	pendingSelfActionUntilTick = 0;
	ZeroMemory(&pendingPlacement, sizeof(pendingPlacement));
	ZeroMemory(&pendingRecordedUtc, sizeof(pendingRecordedUtc));
	hasPendingPlacement = FALSE;
	pendingConfigId = 0;
}

static int GetActiveConfigIdForHistory()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	if (configHash == 0 || inst.InChangingState) {
		UINT64 liveHash = ComputeMonitorConfigHash();
		if (liveHash != 0) {
			configHash = liveHash;
		}
	}
	if (configHash == 0) {
		return 0;
	}
	return inst.GetOrCreateConfigId(configHash);
}

// Drops the placements whose layout was captured least recently, never the one just written.
// Layouts without a snapshot record (never fully captured) count as oldest.
static void PruneOldestPlacements(std::map<UINT64, WINDOWPLACEMENT>& placements, UINT64 keepHash)
{
	const auto& snapshots = InstanceData::g_Instance._ConfigSnapshots;
	while (placements.size() > MAX_CONFIGSLOTS) {
		auto oldest = placements.end();
		ULONGLONG oldestTime = 0;
		for (auto it = placements.begin(); it != placements.end(); ++it) {
			if (it->first == keepHash) {
				continue;
			}
			ULONGLONG capturedTime = 0;
			auto snapshot = snapshots.find(it->first);
			if (snapshot != snapshots.end()) {
				ULARGE_INTEGER value = {};
				value.LowPart = snapshot->second.lastSavedUtc.dwLowDateTime;
				value.HighPart = snapshot->second.lastSavedUtc.dwHighDateTime;
				capturedTime = value.QuadPart;
			}
			if (oldest == placements.end() || capturedTime < oldestTime) {
				oldest = it;
				oldestTime = capturedTime;
			}
		}
		if (oldest == placements.end()) {
			break;
		}
		placements.erase(oldest);
	}
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
	PruneOldestPlacements(m_placements, configHash);

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
	BOOL haveActualPlacement = FALSE;
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
		haveActualPlacement = TRUE;
		if (!WindowPlacementNeedsRestore(m_hwnd, it->second, actual)) {
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
	if (haveActualPlacement && ShouldPreserveActualShowCommandDuringRestore(place.showCmd, actual.showCmd)) {
		place.showCmd = actual.showCmd;
	}
	TCHAR identity[512];
	TCHAR rectTransition[160];
	MarkWindowHistorySelfAction(m_hwnd);
	FormatWindowIdentity(m_hwnd, m_processId, m_wndClass, m_processPath, m_windowTitle, identity, _countof(identity));
	FormatRectTransitionForLog(haveActualPlacement ? &actual.rcNormalPosition : NULL, &place.rcNormalPosition,
		rectTransition, _countof(rectTransition));
	LOG_EVENTF(_T("RESTORE"),
		_T("%s  %-17s %s"),
		rectTransition,
		TranslateShowCommand(place.showCmd),
		identity);

	// Async placement keeps a hung target window from blocking this thread. Both requests are
	// posted to the target's queue, so the normal-then-maximize order is preserved.
	place.flags &= ~WPF_SETMINPOSITION;
	place.flags |= WPF_ASYNCWINDOWPLACEMENT;
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
	_MoveSizeHook = NULL;
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
	_hSettingsIntro = NULL;
	_hSettingsRestoreCheck = NULL;
	_hSettingsAutostartCheck = NULL;
	_hSettingsPersistCheck = NULL;
	_hSettingsLoggingCheck = NULL;
	_hSettingsHistoryCheck = NULL;
	_hSettingsDelayLabel = NULL;
	_hSettingsDelayEdit = NULL;
	_hSettingsRetryLabel = NULL;
	_hSettingsRetryEdit = NULL;
	_hSettingsApplyButton = NULL;
	_hHistoryWindowList = NULL;
	_hHistorySummary = NULL;
	_hHistoryTimelineList = NULL;
	_hHistoryExportButton = NULL;
	_hLogFont = NULL;
	InChangingState = false;
	_RestorePhaseActive = FALSE;
	_SessionLocked = FALSE;
	_SessionNotificationsRegistered = FALSE;
	_DeferredDisplayChangeUntilUnlock = FALSE;
	_DeferredRestoreVerifyUntilUnlock = FALSE;
	RestoreOnDisconnect = true;
	PersistPositions = false;
	LoggingEnabled = true;
	_HistoryTrackingEnabled = FALSE;
	_RestoreRetryDelayMs = DEFAULT_RESTORE_RETRY_DELAY_MS;
	_RestoreRetryLimit = DEFAULT_RESTORE_RETRY_LIMIT;
	_RestoreRetryCount = 0;
	_AwaitingRestoreRetry = FALSE;
	_HistorySelectedHwnd = 0;
	_HistoryNextSequence = 0;
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
	if (_MoveSizeHook != NULL) UnhookWinEvent(_MoveSizeHook);
	_MoveSizeHook = NULL;

	_WindowData.clear();
	_ConfigIds.clear();
	_NextConfigId = 1;
	_ConfigSnapshots.clear();
	_InspectorConfigHashes.clear();
	_InspectorSelectedConfigHash = 0;
	_HistoryWindowKeys.clear();
	_HistorySelectedHwnd = 0;
	_HistoryLog.clear();
	_WindowHistory.clear();
	_HistoryNextSequence = 0;
	_RestorePhaseActive = FALSE;
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
	SetRestorePhaseActive(TRUE);
	_RestoreRetryCount = 0;
	_AwaitingRestoreRetry = FALSE;
	for (auto& wd : _WindowData)
	{
		wd.m_lastRestoreError = ERROR_SUCCESS;
		wd.m_retryPending = FALSE;
		wd.m_skipRetryPasses = FALSE;
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
	if (attempted == 0) {
		SetRestorePhaseActive(FALSE);
	}

	return attempted;
}

void CancelPendingRestores()
{
	auto& inst = InstanceData::g_Instance;
	SetRestorePhaseActive(FALSE);
	inst._RestoreRetryCount = 0;
	inst._AwaitingRestoreRetry = FALSE;
	for (auto& wd : inst._WindowData)
	{
		wd.m_retryPending = FALSE;
		wd.m_lastRestoreError = ERROR_SUCCESS;
		wd.m_skipRetryPasses = FALSE;
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

static ULONGLONG NextHistorySequence()
{
	return ++InstanceData::g_Instance._HistoryNextSequence;
}

static BOOL WindowPlacementsMatchExactly(const WINDOWPLACEMENT& left, const WINDOWPLACEMENT& right)
{
	return left.showCmd == right.showCmd &&
		left.rcNormalPosition.left == right.rcNormalPosition.left &&
		left.rcNormalPosition.top == right.rcNormalPosition.top &&
		left.rcNormalPosition.right == right.rcNormalPosition.right &&
		left.rcNormalPosition.bottom == right.rcNormalPosition.bottom;
}

static BOOL IsTrackableTopLevelWindow(HWND hwnd)
{
	if (hwnd == NULL || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetParent(hwnd) != NULL) {
		return FALSE;
	}

	DWORD style = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
	DWORD exStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
	return (((style & WS_OVERLAPPEDWINDOW) != 0 || (exStyle & WS_EX_APPWINDOW) != 0) &&
		(exStyle & WS_EX_NOACTIVATE) == 0) ? TRUE : FALSE;
}

static WindowHistoryData& GetOrCreateWindowHistory(HWND hwnd, DWORD processId,
	LPCTSTR wndClass, LPCTSTR processPath, LPCTSTR windowTitle)
{
	auto& inst = InstanceData::g_Instance;
	UINT_PTR hwndValue = reinterpret_cast<UINT_PTR>(hwnd);
	WindowHistoryData& history = inst._WindowHistory[hwndValue];
	if (history.hwndValue != 0 &&
		(history.processId != processId ||
			(wndClass != NULL && !history.windowClass.empty() && history.windowClass != wndClass))) {
		history = WindowHistoryData();
	}

	history.hwndValue = hwndValue;
	history.processId = processId;
	history.windowClass = wndClass != NULL ? wndClass : _T("");
	history.processPath = processPath != NULL ? processPath : _T("");
	history.latestTitle = windowTitle != NULL ? windowTitle : _T("");
	return history;
}

static void AppendWindowHistoryEntry(WindowHistoryData& history, const WINDOWPLACEMENT& placement,
	LPCTSTR source, LPCTSTR detail, LPCTSTR windowTitle, int configId = 0, const FILETIME* recordedUtc = NULL)
{
	WindowHistoryEntry entry;
	if (recordedUtc != NULL) {
		entry.recordedUtc = *recordedUtc;
	}
	else {
		GetSystemTimeAsFileTime(&entry.recordedUtc);
	}
	entry.rect = placement.rcNormalPosition;
	entry.showCmd = placement.showCmd;
	entry.hasPlacement = TRUE;
	entry.configId = configId > 0 ? configId : GetActiveConfigIdForHistory();
	entry.sequence = NextHistorySequence();
	entry.source = source != NULL ? source : _T("");
	entry.detail = detail != NULL ? detail : _T("location change");
	entry.windowTitle = windowTitle != NULL ? windowTitle : _T("");
	history.lastRecordedUtc = entry.recordedUtc;
	history.lastPlacement = placement;
	history.hasLastPlacement = TRUE;
	history.latestTitle = entry.windowTitle;
	history.entries.push_back(entry);
	TrimWindowHistory(history);
}

static BOOL TryReadWindowHistorySnapshot(HWND hwnd,
	WindowHistoryData** historyOut,
	WINDOWPLACEMENT* placementOut,
	TCHAR* windowTitle,
	size_t cchWindowTitle)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled || !IsTrackableTopLevelWindow(hwnd)) {
		return FALSE;
	}

	TCHAR wndClass[40];
	TCHAR processPath[MAX_PATH];
	DWORD processId = 0;
	if (windowTitle != NULL && cchWindowTitle > 0) {
		windowTitle[0] = '\0';
	}

	ReadWindowIdentity(hwnd,
		wndClass, _countof(wndClass),
		processPath, _countof(processPath),
		windowTitle, cchWindowTitle,
		&processId);
	if (wndClass[0] == '\0' || processId == 0) {
		return FALSE;
	}

	WINDOWPLACEMENT placement = {};
	placement.length = sizeof(placement);
	if (!GetWindowPlacement(hwnd, &placement)) {
		return FALSE;
	}

	WindowHistoryData& history = GetOrCreateWindowHistory(hwnd, processId, wndClass, processPath,
		windowTitle != NULL ? windowTitle : _T(""));
	if (historyOut != NULL) {
		*historyOut = &history;
	}
	if (placementOut != NULL) {
		*placementOut = placement;
	}
	return TRUE;
}

static void ClearPendingWindowHistory(WindowHistoryData& history)
{
	history.hasPendingPlacement = FALSE;
	ZeroMemory(&history.pendingPlacement, sizeof(history.pendingPlacement));
	ZeroMemory(&history.pendingRecordedUtc, sizeof(history.pendingRecordedUtc));
	history.pendingSource.clear();
	history.pendingDetail.clear();
	history.pendingWindowTitle.clear();
	history.pendingConfigId = 0;
}

static BOOL IsWindowHistoryRateLimited(const WindowHistoryData& history, ULONGLONG nowTick)
{
	return history.rateLimitUntilTick != 0 && nowTick < history.rateLimitUntilTick;
}

static BOOL HasEquivalentWindowHistoryState(const WindowHistoryData& history,
	const WINDOWPLACEMENT& placement, LPCTSTR windowTitle)
{
	std::basic_string<TCHAR> title = windowTitle != NULL ? windowTitle : _T("");
	return history.hasLastPlacement && WindowPlacementsMatchExactly(history.lastPlacement, placement) &&
		history.latestTitle == title;
}

static BOOL FlushPendingWindowHistoryEntry(WindowHistoryData& history, ULONGLONG nowTick)
{
	if (!history.hasPendingPlacement) {
		history.rateLimitUntilTick = 0;
		ClearPendingWindowHistory(history);
		return FALSE;
	}

	if (HasEquivalentWindowHistoryState(history, history.pendingPlacement, history.pendingWindowTitle.c_str())) {
		history.rateLimitUntilTick = 0;
		ClearPendingWindowHistory(history);
		return FALSE;
	}

	AppendWindowHistoryEntry(history,
		history.pendingPlacement,
		history.pendingSource.c_str(),
		history.pendingDetail.c_str(),
		history.pendingWindowTitle.c_str(),
		history.pendingConfigId,
		&history.pendingRecordedUtc);
	ClearPendingWindowHistory(history);
	history.rateLimitUntilTick = 0;
	return TRUE;
}

static void ScheduleWindowHistoryFlushTimer()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._MainWnd == NULL) {
		return;
	}

	ULONGLONG nowTick = GetTickCount64();
	ULONGLONG earliestTick = 0;
	for (const auto& pair : inst._WindowHistory) {
		const WindowHistoryData& history = pair.second;
		if (!history.hasPendingPlacement || history.rateLimitUntilTick == 0) {
			continue;
		}
		if (earliestTick == 0 || history.rateLimitUntilTick < earliestTick) {
			earliestTick = history.rateLimitUntilTick;
		}
	}

	if (earliestTick == 0) {
		KillTimer(inst._MainWnd, HISTORY_FLUSH_TIMER_ID);
		return;
	}

	ULONGLONG remainingMs = earliestTick <= nowTick ? 1ULL : (earliestTick - nowTick);
	UINT delayMs = remainingMs > 0x7FFFFFFFULL ? 0x7FFFFFFF : (UINT)remainingMs;
	SetTimer(inst._MainWnd, HISTORY_FLUSH_TIMER_ID, delayMs, NULL);
}

static BOOL PurgeClosedWindowHistoryEntriesInternal(BOOL refreshUi)
{
	auto& inst = InstanceData::g_Instance;
	BOOL removedAny = FALSE;
	for (auto it = inst._WindowHistory.begin(); it != inst._WindowHistory.end(); ) {
		HWND hwnd = reinterpret_cast<HWND>(it->first);
		if (hwnd == NULL || !IsWindow(hwnd)) {
			if (inst._HistorySelectedHwnd == it->first) {
				inst._HistorySelectedHwnd = 0;
			}
			it = inst._WindowHistory.erase(it);
			removedAny = TRUE;
		}
		else {
			++it;
		}
	}

	if (removedAny) {
		inst._HistoryWindowKeys.clear();
		ScheduleWindowHistoryFlushTimer();
		if (refreshUi) {
			RefreshWindowHistoryInspector();
		}
	}
	return removedAny;
}

static BOOL FlushPendingWindowHistoryEntriesInternal(BOOL refreshUi)
{
	auto& inst = InstanceData::g_Instance;
	ULONGLONG nowTick = GetTickCount64();
	BOOL flushedAny = FALSE;
	for (auto& pair : inst._WindowHistory) {
		WindowHistoryData& history = pair.second;
		if (history.rateLimitUntilTick == 0) {
			continue;
		}
		if (!history.hasPendingPlacement) {
			if (nowTick >= history.rateLimitUntilTick) {
				history.rateLimitUntilTick = 0;
			}
			continue;
		}
		if (nowTick >= history.rateLimitUntilTick) {
			flushedAny = FlushPendingWindowHistoryEntry(history, nowTick) || flushedAny;
		}
	}

	ScheduleWindowHistoryFlushTimer();
	if (flushedAny && refreshUi) {
		RefreshWindowHistoryInspector();
	}
	return flushedAny;
}

static LPCTSTR ResolveWindowHistorySource(WindowHistoryData& history, LPCTSTR sourceOverride)
{
	if (sourceOverride != NULL && sourceOverride[0] != '\0') {
		return sourceOverride;
	}

	ULONGLONG nowTick = GetTickCount64();
	if (history.pendingSelfActionUntilTick != 0 && nowTick <= history.pendingSelfActionUntilTick) {
		history.pendingSelfActionUntilTick = 0;
		return _T("winposkeeper");
	}

	history.pendingSelfActionUntilTick = 0;
	return InstanceData::g_Instance.InChangingState ? _T("display/system") : _T("external/system");
}

static void TrimWindowHistory(WindowHistoryData& history)
{
	if (history.entries.size() > MAX_HISTORY_ENTRIES_PER_WINDOW) {
		history.entries.erase(history.entries.begin(),
			history.entries.begin() + (history.entries.size() - MAX_HISTORY_ENTRIES_PER_WINDOW));
	}
}

static void MarkWindowHistorySelfAction(HWND hwnd)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled || hwnd == NULL) {
		return;
	}

	WindowHistoryData& history = inst._WindowHistory[reinterpret_cast<UINT_PTR>(hwnd)];
	if (history.hwndValue == 0) {
		history.hwndValue = reinterpret_cast<UINT_PTR>(hwnd);
	}
	history.pendingSelfActionUntilTick = GetTickCount64() + 3000;
}

static SavedWindowData* FindTrackedWindowByHwnd(HWND hwnd)
{
	auto& inst = InstanceData::g_Instance;
	for (auto& wd : inst._WindowData) {
		if (wd.m_hwnd == hwnd && wd.m_nUnusedCount <= 2) {
			return &wd;
		}
	}
	return NULL;
}

static void SetRestorePhaseActive(BOOL active)
{
	auto& inst = InstanceData::g_Instance;
	inst._RestorePhaseActive = active;
	if (!active) {
		for (auto& pair : inst._WindowHistory) {
			pair.second.allowCaptureDuringRestore = FALSE;
		}
	}
}

static void RecordWindowHistorySnapshot(HWND hwnd, LPCTSTR sourceOverride, LPCTSTR detail, BOOL forceCapture)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled) {
		return;
	}

	PurgeClosedWindowHistoryEntriesInternal(FALSE);
	FlushPendingWindowHistoryEntriesInternal(FALSE);

	WindowHistoryData* history = NULL;
	WINDOWPLACEMENT placement = {};
	TCHAR windowTitle[256];
	if (!TryReadWindowHistorySnapshot(hwnd, &history, &placement, windowTitle, _countof(windowTitle))) {
		return;
	}
	if (inst._RestorePhaseActive && !forceCapture && !history->allowCaptureDuringRestore) {
		return;
	}
	if (HasEquivalentWindowHistoryState(*history, placement, windowTitle)) {
		if (history->hasPendingPlacement) {
			ClearPendingWindowHistory(*history);
			ScheduleWindowHistoryFlushTimer();
		}
		return;
	}

	LPCTSTR resolvedSource = ResolveWindowHistorySource(*history, sourceOverride);
	LPCTSTR resolvedDetail = (detail != NULL && detail[0] != '\0') ? detail : _T("location change");
	int currentConfigId = GetActiveConfigIdForHistory();
	ULONGLONG nowTick = GetTickCount64();
	if (forceCapture) {
		history->rateLimitUntilTick = 0;
		ClearPendingWindowHistory(*history);
		AppendWindowHistoryEntry(*history, placement, resolvedSource, resolvedDetail, windowTitle, currentConfigId);
		ScheduleWindowHistoryFlushTimer();
		RefreshWindowHistoryInspector();
		return;
	}

	if (!history->inSizeMove) {
		history->rateLimitUntilTick = 0;
		ClearPendingWindowHistory(*history);
		AppendWindowHistoryEntry(*history, placement, resolvedSource, resolvedDetail, windowTitle, currentConfigId);
		ScheduleWindowHistoryFlushTimer();
		RefreshWindowHistoryInspector();
		return;
	}

	history->pendingPlacement = placement;
	GetSystemTimeAsFileTime(&history->pendingRecordedUtc);
	history->hasPendingPlacement = TRUE;
	history->pendingSource = resolvedSource;
	history->pendingDetail = resolvedDetail;
	history->pendingWindowTitle = windowTitle;
	history->pendingConfigId = currentConfigId;
	if (!IsWindowHistoryRateLimited(*history, nowTick)) {
		history->rateLimitUntilTick = nowTick + HISTORY_SIZEMOVE_RATE_LIMIT_WINDOW_MS;
	}
	ScheduleWindowHistoryFlushTimer();
}

static void RecordWindowHistoryMarker(HWND hwnd, LPCTSTR sourceOverride, LPCTSTR detail)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled) {
		return;
	}

	PurgeClosedWindowHistoryEntriesInternal(FALSE);
	FlushPendingWindowHistoryEntriesInternal(FALSE);

	WindowHistoryData* history = NULL;
	WINDOWPLACEMENT placement = {};
	TCHAR windowTitle[256];
	if (!TryReadWindowHistorySnapshot(hwnd, &history, &placement, windowTitle, _countof(windowTitle))) {
		return;
	}
	if (inst._RestorePhaseActive && !history->allowCaptureDuringRestore) {
		return;
	}

	LPCTSTR resolvedSource = ResolveWindowHistorySource(*history, sourceOverride);
	int currentConfigId = GetActiveConfigIdForHistory();
	AppendWindowHistoryEntry(*history, placement,
		resolvedSource,
		(detail != NULL && detail[0] != '\0') ? detail : _T("window event"),
		windowTitle,
		currentConfigId);
	ScheduleWindowHistoryFlushTimer();
	RefreshWindowHistoryInspector();
}

void CaptureWindowHistoryEvent(HWND hwnd, LPCTSTR sourceOverride, LPCTSTR detail, BOOL forceCapture)
{
	RecordWindowHistorySnapshot(hwnd, sourceOverride, detail, forceCapture);
}

void CaptureWindowHistoryEnterSizeMove(HWND hwnd, LPCTSTR sourceOverride)
{
	auto& inst = InstanceData::g_Instance;
	if (inst.InChangingState) {
		SavedWindowData* trackedWindow = FindTrackedWindowByHwnd(hwnd);
		if (trackedWindow != NULL && !trackedWindow->m_skipRetryPasses) {
			trackedWindow->m_skipRetryPasses = TRUE;
			trackedWindow->m_retryPending = FALSE;
			TCHAR identity[512];
			FormatWindowIdentity(hwnd, trackedWindow->m_processId, trackedWindow->m_wndClass,
				trackedWindow->m_processPath, trackedWindow->m_windowTitle,
				identity, _countof(identity));
			LOG_EVENTF(_T("VERIFY"), _T("Skipping remaining restore passes after WM_ENTERSIZEMOVE: %s"), identity);
		}
	}

	if (!inst._HistoryTrackingEnabled) {
		return;
	}

	PurgeClosedWindowHistoryEntriesInternal(FALSE);
	FlushPendingWindowHistoryEntriesInternal(FALSE);

	WindowHistoryData* history = NULL;
	WINDOWPLACEMENT placement = {};
	TCHAR windowTitle[256];
	if (!TryReadWindowHistorySnapshot(hwnd, &history, &placement, windowTitle, _countof(windowTitle))) {
		return;
	}

	history->allowCaptureDuringRestore = TRUE;
	history->inSizeMove = TRUE;
	history->rateLimitUntilTick = 0;
	ClearPendingWindowHistory(*history);
	RecordWindowHistoryMarker(hwnd, sourceOverride, _T("WM_ENTERSIZEMOVE"));
}

void CaptureWindowHistoryExitSizeMove(HWND hwnd, LPCTSTR sourceOverride)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled) {
		return;
	}

	PurgeClosedWindowHistoryEntriesInternal(FALSE);
	FlushPendingWindowHistoryEntriesInternal(FALSE);

	WindowHistoryData* history = NULL;
	WINDOWPLACEMENT placement = {};
	TCHAR windowTitle[256];
	if (!TryReadWindowHistorySnapshot(hwnd, &history, &placement, windowTitle, _countof(windowTitle))) {
		return;
	}

	history->inSizeMove = FALSE;
	history->rateLimitUntilTick = 0;
	if (history->hasPendingPlacement) {
		FlushPendingWindowHistoryEntry(*history, GetTickCount64());
	}
	ClearPendingWindowHistory(*history);
	RecordWindowHistoryMarker(hwnd, sourceOverride, _T("WM_EXITSIZEMOVE"));
}

void FlushPendingWindowHistoryEntries()
{
	FlushPendingWindowHistoryEntriesInternal(TRUE);
}

void PurgeClosedWindowHistoryEntries()
{
	PurgeClosedWindowHistoryEntriesInternal(TRUE);
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

static void FormatRectForLog(const RECT* rect, TCHAR* buffer, size_t cchBuffer)
{
	if (rect == NULL) {
		StringCchCopy(buffer, cchBuffer, _T("(     ?,     ?     ?x    ? )"));
		return;
	}

	StringCchPrintf(buffer, cchBuffer, _T("(%6d,%6d %5dx%-5d)"),
		rect->left,
		rect->top,
		rect->right - rect->left,
		rect->bottom - rect->top);
}

static void FormatRectTransitionForLog(const RECT* fromRect, const RECT* toRect, TCHAR* buffer, size_t cchBuffer)
{
	TCHAR fromText[64];
	TCHAR toText[64];
	FormatRectForLog(fromRect, fromText, _countof(fromText));
	FormatRectForLog(toRect, toText, _countof(toText));
	StringCchPrintf(buffer, cchBuffer, _T("%s -> %s"), fromText, toText);
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

enum RestoreShowCommandClass {
	RestoreShowCommandNormal = 0,
	RestoreShowCommandMinimized = 1,
	RestoreShowCommandMaximized = 2,
	RestoreShowCommandOther = 3,
};

static RestoreShowCommandClass ClassifyShowCommandForRestore(int showCmd)
{
	switch (NormalizeShowCommandForCompare(showCmd))
	{
	case SW_SHOWNORMAL:
		return RestoreShowCommandNormal;
	case SW_SHOWMINIMIZED:
		return RestoreShowCommandMinimized;
	case SW_MAXIMIZE:
		return RestoreShowCommandMaximized;
	default:
		return RestoreShowCommandOther;
	}
}

static BOOL ShouldPreserveActualShowCommandDuringRestore(int expectedShowCmd, int actualShowCmd)
{
	RestoreShowCommandClass expectedClass = ClassifyShowCommandForRestore(expectedShowCmd);
	RestoreShowCommandClass actualClass = ClassifyShowCommandForRestore(actualShowCmd);
	return (expectedClass == RestoreShowCommandNormal || expectedClass == RestoreShowCommandMinimized) &&
		(actualClass == RestoreShowCommandNormal || actualClass == RestoreShowCommandMinimized);
}

BOOL WindowPlacementNeedsRestore(const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual)
{
	if (actual.length != sizeof(WINDOWPLACEMENT)) {
		return TRUE;
	}

	if (!ShouldPreserveActualShowCommandDuringRestore(expected.showCmd, actual.showCmd) &&
		NormalizeShowCommandForCompare(expected.showCmd) != NormalizeShowCommandForCompare(actual.showCmd)) {
		return TRUE;
	}

	return abs(expected.rcNormalPosition.left - actual.rcNormalPosition.left) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.top - actual.rcNormalPosition.top) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.right - actual.rcNormalPosition.right) > PLACEMENT_TOLERANCE ||
		abs(expected.rcNormalPosition.bottom - actual.rcNormalPosition.bottom) > PLACEMENT_TOLERANCE;
}

static BOOL MaximizedPlacementUsesWrongMonitor(HWND hwnd, const WINDOWPLACEMENT& expected)
{
	if (hwnd == NULL || ClassifyShowCommandForRestore(expected.showCmd) != RestoreShowCommandMaximized) {
		return FALSE;
	}

	HMONITOR expectedMonitor = MonitorFromRect(&expected.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
	HMONITOR actualMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (expectedMonitor == NULL || actualMonitor == NULL) {
		return FALSE;
	}

	MONITORINFO expectedInfo = {};
	MONITORINFO actualInfo = {};
	expectedInfo.cbSize = sizeof(expectedInfo);
	actualInfo.cbSize = sizeof(actualInfo);
	if (!GetMonitorInfo(expectedMonitor, &expectedInfo) || !GetMonitorInfo(actualMonitor, &actualInfo)) {
		return FALSE;
	}

	return !EqualRect(&expectedInfo.rcMonitor, &actualInfo.rcMonitor);
}

BOOL WindowPlacementNeedsRestore(HWND hwnd, const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual)
{
	return WindowPlacementNeedsRestore(expected, actual) || MaximizedPlacementUsesWrongMonitor(hwnd, expected);
}

BOOL CALLBACK SaveWindowsCallback(HWND hwnd, LPARAM lParam)
{
	UINT64 configHash = InstanceData::g_Instance._ConfigHash;

	if (IsTrackableTopLevelWindow(hwnd)) {
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
	return TRUE;
}

void ProcessDesktopWindows()
{
	UINT64 currentHash = ComputeMonitorConfigHash();
	if (currentHash != InstanceData::g_Instance._ConfigHash)
	{
		return;
	}
	PurgeClosedWindowHistoryEntriesInternal(FALSE);
	FlushPendingWindowHistoryEntriesInternal(FALSE);
	InstanceData::g_Instance.TagWindowsUnused();
	int savedCount = 0;
	EnumDesktopWindows(NULL, SaveWindowsCallback, (LPARAM)&savedCount);
	GetSystemTimeAsFileTime(&InstanceData::g_Instance._LastCaptureUtc);
	UpdateSnapshotForCurrentConfig(currentHash, (DWORD)savedCount);
	UpdateStatusPanel();
}

void ClearWindowHistoryTracking()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._MainWnd != NULL) {
		KillTimer(inst._MainWnd, HISTORY_FLUSH_TIMER_ID);
	}
	inst._WindowHistory.clear();
	inst._HistoryLog.clear();
	inst._HistoryWindowKeys.clear();
	inst._HistorySelectedHwnd = 0;
	inst._HistoryNextSequence = 0;
	RefreshWindowHistoryInspector();
}

void PrimeWindowHistoryTracking(LPCTSTR reason)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled) {
		return;
	}
	PurgeClosedWindowHistoryEntriesInternal(FALSE);

	for (const auto& wd : inst._WindowData) {
		if (wd.m_hwnd != NULL && wd.m_nUnusedCount <= 2 && IsWindow(wd.m_hwnd)) {
			RecordWindowHistorySnapshot(wd.m_hwnd, _T("initial"), reason, TRUE);
		}
	}
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
	int checkedCount = 0;
	int mismatchCount = 0;
	int successfulCalls = 0;
	int accessDeniedCount = 0;
	int skippedUserMoveCount = 0;
	BOOL hasEligibleWindows = FALSE;

	for (auto& wd : inst._WindowData) {
		if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) {
			continue;
		}
		auto it = wd.m_placements.find(configHash);
		if (it == wd.m_placements.end()) {
			continue;
		}
		if (!IsWindow(wd.m_hwnd)) {
			continue;
		}

		hasEligibleWindows = TRUE;
		wd.m_retryPending = FALSE;
		if (wd.m_skipRetryPasses) {
			skippedUserMoveCount++;
			continue;
		}
		if (wd.m_lastRestoreError == ERROR_ACCESS_DENIED) {
			accessDeniedCount++;
			continue;
		}

		WINDOWPLACEMENT actual = {};
		actual.length = sizeof(actual);
		if (!GetWindowPlacement(wd.m_hwnd, &actual)) {
			LogWin32Error(_T("WARNING"), _T("GetWindowPlacement while preparing retry restore"), GetLastError());
			continue;
		}

		checkedCount++;
		if (!WindowPlacementNeedsRestore(wd.m_hwnd, it->second, actual)) {
			continue;
		}

		mismatchCount++;
		wd.m_retryPending = TRUE;
		if (wd.RestoreWindow(configHash)) {
			successfulCalls++;
		}
		else if (wd.m_lastRestoreError == ERROR_ACCESS_DENIED) {
			accessDeniedCount++;
		}
	}

	inst._AwaitingRestoreRetry = FALSE;
	if (!hasEligibleWindows) {
		SetRestorePhaseActive(FALSE);
		inst.InChangingState = false;
		ProcessDesktopWindows();
		UpdateStatusPanel();
		return;
	}

	if (mismatchCount == 0) {
		if (skippedUserMoveCount > 0 || accessDeniedCount > 0) {
			LOG_EVENTF(_T("RESTORE"),
				_T("Retry %d/%d checked %d window(s); no retry-eligible mismatches found. Skipping %d user-moved window(s) and ignoring %d access-denied window(s)"),
				inst._RestoreRetryCount, inst._RestoreRetryLimit, checkedCount, skippedUserMoveCount, accessDeniedCount);
		}
		else {
			LOG_EVENTF(_T("RESTORE"),
				_T("Retry %d/%d checked %d window(s); no retry-eligible mismatches found"),
				inst._RestoreRetryCount, inst._RestoreRetryLimit, checkedCount);
		}
	}
	else if (accessDeniedCount > 0 || skippedUserMoveCount > 0) {
		LOG_EVENTF(_T("RESTORE"),
			_T("Retry %d/%d checked %d window(s); reapplied %d mismatched window(s); %d SetWindowPlacement call(s) succeeded, %d user-moved window(s) skipped, %d access-denied window(s) ignored"),
			inst._RestoreRetryCount, inst._RestoreRetryLimit, checkedCount, mismatchCount, successfulCalls,
			skippedUserMoveCount, accessDeniedCount);
	}
	else {
		LOG_EVENTF(_T("RESTORE"),
			_T("Retry %d/%d checked %d window(s); reapplied %d mismatched window(s); %d SetWindowPlacement call(s) succeeded"),
			inst._RestoreRetryCount, inst._RestoreRetryLimit, checkedCount, mismatchCount, successfulCalls);
	}

	SetTimer(inst._MainWnd, VERIFY_TIMER_ID, inst._RestoreRetryDelayMs, NULL);
	UpdateStatusPanel();
}

void VerifyRestoredWindows()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	int mismatchCount = 0;
	int ignoredAccessDenied = 0;
	int skippedUserMoveCount = 0;
	std::vector<std::basic_string<TCHAR>> mismatchDetails;
	for (auto& wd : inst._WindowData) {
		if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) continue;
		auto it = wd.m_placements.find(configHash);
		if (it == wd.m_placements.end()) continue;
		if (!IsWindow(wd.m_hwnd)) continue;
		if (wd.m_skipRetryPasses) {
			wd.m_retryPending = FALSE;
			skippedUserMoveCount++;
			continue;
		}
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
		BOOL monitorMismatch = MaximizedPlacementUsesWrongMonitor(wd.m_hwnd, it->second);

		if (monitorMismatch || dx > PLACEMENT_TOLERANCE || dy > PLACEMENT_TOLERANCE ||
			dw > PLACEMENT_TOLERANCE || dh > PLACEMENT_TOLERANCE) {
			TCHAR identity[512];
			TCHAR sz[1024];
			FormatWindowIdentity(wd.m_hwnd, wd.m_processId, wd.m_wndClass, wd.m_processPath, wd.m_windowTitle, identity, _countof(identity));
			StringCchPrintf(sz, _countof(sz),
				_T("%s: expected (%d,%d %dx%d) got (%d,%d %dx%d)%s"),
				identity,
				expected.left, expected.top,
				expected.right - expected.left, expected.bottom - expected.top,
				got.left, got.top,
				got.right - got.left, got.bottom - got.top,
				monitorMismatch ? _T("; maximized on a different monitor") : _T(""));
			mismatchCount++;
			wd.m_retryPending = TRUE;
			mismatchDetails.push_back(sz);
		}
		else {
			wd.m_retryPending = FALSE;
		}
	}
	if (mismatchCount == 0) {
		if (ignoredAccessDenied > 0 || skippedUserMoveCount > 0) {
			LOG_EVENTF(_T("VERIFY"),
				_T("All retry-eligible windows reached expected positions; %d user-moved window(s) and %d access-denied window(s) were excluded"),
				skippedUserMoveCount, ignoredAccessDenied);
		}
		else {
			LOG_EVENT(_T("VERIFY"), _T("All windows at expected positions"));
		}
		inst._RestoreRetryCount = 0;
		inst._AwaitingRestoreRetry = FALSE;
		SetRestorePhaseActive(FALSE);
		inst.InChangingState = false;
		ProcessDesktopWindows();
		return;
	}
	else if (inst._RestoreRetryCount < inst._RestoreRetryLimit) {
		inst._RestoreRetryCount++;
		inst._AwaitingRestoreRetry = TRUE;
		if (ignoredAccessDenied > 0 || skippedUserMoveCount > 0) {
			LOG_EVENTF(_T("VERIFY"),
				_T("%d window(s) still mismatched; waiting %d ms before retry %d/%d. Skipping %d user-moved window(s) and ignoring %d access-denied window(s)"),
				mismatchCount, inst._RestoreRetryDelayMs, inst._RestoreRetryCount, inst._RestoreRetryLimit,
				skippedUserMoveCount, ignoredAccessDenied);
		}
		else {
			LOG_EVENTF(_T("VERIFY"),
				_T("%d window(s) still mismatched; waiting %d ms before retry %d/%d"),
				mismatchCount, inst._RestoreRetryDelayMs, inst._RestoreRetryCount, inst._RestoreRetryLimit);
		}
		SetTimer(inst._MainWnd, VERIFY_TIMER_ID, inst._RestoreRetryDelayMs, NULL);
		UpdateStatusPanel();
		return;
	}
	else {
		for (const auto& detail : mismatchDetails) {
			LOG_EVENT(_T("WARNING"), detail.c_str());
		}
		if (ignoredAccessDenied > 0 || skippedUserMoveCount > 0) {
			LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position; %d user-moved window(s) and %d access-denied window(s) were excluded"),
				mismatchCount, skippedUserMoveCount, ignoredAccessDenied);
		}
		else {
			LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position"), mismatchCount);
		}
	}
	inst._RestoreRetryCount = 0;
	inst._AwaitingRestoreRetry = FALSE;
	SetRestorePhaseActive(FALSE);
	inst.InChangingState = false;
	UpdateStatusPanel();
}