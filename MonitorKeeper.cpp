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
//   - Optional skip of restore when going to a single monitor
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

#define LOGBUFFERSIZE  (32*1024)
#define MAX_LOADSTRING 100
// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
UINT WM_TASKBARCREATED = 0;                     // registered message for Explorer restart

#define AUTOSTART_REG_KEY   _T("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define AUTOSTART_VALUE     _T("MonitorKeeper")
#define SETTINGS_REG_KEY    _T("Software\\MonitorKeeper")

void LogMessage(LPCTSTR);

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
	if (RegOpenKeyEx(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
		return;
	if (enable) {
		TCHAR exePath[MAX_PATH];
		GetModuleFileName(NULL, exePath, MAX_PATH);
		RegSetValueEx(hKey, AUTOSTART_VALUE, 0, REG_SZ,
			reinterpret_cast<const BYTE*>(exePath),
			(DWORD)((lstrlen(exePath) + 1) * sizeof(TCHAR)));
	} else {
		RegDeleteValue(hKey, AUTOSTART_VALUE);
	}
	RegCloseKey(hKey);
}

static void SaveSettings(BOOL skipSingle, BOOL persistPositions)
{
	HKEY hKey;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
		return;
	DWORD val = skipSingle ? 1 : 0;
	RegSetValueEx(hKey, _T("SkipSingleMonitor"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	val = persistPositions ? 1 : 0;
	RegSetValueEx(hKey, _T("PersistPositions"), 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&val), sizeof(val));
	RegCloseKey(hKey);
}

static void LoadSettings(BOOL& skipSingle, BOOL& persistPositions)
{
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, SETTINGS_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
		return;
	DWORD val, size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("SkipSingleMonitor"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
		skipSingle = val ? TRUE : FALSE;
	size = sizeof(val);
	if (RegQueryValueEx(hKey, _T("PersistPositions"), NULL, NULL,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS)
		persistPositions = val ? TRUE : FALSE;
	RegCloseKey(hKey);
}

//
// Represents a snapshot of the current monitor layout (positions, sizes, device names).
// Two MonitorConfigs are "equal" if they have the same hash, which is computed from
// the sorted list of monitor rects and device names.
//
struct MonitorInfo {
	RECT  rcMonitor;
	TCHAR szDevice[CCHDEVICENAME];
};

static BOOL CALLBACK CollectMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
	auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);
	MONITORINFOEX mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfo(hMon, &mi)) {
		MonitorInfo info = {};
		info.rcMonitor = mi.rcMonitor;
		lstrcpyn(info.szDevice, mi.szDevice, CCHDEVICENAME);
		monitors->push_back(info);
	}
	return TRUE;
}

//
// Hash the current monitor configuration into a UINT64.
// Combines device names, positions and sizes using FNV-1a.
//
static UINT64 ComputeMonitorConfigHash()
{
	std::vector<MonitorInfo> monitors;
	EnumDisplayMonitors(NULL, NULL, CollectMonitorProc, reinterpret_cast<LPARAM>(&monitors));

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
		if (!GetWindowPlacement(hwnd, &wp)) return FALSE;

		m_placements[configHash] = wp;

		// Limit stored configs to prevent unbounded growth
		while (m_placements.size() > MAX_CONFIGSLOTS) {
			m_placements.erase(m_placements.begin());
		}

		return TRUE;
	}

	void RestoreWindow(UINT64 configHash)
	{
		TCHAR szTempClass[40];
		auto it = m_placements.find(configHash);
		if (it == m_placements.end()) return;

		if (IsWindow(m_hwnd) && it->second.length == sizeof(WINDOWPLACEMENT)) {
			// verify window class hasn't changed (HWND reuse)
			RealGetWindowClass(m_hwnd, szTempClass, sizeof(szTempClass) / sizeof(TCHAR));
			if (lstrcmp(szTempClass, m_wndClass) == 0) {
				WINDOWPLACEMENT place = it->second;

				if (place.showCmd == SW_MAXIMIZE) {
					// Restore to correct position first, then maximize.
					// Otherwise Windows maximizes on the current screen.
					place.showCmd = SW_SHOWNOACTIVATE;
					SetWindowPlacement(m_hwnd, &place);
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

				SetWindowPlacement(m_hwnd, &place);
			}
		}
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
#ifdef _DEBUG
		_LogInfo[0] = '\0';
#endif
		_ConfigHash = 0;
		_NumMonitors = 0;
		_WindowData.resize(32);
		_MainWnd = NULL;
		InChangingState = false;
		SkipSingleMonitorRestore = true;
		PersistPositions = false;

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

		if (_MutexSingleInstance)
		{
			::ReleaseMutex(_MutexSingleInstance);
			::CloseHandle(_MutexSingleInstance);
		}
	}

	static InstanceData  g_Instance;

	//
	// Primitive log window (debug builds only).
	void LogMessage(LPCTSTR str)
	{
#ifdef _DEBUG
		int len = lstrlen(_LogInfo);
		int newlen = lstrlen(str);
		if (len + newlen >= LOGBUFFERSIZE)
		{
			len = 0;
		}
		lstrcpy(_LogInfo + len, str);
		if (_MainWnd != NULL) {
			SetScrollPos(_MainWnd, SB_VERT, 10000, true);
			InvalidateRect(_MainWnd, NULL, TRUE);
		}
#endif
	}

	//
	// we are not notified on a window destroy, so we marked windows
	// if we haven't seen them. If we don't see it 3 times in a row,
	// we will reuse it's position in the array.
	//
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

	//
	// restore all the top level windows
	//
	void RestoreWindowPositions(UINT64 configHash)
	{
		for (auto& wd : _WindowData)
		{
			if (wd.m_hwnd != NULL && wd.m_nUnusedCount <= 2)
			{
				wd.RestoreWindow(configHash);
			}
		}
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
	//   DWORD magic = 0x4D4B5031 ("MKP1")
	//   DWORD entryCount
	//   For each entry:
	//     TCHAR wndClass[40]
	//     DWORD placementCount
	//     For each placement:
	//       UINT64 configHash
	//       WINDOWPLACEMENT wp
	//
	void SaveToDisk()
	{
		if (!PersistPositions) return;

		TCHAR path[MAX_PATH];
		if (!GetPersistPath(path, MAX_PATH)) return;

		HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) return;

		DWORD written;
		DWORD magic = 0x4D4B5031;
		WriteFile(hFile, &magic, sizeof(magic), &written, NULL);

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
	}

	void LoadFromDisk()
	{
		TCHAR path[MAX_PATH];
		if (!GetPersistPath(path, MAX_PATH)) return;

		HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) return;

		DWORD bytesRead;
		DWORD magic = 0;
		if (!ReadFile(hFile, &magic, sizeof(magic), &bytesRead, NULL) || magic != 0x4D4B5031) {
			CloseHandle(hFile);
			return;
		}

		DWORD entryCount = 0;
		ReadFile(hFile, &entryCount, sizeof(entryCount), &bytesRead, NULL);

		// Sanity cap
		if (entryCount > 10000) { CloseHandle(hFile); return; }

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
				}
			}
		}

		CloseHandle(hFile);
		PersistPositions = true;  // If file existed, enable persistence
	}

	HWINEVENTHOOK		_Hook;
	std::vector<SavedWindowData> _WindowData;
	UINT64				_ConfigHash;
	int					_NumMonitors;
	HWND				_MainWnd;
	BOOL				InChangingState;
	BOOL				SkipSingleMonitorRestore;
	BOOL				PersistPositions;
	BOOL				AlreadyRunning;
	HANDLE				_MutexSingleInstance;
