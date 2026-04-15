// MonitorKeeper.cpp
//
// Original author: Garr Godfrey
// License: MIT License
//
// Monitor Keeper is a system-tray utility that automatically restores window positions
// when the monitor configuration changes. When a monitor is disconnected, Windows moves
// all windows onto the remaining display(s). Monitor Keeper remembers where each window
// was and puts it back when the monitor returns.
//
// Configuration detection uses an FNV-1a hash of all monitor device names and rects,
// so any layout change (not just monitor count) triggers a save/restore cycle.
//
// Features:
//   - Automatic save/restore of window positions per monitor configuration
//   - Persistent storage of positions to disk (%APPDATA%\MonitorKeeper\positions.dat)
//   - Optional "Start with Windows" autostart via registry
//   - Optional restore when monitors disconnect
//   - DPI-aware (PerMonitorV2)
//   - Survives Explorer restarts (re-creates tray icon)
//
// Limitations:
//   - Standard-user instance cannot move windows owned by elevated processes.
//     Run as administrator (e.g. via Task Scheduler) to work around this.
//   - Windows are restored to the state seen when the matching monitor config was
//     last active, so a window may change between minimized/maximized/normal.


#include "MonitorKeeper.h"


#define MAX_CONFIGSLOTS 16

#define MAX_LOG_ENTRIES  500
#define STATUS_HEIGHT    88
#define DISPLAY_SETTLE_TIMER_ID 99
#define DISPLAY_SETTLE_MS  2000
#define VERIFY_TIMER_ID  4
#define VERIFY_TIMER_MS  2000
#define PLACEMENT_TOLERANCE 20
#define MAX_LOADSTRING 100
// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
UINT WM_TASKBARCREATED = 0;                     // registered message for Explorer restart

#define AUTOSTART_REG_KEY   _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define AUTOSTART_VALUE     _T("MonitorKeeper")
#define SETTINGS_REG_KEY    _T("Software\\MonitorKeeper")
#define PERSIST_MAGIC_V2    0x4D4B5032

static BOOL ShouldLogEvents();
void LogEvent(LPCTSTR, LPCTSTR);
void LogEventFormat(LPCTSTR, LPCTSTR, ...);
void UpdateStatusPanel();
void UpdateLoggingUiState();
static LPCTSTR TranslateShowCommand(int nShowCmd);
static void FormatWindowIdentity(HWND hwnd, LPCTSTR fallbackClass, TCHAR* buffer, size_t cchBuffer);
static void FormatFileTimeLocal(const FILETIME* fileTimeUtc, TCHAR* buffer, size_t cchBuffer);
static void FormatWin32Error(DWORD error, TCHAR* buffer, size_t cchBuffer);
static void LogWin32Error(LPCTSTR type, LPCTSTR context, DWORD error);
static void GetCurrentMonitorSummary(TCHAR* buffer, size_t cchBuffer);
static HICON LoadAppIcon(HINSTANCE instance, BOOL isSmall);
static int NormalizeShowCommandForCompare(int showCmd);
static BOOL WindowPlacementNeedsRestore(const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual);
static void ShowMainWindow(HWND hWnd);
static void ShowLogContextMenu(HWND hWnd, int x, int y);
static BOOL CopyTextToClipboard(HWND hWndOwner, const std::basic_string<TCHAR>& text);
static std::basic_string<TCHAR> GetLogEntryText(HWND hList, int index);
static std::basic_string<TCHAR> GetAllLogText(HWND hList);

#define LOG_EVENT(type, text) \
	do { \
		if (ShouldLogEvents()) { \
			LogEvent((type), (text)); \
		} \
	} while (0)

#define LOG_EVENTF(type, format, ...) \
	do { \
		if (ShouldLogEvents()) { \
			LogEventFormat((type), (format), __VA_ARGS__); \
		} \
	} while (0)

static BOOL IsAutostartEnabled()
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return FALSE;
	BOOL exists = (RegQueryValueEx(hKey, AUTOSTART_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
	RegCloseKey(hKey);
	return exists;
}

static void SetAutostart(BOOL enable)
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
	} else {
		status = RegDeleteValue(hKey, AUTOSTART_VALUE);
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			LogWin32Error(_T("WARNING"), _T("RegDeleteValue for autostart"), status);
		}
	}
	RegCloseKey(hKey);
}

static void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled)
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

static void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled)
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return;
	DWORD val, size = sizeof(val);
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

struct ConfigSnapshotInfo {
	FILETIME lastSavedUtc;
	DWORD windowCount;

	ConfigSnapshotInfo()
	{
		lastSavedUtc.dwLowDateTime = 0;
		lastSavedUtc.dwHighDateTime = 0;
		windowCount = 0;
	}
};

//
// Represents a snapshot of the current monitor layout (positions, sizes, device names).
// Two MonitorConfigs are "equal" if they have the same hash, which is computed from
// the sorted list of monitor rects and device names.
//
struct MonitorInfo {
	RECT  rcMonitor;
	TCHAR szDevice[CCHDEVICENAME];
	TCHAR szFriendlyName[128];
};

static BOOL CALLBACK CollectMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
	auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
	MONITORINFOEX mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfo(hMon, &mi)) {
		MonitorInfo info = {};
		DISPLAY_DEVICE dd = {};
		dd.cb = sizeof(dd);
		info.rcMonitor = mi.rcMonitor;
		lstrcpyn(info.szDevice, mi.szDevice, CCHDEVICENAME);
		if (EnumDisplayDevices(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0] != '\0') {
			lstrcpyn(info.szFriendlyName, dd.DeviceString, _countof(info.szFriendlyName));
		}
		else {
			lstrcpyn(info.szFriendlyName, mi.szDevice, _countof(info.szFriendlyName));
		}
		monitors->push_back(info);
	}
	return TRUE;
}

