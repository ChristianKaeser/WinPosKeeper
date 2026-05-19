#include "Persistence.h"

#include "AppState.h"

#include "LoggingUI.h"
#include "WindowTracking.h"

#define AUTOSTART_REG_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define AUTOSTART_VALUE _T("WinPosKeeper")
#define LEGACY_AUTOSTART_VALUE _T("MonWinPosKeeper")
#define LEGACY_AUTOSTART_VALUE_V1 _T("MonitorKeeper")
#define SETTINGS_REG_KEY _T("Software\\WinPosKeeper")
#define LEGACY_SETTINGS_REG_KEY _T("Software\\MonWinPosKeeper")
#define LEGACY_SETTINGS_REG_KEY_V1 _T("Software\\MonitorKeeper")
#define APPDATA_DIR_NAME _T("WinPosKeeper")
#define LEGACY_APPDATA_DIR_NAME _T("MonWinPosKeeper")
#define LEGACY_APPDATA_DIR_NAME_V1 _T("MonitorKeeper")
#define PERSIST_MAGIC_V5 0x4D4B5035
#define PERSIST_BOOT_MARKER_TOLERANCE_100NS (30ULL * 1000ULL * 1000ULL * 10ULL)

static BOOL GetPersistPathForFolder(LPCTSTR folderName, TCHAR* path, DWORD cch)
{
	if (FAILED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, path)))
		return FALSE;
	StringCchCat(path, cch, _T("\\"));
	StringCchCat(path, cch, folderName);
	CreateDirectory(path, NULL);
	StringCchCat(path, cch, _T("\\positions.dat"));
	return TRUE;
}

static ULONGLONG FileTimeToUInt64(const FILETIME& value)
{
	ULARGE_INTEGER result = {};
	result.LowPart = value.dwLowDateTime;
	result.HighPart = value.dwHighDateTime;
	return result.QuadPart;
}

static FILETIME UInt64ToFileTime(ULONGLONG value)
{
	ULARGE_INTEGER result = {};
	FILETIME fileTime = {};
	result.QuadPart = value;
	fileTime.dwLowDateTime = result.LowPart;
	fileTime.dwHighDateTime = result.HighPart;
	return fileTime;
}

static FILETIME GetCurrentBootMarkerUtc()
{
	FILETIME nowUtc = {};
	GetSystemTimeAsFileTime(&nowUtc);
	ULONGLONG nowValue = FileTimeToUInt64(nowUtc);
	ULONGLONG uptimeValue = GetTickCount64() * 10000ULL;
	return UInt64ToFileTime(nowValue > uptimeValue ? nowValue - uptimeValue : 0);
}

static DWORD GetCurrentSessionIdValue()
{
	DWORD sessionId = 0;
	if (!ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
		return 0xFFFFFFFFu;
	}
	return sessionId;
}

static int ClampSettingInt(int value, int minValue, int maxValue)
{
	if (value < minValue) {
		return minValue;
	}
	if (value > maxValue) {
		return maxValue;
	}
	return value;
}

static BOOL IsCompatiblePersistSession(const FILETIME& bootMarkerUtc, DWORD sessionId)
{
	if (sessionId != GetCurrentSessionIdValue()) {
		return FALSE;
	}

	ULONGLONG expected = FileTimeToUInt64(bootMarkerUtc);
	ULONGLONG current = FileTimeToUInt64(GetCurrentBootMarkerUtc());
	ULONGLONG delta = expected > current ? expected - current : current - expected;
	return delta <= PERSIST_BOOT_MARKER_TOLERANCE_100NS;
}

static void PopulateSavedWindowIdentity(SavedWindowData& data, HWND hwnd, DWORD processId)
{
	data.m_hwnd = hwnd;
	data.m_processId = processId;
	data.m_nUnusedCount = 0;
	data.m_lastRestoreError = ERROR_SUCCESS;
	data.m_retryPending = FALSE;

	RealGetWindowClass(hwnd, data.m_wndClass, _countof(data.m_wndClass));
	GetWindowText(hwnd, data.m_windowTitle, _countof(data.m_windowTitle));
	data.m_processPath[0] = '\0';

	HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (hProcess == NULL) {
		StringCchCopy(data.m_processPath, _countof(data.m_processPath), _T("<access denied>"));
		return;
	}

	DWORD cch = _countof(data.m_processPath);
	if (!QueryFullProcessImageName(hProcess, 0, data.m_processPath, &cch)) {
		StringCchCopy(data.m_processPath, _countof(data.m_processPath), _T("<access denied>"));
	}
	CloseHandle(hProcess);
}

