#pragma once

#include "WinPosKeeper.h"

BOOL IsAutostartEnabled();
void SetAutostart(BOOL enable);
void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled,
	int restoreRetryDelayMs, int restoreRetryLimit, BOOL historyTrackingEnabled);
void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled,
	int& restoreRetryDelayMs, int& restoreRetryLimit, BOOL& historyTrackingEnabled);
VOID CALLBACK PersistTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);