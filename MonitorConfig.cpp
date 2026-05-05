#include "MonitorConfig.h"

#include "AppState.h"

#include "LoggingUI.h"
#include "WindowTracking.h"

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

void GetCurrentMonitorLayout(std::vector<MonitorInfo>& monitors)
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

void FormatMonitorSummary(const std::vector<MonitorInfo>& monitors, TCHAR* buffer, size_t cchBuffer)
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

void GetCurrentMonitorSummary(TCHAR* buffer, size_t cchBuffer)
{
	std::vector<MonitorInfo> monitors;
	GetCurrentMonitorLayout(monitors);
	FormatMonitorSummary(monitors, buffer, cchBuffer);
}

UINT64 ComputeMonitorConfigHash()
{
	std::vector<MonitorInfo> monitors;
	GetCurrentMonitorLayout(monitors);

	// Let's ignore szDevice - positions/layout are what really matter

	std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
		return std::tie(a.rcMonitor.left, a.rcMonitor.top, a.rcMonitor.right, a.rcMonitor.bottom) <
			std::tie(b.rcMonitor.left, b.rcMonitor.top, b.rcMonitor.right, b.rcMonitor.bottom);
	});

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
		//fnvData(m.szDevice, lstrlen(m.szDevice) * sizeof(TCHAR));
		fnvData(&m.rcMonitor, sizeof(m.rcMonitor));
	}

	return hash;
}

int GetCurrentMonitorCount()
{
	return GetSystemMetrics(SM_CMONITORS);
}

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

			int restored = InstanceData::g_Instance.RestoreWindowPositions(newHash);
			didRestore = (restored > 0);
			if (restored == 0 && InstanceData::g_Instance.CountPlacementsForConfig(newHash) == 0) {
				LOG_EVENTF(_T("WARNING"), _T("No matching saved placements available for config #%d"), newConfigId);
			}
			else if (restored == 0) {
				LOG_EVENTF(_T("RESTORE"), _T("Config #%d already matches stored placements; no window moves were needed"), newConfigId);
			}
		}
		//InstanceData::g_Instance.SaveToDisk(_T("config change"));
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
		SetTimer(InstanceData::g_Instance._MainWnd, VERIFY_TIMER_ID,
			InstanceData::g_Instance._RestoreRetryDelaySeconds * 1000, NULL);
		UpdateStatusPanel();
	}
	else {
		InstanceData::g_Instance.InChangingState = false;
		ProcessDesktopWindows();
	}
}

VOID CALLBACK TimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	UNREFERENCED_PARAMETER(uMsg);
	UNREFERENCED_PARAMETER(dwTime);
	ProcessMonitors();
	KillTimer(hwnd, idEvent);
}

VOID CALLBACK WinEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd,
	LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	UNREFERENCED_PARAMETER(hWinEventHook);
	UNREFERENCED_PARAMETER(idObject);
	UNREFERENCED_PARAMETER(idChild);
	UNREFERENCED_PARAMETER(dwEventThread);
	UNREFERENCED_PARAMETER(dwmsEventTime);
	if (hwnd != NULL && dwEvent == EVENT_OBJECT_LOCATIONCHANGE)
	{
		if (InstanceData::g_Instance._HistoryTrackingEnabled) {
			CaptureWindowHistoryEvent(hwnd, NULL, _T("location change"), FALSE);
		}
		if (InstanceData::g_Instance.InChangingState) return;
		SetTimer(InstanceData::g_Instance._MainWnd, CAPTURE_TIMER_ID, 1000, SaveTimerCallback);
	}
}

HWINEVENTHOOK HookDisplayChange()
{
	HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
		NULL, WinEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
	if (hook == NULL) {
		LogWin32Error(_T("ERROR"), _T("SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE)"), GetLastError());
	}
	return hook;
}