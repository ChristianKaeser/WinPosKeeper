#pragma once

#include "AppState.h"

BOOL IsAutostartEnabled();
void SetAutostart(BOOL enable);
void SaveSettings(BOOL restoreOnDisconnect, BOOL persistPositions, BOOL loggingEnabled);
void LoadSettings(BOOL& restoreOnDisconnect, BOOL& persistPositions, BOOL& loggingEnabled);
VOID CALLBACK PersistTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);