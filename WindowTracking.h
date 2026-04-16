#pragma once

#include "MonitorKeeper.h"

LPCTSTR TranslateShowCommand(int nShowCmd);
void FormatWindowIdentity(HWND hwnd, LPCTSTR fallbackClass, TCHAR* buffer, size_t cchBuffer);
int NormalizeShowCommandForCompare(int showCmd);
BOOL WindowPlacementNeedsRestore(const WINDOWPLACEMENT& expected, const WINDOWPLACEMENT& actual);
BOOL CALLBACK SaveWindowsCallback(HWND hwnd, LPARAM lParam);
void ProcessDesktopWindows();
VOID CALLBACK SaveTimerCallback(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
void CancelPendingRestores();
void RetryPendingRestores();
void VerifyRestoredWindows();