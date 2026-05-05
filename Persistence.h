#pragma once

#include "MonitorKeeper.h"

BOOL IsAutostartEnabled();
void SetAutostart(BOOL enable);
void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled,
	int restoreRetryDelaySeconds, int restoreRetryLimit);
void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled,
	int& restoreRetryDelaySeconds, int& restoreRetryLimit);
VOID CALLBACK PersistTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);