static void GetCurrentMonitorLayout(std::vector<MonitorInfo>& monitors)
{
	monitors.clear();
	EnumDisplayMonitors(NULL, NULL, CollectMonitorProc, reinterpret_cast<LPARAM>(&monitors));
	std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
		if (a.rcMonitor.left != b.rcMonitor.left) return a.rcMonitor.left < b.rcMonitor.left;
		if (a.rcMonitor.top != b.rcMonitor.top) return a.rcMonitor.top < b.rcMonitor.top;
		if (a.rcMonitor.right != b.rcMonitor.right) return a.rcMonitor.right < b.rcMonitor.right;
		if (a.rcMonitor.bottom != b.rcMonitor.bottom) return a.rcMonitor.bottom < b.rcMonitor.bottom;
		return lstrcmp(a.szDevice, b.szDevice) < 0;
	});
}

static void FormatMonitorSummary(const std::vector<MonitorInfo>& monitors, TCHAR* buffer, size_t cchBuffer)
{
	StringCchPrintf(buffer, cchBuffer, _T("%d monitor(s)"), (int)monitors.size());
	if (monitors.empty()) {
		return;
	}

	StringCchCat(buffer, cchBuffer, _T(": "));
	for (size_t i = 0; i < monitors.size(); ++i) {
		TCHAR shortName[40];
		const TCHAR* fullName = monitors[i].szFriendlyName[0] ? monitors[i].szFriendlyName : monitors[i].szDevice;
		if (lstrlen(fullName) > 30) {
			StringCchCopyN(shortName, _countof(shortName), fullName, 27);
			StringCchCat(shortName, _countof(shortName), _T("..."));
		}
		else {
			StringCchCopy(shortName, _countof(shortName), fullName);
		}

		TCHAR part[160];
		StringCchPrintf(part, _countof(part), _T("%s (%dx%d @ %d,%d)"),
			shortName,
			monitors[i].rcMonitor.right - monitors[i].rcMonitor.left,
			monitors[i].rcMonitor.bottom - monitors[i].rcMonitor.top,
			monitors[i].rcMonitor.left,
			monitors[i].rcMonitor.top);
		StringCchCat(buffer, cchBuffer, part);
		if (i + 1 < monitors.size()) {
			StringCchCat(buffer, cchBuffer, _T(", "));
		}
	}
}

static void GetCurrentMonitorSummary(TCHAR* buffer, size_t cchBuffer)
{
	std::vector<MonitorInfo> monitors;
	GetCurrentMonitorLayout(monitors);
	FormatMonitorSummary(monitors, buffer, cchBuffer);
}

//
// Hash the current monitor configuration into a UINT64.
// Combines device names, positions and sizes using FNV-1a.
//
static UINT64 ComputeMonitorConfigHash()
{
	std::vector<MonitorInfo> monitors;
	GetCurrentMonitorLayout(monitors);

	// Sort by device name for deterministic ordering
	std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
		return lstrcmp(a.szDevice, b.szDevice) < 0;
	});

	// FNV-1a 64-bit
	UINT64 hash = 14695981039346656037ULL;
	auto fnvByte = [&](BYTE b) {
		hash ^= b;
		hash *= 1099511628211ULL;
	};
	auto fnvData = [&](const void* data, size_t len) {
		const BYTE* p = static_cast<const BYTE*>(data);
		for (size_t i = 0; i < len; i++) fnvByte(p[i]);
	};

	for (const auto& m : monitors) {
		fnvData(m.szDevice, lstrlen(m.szDevice) * sizeof(TCHAR));
		fnvData(&m.rcMonitor, sizeof(m.rcMonitor));
	}

	return hash;
}

static int GetCurrentMonitorCount()
{
	return GetSystemMetrics(SM_CMONITORS);
}

//
// class representing the data we save for each top level window.
// Now stores placements keyed by full monitor configuration hash.
//
class SavedWindowData {
public:
	SavedWindowData() {
		m_wndClass[0] = '\0';
		m_hwnd = NULL;
		m_nUnusedCount = 1;
	}

	int					m_nUnusedCount;
	std::map<UINT64, WINDOWPLACEMENT>  m_placements;  // configHash -> placement
	HWND				m_hwnd;
	TCHAR				m_wndClass[40];  // window class, for verification.

	BOOL SetData(HWND hwnd, UINT64 configHash)
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

		// Limit stored configs to prevent unbounded growth
		while (m_placements.size() > MAX_CONFIGSLOTS) {
			m_placements.erase(m_placements.begin());
		}

		return TRUE;
	}

	BOOL HasPlacement(UINT64 configHash) const
	{
		return m_placements.find(configHash) != m_placements.end();
	}

	BOOL RestoreWindow(UINT64 configHash)
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

		// verify window class hasn't changed (HWND reuse)
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
			// Restore to correct position first, then maximize.
			// Otherwise Windows maximizes on the current screen.
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
};


//
// Other global information we need for our application in this class, as
// well as methods that operate on the saved data.
//
class InstanceData {
public:
	InstanceData() {
		_Hook = NULL;
		_ConfigHash = 0;
		_NumMonitors = 0;
		_WindowData.resize(32);
		_ConfigIds.clear();
		_NextConfigId = 1;
		_ConfigSnapshots.clear();
		_MainWnd = NULL;
		_hStatus = NULL;
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

		// check running instance
		_MutexSingleInstance = ::CreateMutex(NULL, TRUE, APPLICATION_INSTANCE_MUTEX_NAME);
		AlreadyRunning = ::GetLastError() == ERROR_ALREADY_EXISTS;
	}

