#pragma once

#include "MonitorKeeper.h"

BOOL ShouldLogEvents();
void LogEvent(LPCTSTR type, LPCTSTR detail);
void LogEventFormat(LPCTSTR type, LPCTSTR format, ...);
void UpdateLoggingUiState();
void UpdateStatusPanel();
void FormatFileTimeLocal(const FILETIME* fileTimeUtc, TCHAR* buffer, size_t cchBuffer);
void FormatWin32Error(DWORD error, TCHAR* buffer, size_t cchBuffer);
void LogWin32Error(LPCTSTR type, LPCTSTR context, DWORD error);
HICON LoadAppIconSized(HINSTANCE instance, int width, int height);
HICON LoadAppIcon(HINSTANCE instance, BOOL isSmall);
std::basic_string<TCHAR> GetLogEntryText(HWND hList, int index);
std::basic_string<TCHAR> GetAllLogText(HWND hList);
BOOL CopyTextToClipboard(HWND hWndOwner, const std::basic_string<TCHAR>& text);
void ShowLogContextMenu(HWND hWnd, int x, int y);

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