#ifdef _DEBUG
	TCHAR				_LogInfo[LOGBUFFERSIZE];
#endif
};


/*static*/ InstanceData  InstanceData::g_Instance;

void LogMessage(LPCTSTR lpz)
{
	InstanceData::g_Instance.LogMessage(lpz);
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
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MONITORKEEPER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_MONITORKEEPER);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
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
				TCHAR sz[256];
				auto it = pData->m_placements.find(configHash);
				if (it != pData->m_placements.end()) {
					StringCchPrintf(sz, _countof(sz), _T("Save %s, cfg=%I64X, x=%d, y=%d, show=%s\n"),
						pData->m_wndClass, configHash,
						it->second.rcNormalPosition.left,
						it->second.rcNormalPosition.top,
						TranslateShowCommand(it->second.showCmd));
					LogMessage(sz);
				}
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
	if (newHash != InstanceData::g_Instance._ConfigHash)
	{
		// Optionally skip restore when going to a single monitor
		if (monitors == 1 && InstanceData::g_Instance.SkipSingleMonitorRestore)
		{
			LogMessage(_T("Config changed to single monitor - skipping restore\n"));
		}
		else
		{
			TCHAR sz[256];
			StringCchPrintf(sz, _countof(sz), _T("Config changed: %I64X -> %I64X (%d monitors)\n"),
				InstanceData::g_Instance._ConfigHash, newHash, monitors);
			LogMessage(sz);

			// restore windows to their saved positions for this config
			InstanceData::g_Instance.RestoreWindowPositions(newHash);
		}
	}
	InstanceData::g_Instance._ConfigHash = newHash;
	InstanceData::g_Instance._NumMonitors = monitors;
	InstanceData::g_Instance.InChangingState = false;
}