	~InstanceData()
	{
		Shutdown();
	}

	void Shutdown() {
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
		}
	}

	static InstanceData  g_Instance;

	void TagWindowsUnused()
	{
		for (auto& wd : _WindowData)
		{
			if (wd.m_hwnd != NULL && wd.m_nUnusedCount < 100)
			{
				wd.m_nUnusedCount++;
			}
		}
	}

	int CountTrackedWindows() const
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

	int CountSavedWindowRecords() const
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

	int CountTotalPlacements() const
	{
		int count = 0;
		for (const auto& wd : _WindowData)
		{
			count += (int)wd.m_placements.size();
		}
		return count;
	}

	int CountPlacementsForConfig(UINT64 configHash) const
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

	int GetOrCreateConfigId(UINT64 configHash)
	{
		auto it = _ConfigIds.find(configHash);
		if (it != _ConfigIds.end()) {
			return it->second;
		}

		int configId = _NextConfigId++;
		_ConfigIds[configHash] = configId;
		return configId;
	}

	BOOL TryGetSnapshotInfo(UINT64 configHash, ConfigSnapshotInfo& info) const
	{
		auto it = _ConfigSnapshots.find(configHash);
		if (it == _ConfigSnapshots.end())
			return FALSE;
		info = it->second;
		return TRUE;
	}

	//
	// restore all the top level windows
	//
	int RestoreWindowPositions(UINT64 configHash)
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

	//
	// Find an existing or available slot for the given window.
	SavedWindowData*	FindWindowSlot(HWND hwnd)
	{
		// find existing HWND in array
		for (auto& wd : _WindowData)
		{
			if (wd.m_hwnd == hwnd) {
				return &wd;
			}
		}

		// find an unused slot
		for (auto& wd : _WindowData)
		{
			if (wd.m_hwnd == NULL || wd.m_nUnusedCount > 2) {
				return &wd;
			}
		}

		// all used, grow the vector
		size_t oldSize = _WindowData.size();
		_WindowData.resize(oldSize + 32);
		return &_WindowData[oldSize];
	}

	static BOOL GetPersistPath(TCHAR* path, DWORD cch)
	{
		if (FAILED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, path)))
			return FALSE;
		StringCchCat(path, cch, _T("\\MonitorKeeper"));
		CreateDirectory(path, NULL);
		StringCchCat(path, cch, _T("\\positions.dat"));
		return TRUE;
	}

	//
	// File format (binary, little-endian, all fields fixed size):
	//   DWORD magic = 0x4D4B5032 ("MKP2")
	//   DWORD snapshotCount
	//   For each snapshot:
	//     UINT64 configHash
	//     FILETIME lastSavedUtc
	//     DWORD windowCount
	//   DWORD entryCount
	//   For each entry:
	//     TCHAR wndClass[40]
	//     DWORD placementCount
	//     For each placement:
	//       UINT64 configHash
	//       WINDOWPLACEMENT wp
	//
	BOOL SaveToDisk(LPCTSTR reason)
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

		// Count entries that have valid data
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

	void LoadFromDisk()
	{
		TCHAR path[MAX_PATH];
		if (!GetPersistPath(path, MAX_PATH)) {
			LOG_EVENT(_T("WARNING"), _T("Unable to resolve persistence path while loading"));
			return;
		}

		HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) return;

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

		// Sanity cap
		if (entryCount > 10000) {
			LOG_EVENTF(_T("ERROR"), _T("Persisted entry count is unreasonable: %lu"), entryCount);
			CloseHandle(hFile);
			return;
		}

		for (DWORD e = 0; e < entryCount; e++) {
			TCHAR wndClass[40];
			if (!ReadFile(hFile, wndClass, sizeof(wndClass), &bytesRead, NULL) || bytesRead != sizeof(wndClass))
				break;
			// Ensure null-terminated
			wndClass[39] = '\0';

			DWORD placementCount = 0;
			if (!ReadFile(hFile, &placementCount, sizeof(placementCount), &bytesRead, NULL))
				break;
			if (placementCount > MAX_CONFIGSLOTS) break;

			// Find or allocate a slot for this window class
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
		PersistPositions = true;  // If file existed, enable persistence

		TCHAR message[256];
		StringCchPrintf(message, _countof(message),
			_T("Loaded persisted data from disk (%lu window record(s), %d placement(s))"),
			entryCount, CountTotalPlacements());
		LOG_EVENT(_T("DISK"), message);
	}

	HWINEVENTHOOK		_Hook;
	std::map<UINT64, int> _ConfigIds;
	int					_NextConfigId;
	std::map<UINT64, ConfigSnapshotInfo> _ConfigSnapshots;
	std::vector<SavedWindowData> _WindowData;
	UINT64				_ConfigHash;
	int					_NumMonitors;
	HWND				_MainWnd;
	HWND				_hStatus;
	HWND				_hLogList;
	HFONT				_hLogFont;
	BOOL				InChangingState;
	BOOL				RestoreOnDisconnect;
	BOOL				PersistPositions;
	BOOL				LoggingEnabled;
	BOOL				AlreadyRunning;
	HANDLE				_MutexSingleInstance;
	FILETIME			_LastCaptureUtc;
	FILETIME			_LastPersistUtc;
};


/*static*/ InstanceData  InstanceData::g_Instance;

static BOOL ShouldLogEvents()
{
	return InstanceData::g_Instance.LoggingEnabled;
}

