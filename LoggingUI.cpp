#include "LoggingUI.h"

#include "AppState.h"

#include "MonitorConfig.h"
#include "Persistence.h"

BOOL ShouldLogEvents()
{
	return InstanceData::g_Instance.LoggingEnabled;
}

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

void FormatFileTimeLocal(const FILETIME* fileTimeUtc, TCHAR* buffer, size_t cchBuffer)
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

void FormatWin32Error(DWORD error, TCHAR* buffer, size_t cchBuffer)
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

void LogWin32Error(LPCTSTR type, LPCTSTR context, DWORD error)
{
	TCHAR errorText[256];
	FormatWin32Error(error, errorText, _countof(errorText));
	LogEventFormat(type, _T("%s failed: %s (%lu)"), context, errorText, error);
}

HICON LoadAppIconSized(HINSTANCE instance, int width, int height)
{
	HICON icon = (HICON)LoadImage(instance, MAKEINTRESOURCE(IDI_MONITORKEEPER), IMAGE_ICON,
		width, height, LR_DEFAULTCOLOR | LR_SHARED);
	if (icon == NULL) {
		icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_MONITORKEEPER));
	}
	return icon;
}

HICON LoadAppIcon(HINSTANCE instance, BOOL isSmall)
{
	int width = GetSystemMetrics(isSmall ? SM_CXSMICON : SM_CXICON);
	int height = GetSystemMetrics(isSmall ? SM_CYSMICON : SM_CYICON);
	return LoadAppIconSized(instance, width, height);
}

std::basic_string<TCHAR> GetLogEntryText(HWND hList, int index)
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

std::basic_string<TCHAR> GetAllLogText(HWND hList)
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

BOOL CopyTextToClipboard(HWND hWndOwner, const std::basic_string<TCHAR>& text)
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

void ShowLogContextMenu(HWND hWnd, int x, int y)
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