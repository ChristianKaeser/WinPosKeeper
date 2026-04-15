#pragma once

#include "AppState.h"

struct MonitorInfo {
	RECT rcMonitor;
	TCHAR szDevice[CCHDEVICENAME];
	TCHAR szFriendlyName[128];
};

void GetCurrentMonitorLayout(std::vector<MonitorInfo>& monitors);
void FormatMonitorSummary(const std::vector<MonitorInfo>& monitors, TCHAR* buffer, size_t cchBuffer);
void GetCurrentMonitorSummary(TCHAR* buffer, size_t cchBuffer);
UINT64 ComputeMonitorConfigHash();
int GetCurrentMonitorCount();
void ProcessMonitors();
VOID CALLBACK TimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
VOID CALLBACK WinEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd,
	LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);
HWINEVENTHOOK HookDisplayChange();