//
// Add a timestamped event to the log listbox.
//
void LogEvent(LPCTSTR type, LPCTSTR detail)
{
	if (!InstanceData::g_Instance.LoggingEnabled)
		return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	TCHAR buf[1024];
	StringCchPrintf(buf, _countof(buf), _T("%02d:%02d:%02d [%-7s] %s"),
		st.wHour, st.wMinute, st.wSecond, type, detail);

	HWND hList = InstanceData::g_Instance._hLogList;
	if (!hList) return;

	SendMessage(hList, WM_SETREDRAW, FALSE, 0);
	SendMessage(hList, LB_INSERTSTRING, 0, (LPARAM)buf);
	while (SendMessage(hList, LB_GETCOUNT, 0, 0) > MAX_LOG_ENTRIES)
		SendMessage(hList, LB_DELETESTRING, MAX_LOG_ENTRIES, 0);
	SendMessage(hList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(hList, NULL, TRUE);
}

void LogEventFormat(LPCTSTR type, LPCTSTR format, ...)
{
	if (!ShouldLogEvents())
		return;

	va_list args;
	va_start(args, format);
	TCHAR buffer[1024];
	StringCchVPrintf(buffer, _countof(buffer), format, args);
	va_end(args);
	LogEvent(type, buffer);
}

void UpdateLoggingUiState()
{
	if (InstanceData::g_Instance._hLogList != NULL) {
		EnableWindow(InstanceData::g_Instance._hLogList, InstanceData::g_Instance.LoggingEnabled);
	}
}

static void FormatFileTimeLocal(const FILETIME* fileTimeUtc, TCHAR* buffer, size_t cchBuffer)
{
	if (fileTimeUtc == NULL || (fileTimeUtc->dwLowDateTime == 0 && fileTimeUtc->dwHighDateTime == 0)) {
		StringCchCopy(buffer, cchBuffer, _T("never"));
		return;
	}

	FILETIME localTime = {};
	SYSTEMTIME st = {};
	if (FileTimeToLocalFileTime(fileTimeUtc, &localTime) && FileTimeToSystemTime(&localTime, &st)) {
		StringCchPrintf(buffer, cchBuffer, _T("%02d:%02d:%02d"), st.wHour, st.wMinute, st.wSecond);
	}
	else {
		StringCchCopy(buffer, cchBuffer, _T("unknown"));
	}
}

static void FormatWin32Error(DWORD error, TCHAR* buffer, size_t cchBuffer)
{
	DWORD chars = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error, 0, buffer, (DWORD)cchBuffer, NULL);
	if (chars == 0) {
		StringCchPrintf(buffer, cchBuffer, _T("Win32 error %lu"), error);
		return;
	}

	while (chars > 0 && (buffer[chars - 1] == '\r' || buffer[chars - 1] == '\n' || buffer[chars - 1] == ' ')) {
		buffer[chars - 1] = '\0';
		chars--;
	}
}

static void LogWin32Error(LPCTSTR type, LPCTSTR context, DWORD error)
{
	TCHAR errorText[256];
	FormatWin32Error(error, errorText, _countof(errorText));
	LogEventFormat(type, _T("%s failed: %s (%lu)"), context, errorText, error);
}

static HICON LoadAppIcon(HINSTANCE instance, BOOL isSmall)
{
	int width = GetSystemMetrics(isSmall ? SM_CXSMICON : SM_CXICON);
	int height = GetSystemMetrics(isSmall ? SM_CYSMICON : SM_CYICON);
	HICON icon = (HICON)LoadImage(instance, MAKEINTRESOURCE(IDI_MONITORKEEPER), IMAGE_ICON,
		width, height, LR_DEFAULTCOLOR | LR_SHARED);
	if (icon == NULL) {
		icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_MONITORKEEPER));
	}
	return icon;
}

static int NormalizeShowCommandForCompare(int showCmd)
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

static BOOL WindowPlacementNeedsRestore(const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual)
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

static void FormatWindowIdentity(HWND hwnd, LPCTSTR fallbackClass, TCHAR* buffer, size_t cchBuffer)
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

static void ShowMainWindow(HWND hWnd)
{
	ShowWindow(hWnd, SW_RESTORE);
	SetForegroundWindow(hWnd);
	UpdateWindow(hWnd);
}

static std::basic_string<TCHAR> GetLogEntryText(HWND hList, int index)
{
	if (hList == NULL || index == LB_ERR) {
		return std::basic_string<TCHAR>();
	}

	LRESULT length = SendMessage(hList, LB_GETTEXTLEN, index, 0);
	if (length == LB_ERR) {
		return std::basic_string<TCHAR>();
	}

	std::vector<TCHAR> buffer((size_t)length + 1, 0);
	if (SendMessage(hList, LB_GETTEXT, index, (LPARAM)buffer.data()) == LB_ERR) {
		return std::basic_string<TCHAR>();
	}

	return std::basic_string<TCHAR>(buffer.data());
}

static std::basic_string<TCHAR> GetAllLogText(HWND hList)
{
	std::basic_string<TCHAR> text;
	if (hList == NULL) {
		return text;
	}

	int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
	for (int i = 0; i < count; ++i) {
		std::basic_string<TCHAR> line = GetLogEntryText(hList, i);
		if (!line.empty()) {
			text.append(line);
			text.append(_T("\r\n"));
		}
	}
	return text;
}