BOOL IsAutostartEnabled()
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return FALSE;
	BOOL exists = (RegQueryValueEx(hKey, AUTOSTART_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) ||
		(RegQueryValueEx(hKey, LEGACY_AUTOSTART_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) ||
		(RegQueryValueEx(hKey, LEGACY_AUTOSTART_VALUE_V1, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
	RegCloseKey(hKey);
	return exists;
}

void SetAutostart(BOOL enable)
{
	HKEY hKey;
	LONG status = RegOpenKeyEx(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_WRITE, &hKey);
	if (status != ERROR_SUCCESS) {
		LogWin32Error(_T("WARNING"), _T("RegOpenKeyEx for autostart"), status);
		return;
	}
	if (enable) {
		TCHAR exePath[MAX_PATH];
		GetModuleFileName(NULL, exePath, MAX_PATH);
		status = RegSetValueEx(hKey, AUTOSTART_VALUE, 0, REG_SZ,
			reinterpret_cast<const BYTE*>(exePath),
			(DWORD)((lstrlen(exePath) + 1) * sizeof(TCHAR)));
		if (status != ERROR_SUCCESS) {
			LogWin32Error(_T("WARNING"), _T("RegSetValueEx for autostart"), status);
		}
		RegDeleteValue(hKey, LEGACY_AUTOSTART_VALUE);
		RegDeleteValue(hKey, LEGACY_AUTOSTART_VALUE_V1);
	}
	else {
		status = RegDeleteValue(hKey, AUTOSTART_VALUE);
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			LogWin32Error(_T("WARNING"), _T("RegDeleteValue for autostart"), status);
		}
		status = RegDeleteValue(hKey, LEGACY_AUTOSTART_VALUE);
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			LogWin32Error(_T("WARNING"), _T("RegDeleteValue for legacy autostart"), status);
		}
		status = RegDeleteValue(hKey, LEGACY_AUTOSTART_VALUE_V1);
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			LogWin32Error(_T("WARNING"), _T("RegDeleteValue for legacy v1 autostart"), status);
		}
	}
	RegCloseKey(hKey);
}