//
// called by hook for window changes
//
void ProcessDesktopWindows()
{
	TCHAR sz[256];
	UINT64 currentHash = ComputeMonitorConfigHash();
	if (currentHash != InstanceData::g_Instance._ConfigHash)
	{
		// we haven't completed our switch to change of monitors yet.
		// so don't save positions until we've repositioned things.
		return;
	}
	StringCchPrintf(sz, _countof(sz), _T("Save positions, cfg=%I64X, %d monitors\n"),
		currentHash, GetCurrentMonitorCount());
	InstanceData::g_Instance.TagWindowsUnused();
	InstanceData::g_Instance.LogMessage(sz);
	EnumDesktopWindows(NULL, SaveWindowsCallback, 0);
	InstanceData::g_Instance.SaveToDisk();
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
	return SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
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
   icon.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MONITORKEEPER));
   lstrcpy(icon.szTip, _T("Monitor Keeper"));
   Shell_NotifyIcon(NIM_ADD, &icon);

   icon.uVersion = NOTIFYICON_VERSION_4;
   Shell_NotifyIcon(NIM_SETVERSION, &icon);
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

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW & ~WS_VISIBLE,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   // Load user settings from registry
   LoadSettings(InstanceData::g_Instance.SkipSingleMonitorRestore,
                InstanceData::g_Instance.PersistPositions);

   // Load persisted positions (if any exist, this also enables PersistPositions)
   InstanceData::g_Instance.LoadFromDisk();

   ProcessDesktopWindows();
   InstanceData::g_Instance._ConfigHash = ComputeMonitorConfigHash();
   InstanceData::g_Instance._NumMonitors = GetCurrentMonitorCount();

   if (!hWnd)
   {
      return FALSE;
   }

   InstanceData::g_Instance._MainWnd = hWnd;
   InstanceData::g_Instance._Hook = HookDisplayChange();

   SetScrollRange(hWnd, SB_VERT, 0, 10000, false);

   // Register for Explorer restart notification so we can re-add the tray icon
   WM_TASKBARCREATED = RegisterWindowMessage(_T("TaskbarCreated"));

   AddTrayIcon(hWnd);

   // Set initial check states for menu items
   {
       HMENU menu = GetMenu(hWnd);
       menu = GetSubMenu(menu, 1);
       CheckMenuItem(menu, IDM_SKIP_SINGLE_MONITOR,
           InstanceData::g_Instance.SkipSingleMonitorRestore ? MF_CHECKED : MF_UNCHECKED);
       CheckMenuItem(menu, IDM_AUTOSTART, IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED);
       CheckMenuItem(menu, IDM_PERSIST_POSITIONS,
           InstanceData::g_Instance.PersistPositions ? MF_CHECKED : MF_UNCHECKED);
   }
   
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
		LogMessage(_T("WM_DISPLAYCHANGE\n"));
		KillTimer(hWnd, 2);  // Cancel any pending save to avoid saving mid-transition positions
		InstanceData::g_Instance.InChangingState = true;
		SetTimer(hWnd, 99, 500, TimerCallback);
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
				ShowWindow(hWnd, SW_RESTORE);
				UpdateWindow(hWnd);
				break;
			case IDM_SKIP_SINGLE_MONITOR:
				{
					InstanceData::g_Instance.SkipSingleMonitorRestore =
						!InstanceData::g_Instance.SkipSingleMonitorRestore;
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					CheckMenuItem(menu, IDM_SKIP_SINGLE_MONITOR,
						InstanceData::g_Instance.SkipSingleMonitorRestore ? MF_CHECKED : MF_UNCHECKED);
					SaveSettings(InstanceData::g_Instance.SkipSingleMonitorRestore,
						InstanceData::g_Instance.PersistPositions);
				}
				break;
			case IDM_AUTOSTART:
				{
					BOOL enabled = IsAutostartEnabled();
					SetAutostart(!enabled);
					HMENU menu = GetMenu(hWnd);
					menu = GetSubMenu(menu, 1);
					CheckMenuItem(menu, IDM_AUTOSTART, !enabled ? MF_CHECKED : MF_UNCHECKED);
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
					SaveSettings(InstanceData::g_Instance.SkipSingleMonitorRestore,
						InstanceData::g_Instance.PersistPositions);
					if (InstanceData::g_Instance.PersistPositions) {
						InstanceData::g_Instance.SaveToDisk();
					}
				}
				break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		return false;
	case (WM_USER+100):
		// notify icon
		{
		//
		// pop up our context menu on the notify icon.
		//
		UINT nMsg = LOWORD(lParam);
		if (nMsg == WM_CONTEXTMENU || nMsg == WM_RBUTTONUP) {

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
	case WM_VSCROLL: 
		{
			UINT cmd = LOWORD(wParam);
			int pos = GetScrollPos(hWnd, SB_VERT);
			if (cmd == SB_BOTTOM) {
				pos = 10000;
			}
			else if (cmd == SB_TOP) {
				pos += 0;
			}
			else if (cmd == SB_PAGEDOWN) {
				pos += 1000;
			}
			else if (cmd == SB_PAGEUP) {
				pos -= 1000;
			}
			else if (cmd == SB_THUMBPOSITION) {
				pos = HIWORD(wParam);
			}
			else if (cmd == SB_THUMBTRACK) {
				pos = HIWORD(wParam);
			}
			else {
				break;
			}
			if (pos < 0) pos = 0;
			if (pos > 10000) pos = 10000;
			SetScrollPos(hWnd, SB_VERT, pos, true);
			InvalidateRect(hWnd, NULL, true);
		}
		break;
    case WM_PAINT:
        {
#ifdef _DEBUG
            PAINTSTRUCT ps;
			RECT r,r2;
            HDC hdc = BeginPaint(hWnd, &ps);
			GetClientRect(hWnd, &r);
			r2 = r;
			DrawText(hdc, InstanceData::g_Instance._LogInfo,
				-1, &r2, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);
			
			//
			// get scroll position.
			//
			int pos = GetScrollPos(hWnd, SB_VERT);
			pos = pos * (r2.bottom - r.bottom)/ 10000;

			r.top = r.top - pos;
			DrawText(hdc, InstanceData::g_Instance._LogInfo,
				-1, &r, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);

            EndPaint(hWnd, &ps);
#endif
        }
        break;
    case WM_DESTROY:
		{
		InstanceData::g_Instance.SaveToDisk();
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
			LogMessage(_T("TaskbarCreated — re-adding tray icon\n"));
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