static BOOL CopyTextToClipboard(HWND hWndOwner, const std::basic_string<TCHAR>& text)
{
	if (text.empty()) {
		LOG_EVENT(_T("WARNING"), _T("Clipboard copy requested with no text available"));
		return FALSE;
	}

	if (!OpenClipboard(hWndOwner)) {
		LogWin32Error(_T("WARNING"), _T("OpenClipboard"), GetLastError());
		return FALSE;
	}

	if (!EmptyClipboard()) {
		DWORD error = GetLastError();
		CloseClipboard();
		LogWin32Error(_T("WARNING"), _T("EmptyClipboard"), error);
		return FALSE;
	}

	SIZE_T bytes = (text.size() + 1) * sizeof(TCHAR);
	HGLOBAL hData = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hData == NULL) {
		CloseClipboard();
		LogWin32Error(_T("WARNING"), _T("GlobalAlloc for clipboard"), GetLastError());
		return FALSE;
	}

	void* pData = GlobalLock(hData);
	if (pData == NULL) {
		DWORD error = GetLastError();
		GlobalFree(hData);
		CloseClipboard();
		LogWin32Error(_T("WARNING"), _T("GlobalLock for clipboard"), error);
		return FALSE;
	}

	memcpy(pData, text.c_str(), bytes);
	GlobalUnlock(hData);

	if (SetClipboardData(CF_UNICODETEXT, hData) == NULL) {
		DWORD error = GetLastError();
		GlobalFree(hData);
		CloseClipboard();
		LogWin32Error(_T("WARNING"), _T("SetClipboardData"), error);
		return FALSE;
	}

	CloseClipboard();
	return TRUE;
}

static void ShowLogContextMenu(HWND hWnd, int x, int y)
{
	HWND hList = InstanceData::g_Instance._hLogList;
	if (hList == NULL) {
		return;
	}

	if (x == -1 || y == -1) {
		RECT rect;
		GetWindowRect(hList, &rect);
		x = rect.left + 12;
		y = rect.top + 12;
	}
	else {
		POINT pt = { x, y };
		ScreenToClient(hList, &pt);
		DWORD itemData = (DWORD)SendMessage(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
		if (HIWORD(itemData) == 0) {
			SendMessage(hList, LB_SETCURSEL, LOWORD(itemData), 0);
		}
	}

	HMENU menu = CreatePopupMenu();
	if (menu == NULL) {
		LogWin32Error(_T("WARNING"), _T("CreatePopupMenu"), GetLastError());
		return;
	}

	AppendMenu(menu, MF_STRING, IDM_COPY_LOG_ENTRY, _T("Copy Entry"));
	AppendMenu(menu, MF_STRING, IDM_COPY_ALL_LOG, _T("Copy All"));
	AppendMenu(menu, MF_SEPARATOR, 0, NULL);
	AppendMenu(menu, MF_STRING, IDM_CLEAR_LOG, _T("Clear Log"));

	int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
	int current = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
	if (count <= 0) {
		EnableMenuItem(menu, IDM_COPY_LOG_ENTRY, MF_BYCOMMAND | MF_GRAYED);
		EnableMenuItem(menu, IDM_COPY_ALL_LOG, MF_BYCOMMAND | MF_GRAYED);
		EnableMenuItem(menu, IDM_CLEAR_LOG, MF_BYCOMMAND | MF_GRAYED);
	}
	else if (current == LB_ERR) {
		EnableMenuItem(menu, IDM_COPY_LOG_ENTRY, MF_BYCOMMAND | MF_GRAYED);
	}

	SetForegroundWindow(hWnd);
	TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, x, y, 0, hWnd, NULL);
	DestroyMenu(menu);
	PostMessage(hWnd, WM_NULL, 0, 0);
}

//
// Refresh the status panel text with current state.
//
void UpdateStatusPanel()
{
	HWND hStatus = InstanceData::g_Instance._hStatus;
	if (!hStatus) return;

	auto& inst = InstanceData::g_Instance;

	int trackedWindows = inst.CountTrackedWindows();
	int windowRecords = inst.CountSavedWindowRecords();
	int currentConfigPlacements = inst.CountPlacementsForConfig(inst._ConfigHash);
	int totalPlacements = inst.CountTotalPlacements();
	std::vector<UINT64> configs;
	for (const auto& wd : inst._WindowData) {
		for (const auto& p : wd.m_placements) {
			configs.push_back(p.first);
		}
	}
	std::sort(configs.begin(), configs.end());
	configs.erase(std::unique(configs.begin(), configs.end()), configs.end());

	DWORD fileSize = 0;
	int configId = inst.GetOrCreateConfigId(inst._ConfigHash);
	TCHAR monitorSummary[1024];
	GetCurrentMonitorSummary(monitorSummary, _countof(monitorSummary));
	TCHAR path[MAX_PATH];
	if (InstanceData::GetPersistPath(path, MAX_PATH)) {
		HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			fileSize = GetFileSize(hFile, NULL);
			CloseHandle(hFile);
		}
	}

	TCHAR lastCapture[32];
	TCHAR lastPersist[32];
	FormatFileTimeLocal(&inst._LastCaptureUtc, lastCapture, _countof(lastCapture));
	FormatFileTimeLocal(&inst._LastPersistUtc, lastPersist, _countof(lastPersist));

	TCHAR text[1024];
	StringCchPrintf(text, _countof(text),
		_T("Config #%d: 0x%016I64X  |  Monitors: %s\r\n")
		_T("Stored configs: %d  |  Window records: %d  |  Current cfg positions: %d  |  Total placements: %d  |  Recent HWNDs: %d\r\n")
		_T("Last capture: %s  |  Last disk: %s  |  Disk: %d KB  |  Persist: %s  |  Restore on disconnect: %s  |  Autostart: %s  |  Logging: %s"),
		configId, inst._ConfigHash,
		monitorSummary,
		(int)configs.size(), windowRecords, currentConfigPlacements, totalPlacements, trackedWindows,
		lastCapture, lastPersist,
		fileSize / 1024, inst.PersistPositions ? _T("ON") : _T("OFF"),
		inst.RestoreOnDisconnect ? _T("ON") : _T("OFF"),
		IsAutostartEnabled() ? _T("ON") : _T("OFF"),
		inst.LoggingEnabled ? _T("ON") : _T("OFF"));

	SetWindowText(hStatus, text);
}



// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

	if (InstanceData::g_Instance.AlreadyRunning) {
		return FALSE;
	}


    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MONITORKEEPER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MONITORKEEPER));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
	wcex.hIcon          = LoadAppIcon(hInstance, FALSE);
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_MONITORKEEPER);
    wcex.lpszClassName  = szWindowClass;
	wcex.hIconSm        = LoadAppIcon(wcex.hInstance, TRUE);

    return RegisterClassExW(&wcex);
}


static LPCTSTR TranslateShowCommand(int nShowCmd)
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

//
// Called by EnumDesktopWindows whenever a window changes state.
// This will capture a lot of events.
//
BOOL CALLBACK SaveWindowsCallback(
	_In_ HWND   hwnd,
	_In_ LPARAM lParam
)
{
	UINT64 configHash = InstanceData::g_Instance._ConfigHash;

	//
	// only track windows that are visible, don't have a parent, 
	// have at least one style that is in the OVERLAPPEDWINDOW style and
	// do not have the WS_EX_NOACTIVE style.
	//
	// we include WS_EX_TOOLBAR because they sometimes are useful and get
	// moved as well.
	//
	if (IsWindowVisible(hwnd) &&
		GetParent(hwnd) == NULL) {
		DWORD dwStyle = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
		DWORD dwExStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
		if (((dwStyle & (WS_OVERLAPPEDWINDOW)) != 0  ||
			  (dwExStyle & WS_EX_APPWINDOW) != 0 )
			&&
			(dwExStyle & (WS_EX_NOACTIVATE)) == 0) 
		{
			SavedWindowData *pData = InstanceData::g_Instance.FindWindowSlot(hwnd);
			if (pData->SetData(hwnd, configHash))
			{
				int* pCount = reinterpret_cast<int*>(lParam);
				if (pCount) (*pCount)++;
			}
		}
	}
	return true;
}

//
// Process when the monitor configuration changes. If the config hash differs
// from the last known config, attempt to restore saved window positions.
//
void ProcessMonitors()
{
	UINT64 newHash = ComputeMonitorConfigHash();
	int monitors = GetCurrentMonitorCount();
	UINT64 oldHash = InstanceData::g_Instance._ConfigHash;
	int oldMonitorCount = InstanceData::g_Instance._NumMonitors;
	int oldConfigId = InstanceData::g_Instance.GetOrCreateConfigId(oldHash);
	int newConfigId = InstanceData::g_Instance.GetOrCreateConfigId(newHash);
	TCHAR monitorSummary[1024];
	GetCurrentMonitorSummary(monitorSummary, _countof(monitorSummary));
	bool configChanged = (newHash != oldHash);
	bool monitorCountDropped = (oldMonitorCount > 0 && monitors < oldMonitorCount);
	bool allowRestore = (!monitorCountDropped || InstanceData::g_Instance.RestoreOnDisconnect);
	bool didRestore = false;
	if (configChanged)
	{
		if (!allowRestore)
		{
			LOG_EVENTF(_T("CONFIG"), _T("Config #%d -> #%d, %s; skipping restore because restore-on-disconnect is disabled"),
				oldConfigId, newConfigId, monitorSummary);
		}
		else
		{
			LOG_EVENTF(_T("CONFIG"), _T("Config #%d -> #%d, %s"), oldConfigId, newConfigId, monitorSummary);

			// restore windows to their saved positions for this config
			int restored = InstanceData::g_Instance.RestoreWindowPositions(newHash);
			didRestore = (restored > 0);
			if (restored == 0 && InstanceData::g_Instance.CountPlacementsForConfig(newHash) == 0) {
				LOG_EVENTF(_T("WARNING"), _T("No matching saved placements available for config #%d"), newConfigId);
			}
			else if (restored == 0) {
				LOG_EVENTF(_T("RESTORE"), _T("Config #%d already matches stored placements; no window moves were needed"), newConfigId);
			}
		}
		InstanceData::g_Instance.SaveToDisk(_T("config change"));
	}
	else if (InstanceData::g_Instance.InChangingState)
	{
		LOG_EVENTF(_T("CONFIG"), _T("Display change settled on existing config #%d, %s"),
			newConfigId, monitorSummary);
		if (!allowRestore)
		{
			LOG_EVENT(_T("CONFIG"), _T("Skipping restore because restore-on-disconnect is disabled"));
		}
		else
		{
			int restored = InstanceData::g_Instance.RestoreWindowPositions(newHash);
			didRestore = (restored > 0);
			if (restored == 0 && InstanceData::g_Instance.CountPlacementsForConfig(newHash) == 0) {
				LOG_EVENTF(_T("WARNING"), _T("No matching saved placements available for config #%d"), newConfigId);
			}
			else if (restored == 0) {
				LOG_EVENTF(_T("RESTORE"), _T("Config #%d already matches stored placements; no window moves were needed"), newConfigId);
			}
		}
	}
	InstanceData::g_Instance._ConfigHash = newHash;
	InstanceData::g_Instance._NumMonitors = monitors;

	if (didRestore) {
		// Defer InChangingState=false until verify timer fires
		SetTimer(InstanceData::g_Instance._MainWnd, VERIFY_TIMER_ID, VERIFY_TIMER_MS, NULL);
	} else {
		InstanceData::g_Instance.InChangingState = false;
	}
	UpdateStatusPanel();
}