void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled,
	int restoreRetryDelayMs, int restoreRetryLimit, BOOL historyTrackingEnabled)
{
	HKEY hKey;
	LONG status = RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
	if (status != ERROR_SUCCESS) {
		LogWin32Error(_T("WARNING"), _T("RegCreateKeyEx for settings"), status);
		return;
	}
	DWORD val = restoreOnDisconnect ? 1 : 0;
	status = RegSetValueEx(hKey, _T("RestoreOnDisconnect"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx RestoreOnDisconnect"), status);
	val = persistPositions ? 1 : 0;
	status = RegSetValueEx(hKey, _T("PersistPositions"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx PersistPositions"), status);
	val = loggingEnabled ? 1 : 0;
	status = RegSetValueEx(hKey, _T("LoggingEnabled"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx LoggingEnabled"), status);
	val = (DWORD)ClampSettingInt(restoreRetryDelayMs,
		MIN_RESTORE_RETRY_DELAY_MS, MAX_RESTORE_RETRY_DELAY_MS);
	status = RegSetValueEx(hKey, _T("RestoreRetryDelayMs"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx RestoreRetryDelayMs"), status);
	val = (DWORD)ClampSettingInt(restoreRetryLimit,
		MIN_RESTORE_RETRY_LIMIT, MAX_RESTORE_RETRY_LIMIT);
	status = RegSetValueEx(hKey, _T("RestoreRetryLimit"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx RestoreRetryLimit"), status);
	val = historyTrackingEnabled ? 1 : 0;
	status = RegSetValueEx(hKey, _T("HistoryTrackingEnabled"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	if (status != ERROR_SUCCESS) LogWin32Error(_T("WARNING"), _T("RegSetValueEx HistoryTrackingEnabled"), status);
	RegCloseKey(hKey);
}

void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled,
	int& restoreRetryDelayMs, int& restoreRetryLimit, BOOL& historyTrackingEnabled)
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
		if (RegOpenKeyEx(HKEY_CURRENT_USER, LEGACY_SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
			if (RegOpenKeyEx(HKEY_CURRENT_USER, LEGACY_SETTINGS_REG_KEY_V1, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
				return;
		}
	}
	DWORD val;
	DWORD size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("RestoreOnDisconnect"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
		restoreOnDisconnect = val ? TRUE : FALSE;
	}
	else {
		size = sizeof(val);
		if (RegQueryValueEx(hKey, _T("SkipSingleMonitor"), NULL, NULL,
			reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
			restoreOnDisconnect = val ? FALSE : TRUE;
	}
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("PersistPositions"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
		persistPositions = val ? TRUE : FALSE;
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("LoggingEnabled"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
		loggingEnabled = val ? TRUE : FALSE;
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("RestoreRetryDelayMs"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
		restoreRetryDelayMs = ClampSettingInt((int)val,
			MIN_RESTORE_RETRY_DELAY_MS, MAX_RESTORE_RETRY_DELAY_MS);
	}
	else {
		size = sizeof(val);
		if (RegQueryValueEx(hKey, _T("RestoreRetryDelaySeconds"), NULL, NULL,
			reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
			ULONGLONG legacyDelayMs = (ULONGLONG)val * 1000ULL;
			restoreRetryDelayMs = ClampSettingInt(
				legacyDelayMs > (ULONGLONG)INT_MAX ? INT_MAX : (int)legacyDelayMs,
				MIN_RESTORE_RETRY_DELAY_MS, MAX_RESTORE_RETRY_DELAY_MS);
		}
	}
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("RestoreRetryLimit"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
		restoreRetryLimit = ClampSettingInt((int)val,
			MIN_RESTORE_RETRY_LIMIT, MAX_RESTORE_RETRY_LIMIT);
	}
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("HistoryTrackingEnabled"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS) {
		historyTrackingEnabled = val ? TRUE : FALSE;
	}
	RegCloseKey(hKey);
}

BOOL InstanceData::GetPersistPath(TCHAR* path, DWORD cch)
{
	return GetPersistPathForFolder(APPDATA_DIR_NAME, path, cch);
}

BOOL InstanceData::SaveToDisk(LPCTSTR reason)
{
	if (!PersistPositions) return FALSE;

	TCHAR path[MAX_PATH];
	if (!GetPersistPath(path, MAX_PATH)) {
		LOG_EVENT(_T("WARNING"), _T("Unable to resolve persistence path"));
		return FALSE;
	}

	HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		LogWin32Error(_T("WARNING"), _T("CreateFile for persistence"), GetLastError());
		return FALSE;
	}

	DWORD written;
	FILETIME bootMarkerUtc = GetCurrentBootMarkerUtc();
	DWORD sessionId = GetCurrentSessionIdValue();
	DWORD magic = PERSIST_MAGIC_V5;
	WriteFile(hFile, &magic, sizeof(magic), &written, NULL);
	WriteFile(hFile, &bootMarkerUtc, sizeof(bootMarkerUtc), &written, NULL);
	WriteFile(hFile, &sessionId, sizeof(sessionId), &written, NULL);

	DWORD snapshotCount = (DWORD)_ConfigSnapshots.size();
	int persistedPlacementCount = 0;
	WriteFile(hFile, &snapshotCount, sizeof(snapshotCount), &written, NULL);
	for (const auto& snapshot : _ConfigSnapshots) {
		WriteFile(hFile, &snapshot.first, sizeof(snapshot.first), &written, NULL);
		WriteFile(hFile, &snapshot.second.lastSavedUtc, sizeof(snapshot.second.lastSavedUtc), &written, NULL);
		WriteFile(hFile, &snapshot.second.windowCount, sizeof(snapshot.second.windowCount), &written, NULL);
		DWORD monitorCount = (DWORD)snapshot.second.monitorLayout.size();
		WriteFile(hFile, &monitorCount, sizeof(monitorCount), &written, NULL);
		for (const auto& monitor : snapshot.second.monitorLayout) {
			WriteFile(hFile, &monitor, sizeof(monitor), &written, NULL);
		}
	}

	DWORD entryCount = 0;
	for (const auto& wd : _WindowData) {
		if (wd.m_hwnd != NULL && wd.m_processId != 0 && IsWindow(wd.m_hwnd) && wd.m_wndClass[0] != '\0' && !wd.m_placements.empty())
			entryCount++;
	}
	WriteFile(hFile, &entryCount, sizeof(entryCount), &written, NULL);

	for (const auto& wd : _WindowData) {
		if (wd.m_hwnd == NULL || wd.m_processId == 0 || !IsWindow(wd.m_hwnd) ||
			wd.m_wndClass[0] == '\0' || wd.m_placements.empty())
			continue;
		UINT_PTR hwndValue = reinterpret_cast<UINT_PTR>(wd.m_hwnd);
		WriteFile(hFile, &hwndValue, sizeof(hwndValue), &written, NULL);
		WriteFile(hFile, &wd.m_processId, sizeof(wd.m_processId), &written, NULL);
		WriteFile(hFile, wd.m_wndClass, sizeof(wd.m_wndClass), &written, NULL);
		DWORD placementCount = (DWORD)wd.m_placements.size();
		persistedPlacementCount += (int)placementCount;
		WriteFile(hFile, &placementCount, sizeof(placementCount), &written, NULL);
		for (const auto& pair : wd.m_placements) {
			WriteFile(hFile, &pair.first, sizeof(pair.first), &written, NULL);
			WriteFile(hFile, &pair.second, sizeof(pair.second), &written, NULL);
		}
	}

	CloseHandle(hFile);
	GetSystemTimeAsFileTime(&_LastPersistUtc);

	TCHAR message[512];
	StringCchPrintf(message, _countof(message),
		_T("Persisted %lu live window record(s), %d placement(s), across %lu config snapshot(s) for same-session recovery (%s)"),
		entryCount, persistedPlacementCount, snapshotCount, (reason != NULL) ? reason : _T("unspecified"));
	LOG_EVENT(_T("DISK"), message);
	UpdateStatusPanel();
	return TRUE;
}

void InstanceData::LoadFromDisk()
{
	TCHAR path[MAX_PATH];
	if (!GetPersistPath(path, MAX_PATH)) {
		LOG_EVENT(_T("WARNING"), _T("Unable to resolve persistence path while loading"));
		return;
	}

	HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		if (!GetPersistPathForFolder(LEGACY_APPDATA_DIR_NAME, path, MAX_PATH)) {
			if (!GetPersistPathForFolder(LEGACY_APPDATA_DIR_NAME_V1, path, MAX_PATH)) {
				return;
			}
		}
		hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			if (!GetPersistPathForFolder(LEGACY_APPDATA_DIR_NAME_V1, path, MAX_PATH)) {
				return;
			}
			hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				return;
			}
		}
	}

	FILETIME lastWriteUtc = {};
	GetFileTime(hFile, NULL, NULL, &lastWriteUtc);

	DWORD bytesRead;
	DWORD magic = 0;
	FILETIME bootMarkerUtc = {};
	DWORD sessionId = 0;
	if (!ReadFile(hFile, &magic, sizeof(magic), &bytesRead, NULL)) {
		LogWin32Error(_T("WARNING"), _T("ReadFile for persistence magic"), GetLastError());
		CloseHandle(hFile);
		return;
	}
	if (magic != PERSIST_MAGIC_V5) {
		LOG_EVENTF(_T("ERROR"), _T("Unsupported persisted data format on disk (magic=0x%08X, expected 0x%08X)"),
			magic, PERSIST_MAGIC_V5);
		CloseHandle(hFile);
		return;
	}
	if (!ReadFile(hFile, &bootMarkerUtc, sizeof(bootMarkerUtc), &bytesRead, NULL) || bytesRead != sizeof(bootMarkerUtc) ||
		!ReadFile(hFile, &sessionId, sizeof(sessionId), &bytesRead, NULL) || bytesRead != sizeof(sessionId)) {
		LogWin32Error(_T("WARNING"), _T("ReadFile for persistence session header"), GetLastError());
		CloseHandle(hFile);
		return;
	}
	if (!IsCompatiblePersistSession(bootMarkerUtc, sessionId)) {
		LOG_EVENT(_T("DISK"), _T("Ignoring persisted positions captured in a different Windows session or boot"));
		CloseHandle(hFile);
		return;
	}

	DWORD snapshotCount = 0;
	if (!ReadFile(hFile, &snapshotCount, sizeof(snapshotCount), &bytesRead, NULL)) {
		LogWin32Error(_T("WARNING"), _T("ReadFile for snapshot count"), GetLastError());
		CloseHandle(hFile);
		return;
	}
	if (snapshotCount > 10000) {
		LOG_EVENTF(_T("ERROR"), _T("Persisted snapshot count is unreasonable: %lu"), snapshotCount);
		CloseHandle(hFile);
		return;
	}
	for (DWORD i = 0; i < snapshotCount; i++) {
		UINT64 configHash = 0;
		ConfigSnapshotInfo info;
		DWORD monitorCount = 0;
		if (!ReadFile(hFile, &configHash, sizeof(configHash), &bytesRead, NULL) || bytesRead != sizeof(configHash))
			break;
		if (!ReadFile(hFile, &info.lastSavedUtc, sizeof(info.lastSavedUtc), &bytesRead, NULL) || bytesRead != sizeof(info.lastSavedUtc))
			break;
		if (!ReadFile(hFile, &info.windowCount, sizeof(info.windowCount), &bytesRead, NULL) || bytesRead != sizeof(info.windowCount))
			break;
		if (!ReadFile(hFile, &monitorCount, sizeof(monitorCount), &bytesRead, NULL) || bytesRead != sizeof(monitorCount))
			break;
		if (monitorCount > 64) {
			LOG_EVENTF(_T("ERROR"), _T("Persisted monitor count is unreasonable: %lu"), monitorCount);
			CloseHandle(hFile);
			return;
		}
		info.monitorLayout.resize((size_t)monitorCount);
		for (DWORD monitorIndex = 0; monitorIndex < monitorCount; ++monitorIndex) {
			if (!ReadFile(hFile, &info.monitorLayout[(size_t)monitorIndex], sizeof(MonitorInfo), &bytesRead, NULL) ||
				bytesRead != sizeof(MonitorInfo)) {
				info.monitorLayout.resize((size_t)monitorIndex);
				break;
			}
		}
		_ConfigSnapshots[configHash] = info;
		GetOrCreateConfigId(configHash);
	}

	DWORD entryCount = 0;
	ReadFile(hFile, &entryCount, sizeof(entryCount), &bytesRead, NULL);

	if (entryCount > 10000) {
		LOG_EVENTF(_T("ERROR"), _T("Persisted entry count is unreasonable: %lu"), entryCount);
		CloseHandle(hFile);
		return;
	}

	DWORD loadedEntryCount = 0;
	int loadedPlacementCount = 0;

	for (DWORD e = 0; e < entryCount; e++) {
		UINT_PTR hwndValue = 0;
		DWORD processId = 0;
		TCHAR wndClass[40];
		if (!ReadFile(hFile, &hwndValue, sizeof(hwndValue), &bytesRead, NULL) || bytesRead != sizeof(hwndValue))
			break;
		if (!ReadFile(hFile, &processId, sizeof(processId), &bytesRead, NULL) || bytesRead != sizeof(processId))
			break;
		if (!ReadFile(hFile, wndClass, sizeof(wndClass), &bytesRead, NULL) || bytesRead != sizeof(wndClass))
			break;
		wndClass[39] = '\0';

		DWORD placementCount = 0;
		if (!ReadFile(hFile, &placementCount, sizeof(placementCount), &bytesRead, NULL))
			break;
		if (placementCount > MAX_CONFIGSLOTS) break;

		SavedWindowData* pData = nullptr;
		HWND hwnd = reinterpret_cast<HWND>(hwndValue);
		DWORD liveProcessId = 0;
		TCHAR liveClass[40] = _T("");
		if (hwnd != NULL && IsWindow(hwnd)) {
			GetWindowThreadProcessId(hwnd, &liveProcessId);
			RealGetWindowClass(hwnd, liveClass, _countof(liveClass));
			if (liveProcessId == processId && lstrcmp(liveClass, wndClass) == 0) {
				pData = FindWindowSlot(hwnd, processId, wndClass);
				PopulateSavedWindowIdentity(*pData, hwnd, processId);
			}
		}

		for (DWORD p = 0; p < placementCount; p++) {
			UINT64 configHash;
			WINDOWPLACEMENT wp;
			if (!ReadFile(hFile, &configHash, sizeof(configHash), &bytesRead, NULL) || bytesRead != sizeof(configHash))
				break;
			if (!ReadFile(hFile, &wp, sizeof(wp), &bytesRead, NULL) || bytesRead != sizeof(wp))
				break;
			if (pData != NULL && wp.length == sizeof(WINDOWPLACEMENT)) {
				pData->m_placements[configHash] = wp;
				GetOrCreateConfigId(configHash);
				loadedPlacementCount++;
			}
		}
		if (pData != NULL) {
			loadedEntryCount++;
		}
	}

	CloseHandle(hFile);
	_LastPersistUtc = lastWriteUtc;

	TCHAR message[256];
	StringCchPrintf(message, _countof(message),
		_T("Loaded persisted data from disk for same-session recovery (%lu window record(s), %d placement(s))"),
		loadedEntryCount, loadedPlacementCount);
	LOG_EVENT(_T("DISK"), message);
}

VOID CALLBACK PersistTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(idEvent);
	UNREFERENCED_PARAMETER(dwTime);
	PurgeClosedWindowHistoryEntries();
	FlushPendingWindowHistoryEntries();
	InstanceData::g_Instance.SaveToDisk(_T("periodic 5-minute flush"));
}