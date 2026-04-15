#include "Persistence.h"

#include "LoggingUI.h"

#define AUTOSTART_REG_KEY _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define AUTOSTART_VALUE _T("MonWinPosKeeper")
#define LEGACY_AUTOSTART_VALUE _T("MonitorKeeper")
#define SETTINGS_REG_KEY _T("Software\\MonWinPosKeeper")
#define LEGACY_SETTINGS_REG_KEY _T("Software\\MonitorKeeper")
#define APPDATA_DIR_NAME _T("MonWinPosKeeper")
#define LEGACY_APPDATA_DIR_NAME _T("MonitorKeeper")
#define PERSIST_MAGIC_V2 0x4D4B5032

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

BOOL IsAutostartEnabled()
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return FALSE;
	BOOL exists = (RegQueryValueEx(hKey, AUTOSTART_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) ||
		(RegQueryValueEx(hKey, LEGACY_AUTOSTART_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
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
	}
	RegCloseKey(hKey);
}

void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled)
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
	RegCloseKey(hKey);
}

void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled)
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
		if (RegOpenKeyEx(HKEY_CURRENT_USER, LEGACY_SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
			return;
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
	DWORD magic = PERSIST_MAGIC_V2;
	WriteFile(hFile, &magic, sizeof(magic), &written, NULL);

	DWORD snapshotCount = (DWORD)_ConfigSnapshots.size();
	WriteFile(hFile, &snapshotCount, sizeof(snapshotCount), &written, NULL);
	for (const auto& snapshot : _ConfigSnapshots) {
		WriteFile(hFile, &snapshot.first, sizeof(snapshot.first), &written, NULL);
		WriteFile(hFile, &snapshot.second.lastSavedUtc, sizeof(snapshot.second.lastSavedUtc), &written, NULL);
		WriteFile(hFile, &snapshot.second.windowCount, sizeof(snapshot.second.windowCount), &written, NULL);
	}

	DWORD entryCount = 0;
	for (const auto& wd : _WindowData) {
		if (wd.m_wndClass[0] != '\0' && !wd.m_placements.empty())
			entryCount++;
	}
	WriteFile(hFile, &entryCount, sizeof(entryCount), &written, NULL);

	for (const auto& wd : _WindowData) {
		if (wd.m_wndClass[0] == '\0' || wd.m_placements.empty())
			continue;
		WriteFile(hFile, wd.m_wndClass, sizeof(wd.m_wndClass), &written, NULL);
		DWORD placementCount = (DWORD)wd.m_placements.size();
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
		_T("Persisted %lu window record(s), %d placement(s), across %lu config snapshot(s) to disk (%s)"),
		entryCount, CountTotalPlacements(), snapshotCount, (reason != NULL) ? reason : _T("unspecified"));
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
			return;
		}
		hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}

	FILETIME lastWriteUtc = {};
	GetFileTime(hFile, NULL, NULL, &lastWriteUtc);

	DWORD bytesRead;
	DWORD magic = 0;
	if (!ReadFile(hFile, &magic, sizeof(magic), &bytesRead, NULL)) {
		LogWin32Error(_T("WARNING"), _T("ReadFile for persistence magic"), GetLastError());
		CloseHandle(hFile);
		return;
	}
	if (magic != PERSIST_MAGIC_V2) {
		LOG_EVENTF(_T("ERROR"), _T("Unsupported persisted data format on disk (magic=0x%08X, expected 0x%08X)"),
			magic, PERSIST_MAGIC_V2);
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
		if (!ReadFile(hFile, &configHash, sizeof(configHash), &bytesRead, NULL) || bytesRead != sizeof(configHash))
			break;
		if (!ReadFile(hFile, &info.lastSavedUtc, sizeof(info.lastSavedUtc), &bytesRead, NULL) || bytesRead != sizeof(info.lastSavedUtc))
			break;
		if (!ReadFile(hFile, &info.windowCount, sizeof(info.windowCount), &bytesRead, NULL) || bytesRead != sizeof(info.windowCount))
			break;
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

	for (DWORD e = 0; e < entryCount; e++) {
		TCHAR wndClass[40];
		if (!ReadFile(hFile, wndClass, sizeof(wndClass), &bytesRead, NULL) || bytesRead != sizeof(wndClass))
			break;
		wndClass[39] = '\0';

		DWORD placementCount = 0;
		if (!ReadFile(hFile, &placementCount, sizeof(placementCount), &bytesRead, NULL))
			break;
		if (placementCount > MAX_CONFIGSLOTS) break;

		SavedWindowData* pData = nullptr;
		for (auto& wd : _WindowData) {
			if (lstrcmp(wd.m_wndClass, wndClass) == 0) {
				pData = &wd;
				break;
			}
		}
		if (!pData) {
			for (auto& wd : _WindowData) {
				if (wd.m_hwnd == NULL && wd.m_wndClass[0] == '\0') {
					pData = &wd;
					break;
				}
			}
		}
		if (!pData) {
			size_t oldSize = _WindowData.size();
			_WindowData.resize(oldSize + 32);
			pData = &_WindowData[oldSize];
		}

		lstrcpyn(pData->m_wndClass, wndClass, 40);

		for (DWORD p = 0; p < placementCount; p++) {
			UINT64 configHash;
			WINDOWPLACEMENT wp;
			if (!ReadFile(hFile, &configHash, sizeof(configHash), &bytesRead, NULL) || bytesRead != sizeof(configHash))
				break;
			if (!ReadFile(hFile, &wp, sizeof(wp), &bytesRead, NULL) || bytesRead != sizeof(wp))
				break;
			if (wp.length == sizeof(WINDOWPLACEMENT)) {
				pData->m_placements[configHash] = wp;
				GetOrCreateConfigId(configHash);
			}
		}
	}

	CloseHandle(hFile);
	_LastPersistUtc = lastWriteUtc;
	PersistPositions = true;

	TCHAR message[256];
	StringCchPrintf(message, _countof(message),
		_T("Loaded persisted data from disk (%lu window record(s), %d placement(s))"),
		entryCount, CountTotalPlacements());
	LOG_EVENT(_T("DISK"), message);
}

VOID CALLBACK PersistTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(idEvent);
	UNREFERENCED_PARAMETER(dwTime);
	InstanceData::g_Instance.SaveToDisk(_T("periodic 5-minute flush"));
}