//
// called by hook for window changes
//
void ProcessDesktopWindows()
{
	UINT64 currentHash = ComputeMonitorConfigHash();
	if (currentHash != InstanceData::g_Instance._ConfigHash)
	{
		// we haven't completed our switch to change of monitors yet.
		// so don't save positions until we've repositioned things.
		return;
	}
	InstanceData::g_Instance.TagWindowsUnused();
	int configId = InstanceData::g_Instance.GetOrCreateConfigId(currentHash);
	int savedCount = 0;
	EnumDesktopWindows(NULL, SaveWindowsCallback, (LPARAM)&savedCount);
	GetSystemTimeAsFileTime(&InstanceData::g_Instance._LastCaptureUtc);
	ConfigSnapshotInfo& snapshot = InstanceData::g_Instance._ConfigSnapshots[currentHash];
	snapshot.lastSavedUtc = InstanceData::g_Instance._LastCaptureUtc;
	snapshot.windowCount = (DWORD)savedCount;

	//LOG_EVENTF(_T("SAVE"), _T("Captured %d top-level window position(s) in memory for config #%d"), savedCount, configId);
	UpdateStatusPanel();
}


//
// save windows positions after a slight delay
VOID CALLBACK SaveTimerCallback(
	_In_ HWND     hwnd,
	_In_ UINT     uMsg,
	_In_ UINT_PTR idEvent,
	_In_ DWORD    dwTime
)
{
	KillTimer(hwnd, idEvent);
	if (!InstanceData::g_Instance.InChangingState) {
		ProcessDesktopWindows();
	}
}


//
// Periodic timer to flush position data to disk (every 5 minutes).
//
#define PERSIST_TIMER_ID     3
#define PERSIST_TIMER_MS     (5 * 60 * 1000)

VOID CALLBACK PersistTimerCallback(
	_In_ HWND     hwnd,
	_In_ UINT     uMsg,
	_In_ UINT_PTR idEvent,
	_In_ DWORD    dwTime
)
{
	InstanceData::g_Instance.SaveToDisk(_T("periodic 5-minute flush"));
}


//
// Our window hook, grabbing the event when the active window changes.
VOID CALLBACK WinEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	if (InstanceData::g_Instance.InChangingState) return;
	if (hwnd != NULL &&
		dwEvent == EVENT_OBJECT_LOCATIONCHANGE)
	{
		// use our HWND so that this timer get replaced each time we call SetTimer.
		SetTimer(InstanceData::g_Instance._MainWnd, 2, 1000, SaveTimerCallback);
	}
}


//
// reposition windows after a slight delay
VOID CALLBACK TimerCallback(
	_In_ HWND     hwnd,
	_In_ UINT     uMsg,
	_In_ UINT_PTR idEvent,
	_In_ DWORD    dwTime
)
{
	ProcessMonitors();
	KillTimer(hwnd, idEvent);
}



HWINEVENTHOOK HookDisplayChange()
{
	HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
	if (hook == NULL) {
		LogWin32Error(_T("ERROR"), _T("SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE)"), GetLastError());
	}
	return hook;
}


//
// Create or re-create the notification tray icon.
// Called on startup and whenever Explorer restarts (TaskbarCreated).
//
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


//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   // Initialize common controls for listbox/etc.
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

   // Create status panel (static text, 3 lines)
   InstanceData::g_Instance._hStatus = CreateWindowEx(0, _T("STATIC"), _T(""),
      WS_CHILD | WS_VISIBLE | SS_LEFT,
      4, 4, 690, STATUS_HEIGHT,
      hWnd, NULL, hInstance, NULL);

   // Create event log listbox
   InstanceData::g_Instance._hLogList = CreateWindowEx(WS_EX_CLIENTEDGE, _T("LISTBOX"), _T(""),
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
	LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS,
      4, STATUS_HEIGHT + 8, 690, 400,
      hWnd, NULL, hInstance, NULL);

	if (InstanceData::g_Instance._hStatus == NULL || InstanceData::g_Instance._hLogList == NULL) {
		LogWin32Error(_T("ERROR"), _T("CreateWindowEx for main window child controls"), GetLastError());
		return FALSE;
	}

   // Set a reasonable font
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

   // Set horizontal scroll extent for long log lines
	SendMessage(InstanceData::g_Instance._hLogList, LB_SETHORIZONTALEXTENT, 4096, 0);

   // Load user settings from registry
	LoadSettings(InstanceData::g_Instance.RestoreOnDisconnect,
					 InstanceData::g_Instance.PersistPositions,
					 InstanceData::g_Instance.LoggingEnabled);

   // Load persisted positions (if any exist, this also enables PersistPositions)
   InstanceData::g_Instance.LoadFromDisk();

   ProcessDesktopWindows();
   InstanceData::g_Instance._ConfigHash = ComputeMonitorConfigHash();
   InstanceData::g_Instance._NumMonitors = GetCurrentMonitorCount();

   InstanceData::g_Instance._Hook = HookDisplayChange();

   // Register for Explorer restart notification so we can re-add the tray icon
   WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

   AddTrayIcon(hWnd);

   // Start periodic disk-save timer
   SetTimer(hWnd, PERSIST_TIMER_ID, PERSIST_TIMER_MS, PersistTimerCallback);

   // Set initial check states for menu items
   {
       HMENU menu = GetMenu(hWnd);
       menu = GetSubMenu(menu, 1);
	   CheckMenuItem(menu, IDM_RESTORE_ON_DISCONNECT,
	       InstanceData::g_Instance.RestoreOnDisconnect ? MF_CHECKED : MF_UNCHECKED);
       CheckMenuItem(menu, IDM_AUTOSTART, IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED);
       CheckMenuItem(menu, IDM_PERSIST_POSITIONS,
           InstanceData::g_Instance.PersistPositions ? MF_CHECKED : MF_UNCHECKED);
	   CheckMenuItem(menu, IDM_ENABLE_LOGGING,
		   InstanceData::g_Instance.LoggingEnabled ? MF_CHECKED : MF_UNCHECKED);
   }

   UpdateLoggingUiState();
   UpdateStatusPanel();
	LOG_EVENT(_T("INFO"), _T("MonitorKeeper started"));

   return TRUE;
}




//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
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
		KillTimer(hWnd, 2);  // Cancel any pending save to avoid saving mid-transition positions
		KillTimer(hWnd, DISPLAY_SETTLE_TIMER_ID);
		InstanceData::g_Instance.InChangingState = true;
		SetTimer(hWnd, DISPLAY_SETTLE_TIMER_ID, DISPLAY_SETTLE_MS, TimerCallback);
		break;
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
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
				{
					InstanceData::g_Instance.RestoreOnDisconnect =
						!InstanceData::g_Instance.RestoreOnDisconnect;
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					CheckMenuItem(menu, IDM_RESTORE_ON_DISCONNECT,
						InstanceData::g_Instance.RestoreOnDisconnect ? MF_CHECKED : MF_UNCHECKED);
					SaveSettings(InstanceData::g_Instance.RestoreOnDisconnect,
						InstanceData::g_Instance.PersistPositions,
						InstanceData::g_Instance.LoggingEnabled);
					LOG_EVENT(_T("INFO"), InstanceData::g_Instance.RestoreOnDisconnect
						? _T("Restore-on-disconnect enabled")
						: _T("Restore-on-disconnect disabled"));
					UpdateStatusPanel();
				}
				break;
			case IDM_AUTOSTART:
				{
					BOOL enabled = IsAutostartEnabled();
					SetAutostart(!enabled);
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					CheckMenuItem(menu, IDM_AUTOSTART, !enabled ? MF_CHECKED : MF_UNCHECKED);
					LOG_EVENT(_T("INFO"), !enabled ? _T("Autostart enabled") : _T("Autostart disabled"));
					UpdateStatusPanel();
				}
				break;
			case IDM_PERSIST_POSITIONS:
				{
					InstanceData::g_Instance.PersistPositions =
						!InstanceData::g_Instance.PersistPositions;
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					CheckMenuItem(menu, IDM_PERSIST_POSITIONS,
						InstanceData::g_Instance.PersistPositions ? MF_CHECKED : MF_UNCHECKED);
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
				}
				break;
			case IDM_ENABLE_LOGGING:
				{
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					if (InstanceData::g_Instance.LoggingEnabled) {
						LogEvent(_T("INFO"), _T("Event logging disabled"));
						InstanceData::g_Instance.LoggingEnabled = FALSE;
					}
					else {
						InstanceData::g_Instance.LoggingEnabled = TRUE;
						LogEvent(_T("INFO"), _T("Event logging enabled"));
					}
					CheckMenuItem(menu, IDM_ENABLE_LOGGING,
						InstanceData::g_Instance.LoggingEnabled ? MF_CHECKED : MF_UNCHECKED);
					SaveSettings(InstanceData::g_Instance.RestoreOnDisconnect,
						InstanceData::g_Instance.PersistPositions,
						InstanceData::g_Instance.LoggingEnabled);
					UpdateLoggingUiState();
					UpdateStatusPanel();
				}
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
	case (WM_USER+100):
		// notify icon
		{
		//
		// pop up our context menu on the notify icon.
		//
		UINT nMsg = LOWORD(lParam);
		if (nMsg == WM_LBUTTONDBLCLK || nMsg == NIN_SELECT || nMsg == NIN_KEYSELECT) {
			ShowMainWindow(hWnd);
		}
		else if (nMsg == WM_CONTEXTMENU || nMsg == WM_RBUTTONUP) {

			int x = GET_X_LPARAM(wParam);
			int y = GET_Y_LPARAM(wParam);
			HMENU menu = GetMenu(hWnd);
			menu = GetSubMenu(menu, 1);
			// SetForegroundWindow is required before TrackPopupMenu, otherwise
			// the menu won't dismiss when clicking outside of it.
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
			if (InstanceData::g_Instance._hStatus)
				MoveWindow(InstanceData::g_Instance._hStatus, 4, 4, cx - 8, STATUS_HEIGHT, TRUE);
			if (InstanceData::g_Instance._hLogList)
				MoveWindow(InstanceData::g_Instance._hLogList, 4, STATUS_HEIGHT + 8, cx - 8, cy - STATUS_HEIGHT - 12, TRUE);
		}
		break;
	case WM_TIMER:
		if (wParam == VERIFY_TIMER_ID) {
			KillTimer(hWnd, VERIFY_TIMER_ID);
			// Verify restored window positions
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
			} else {
				LOG_EVENTF(_T("WARNING"), _T("%d window(s) not at expected position"), warnCount);
			}
			inst.InChangingState = false;
			UpdateStatusPanel();
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
		// destroy our notify icon.
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
			// Explorer restarted — re-add our tray icon
			LOG_EVENT(_T("INFO"), _T("Explorer restarted - re-adding tray icon"));
			AddTrayIcon(hWnd);
			return 0;
		}
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
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
