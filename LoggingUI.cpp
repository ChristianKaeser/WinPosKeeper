#include "LoggingUI.h"

#include "AppState.h"

#include "MonitorConfig.h"
#include "Persistence.h"
#include "WindowTracking.h"

#include <commdlg.h>

static int GetActiveConfigIdForLog()
{
	auto& inst = InstanceData::g_Instance;
	UINT64 configHash = inst._ConfigHash;
	if (configHash == 0 || inst.InChangingState) {
		UINT64 liveHash = ComputeMonitorConfigHash();
		if (liveHash != 0) {
			configHash = liveHash;
		}
	}
	if (configHash == 0) {
		return 0;
	}
	return inst.GetOrCreateConfigId(configHash);
}

static void FormatConfigTag(int configId, TCHAR* buffer, size_t cchBuffer)
{
	if (configId > 0) {
		StringCchPrintf(buffer, cchBuffer, _T("[cfg #%d]"), configId);
	}
	else {
		StringCchCopy(buffer, cchBuffer, _T("[cfg n/a]"));
	}
}

static std::basic_string<TCHAR> PrefixDetailWithConfigId(int configId, LPCTSTR detail)
{
	std::basic_string<TCHAR> text;
	if (configId > 0) {
		TCHAR prefix[32];
		StringCchPrintf(prefix, _countof(prefix), _T("[cfg #%d] "), configId);
		text = prefix;
	}
	if (detail != NULL) {
		text.append(detail);
	}
	return text;
}

BOOL ShouldLogEvents()
{
	auto& inst = InstanceData::g_Instance;
	return inst.LoggingEnabled || inst._HistoryTrackingEnabled;
}

static BOOL GetStartupDiagnosticsPath(TCHAR* path, size_t cchPath)
{
	if (path == NULL || cchPath == 0) {
		return FALSE;
	}

	DWORD length = GetTempPath((DWORD)cchPath, path);
	if (length == 0 || length >= cchPath) {
		return FALSE;
	}
	return SUCCEEDED(StringCchCat(path, cchPath, _T("WinPosKeeper-startup-errors.log"))) ? TRUE : FALSE;
}

static void AppendStartupDiagnosticsLine(LPCTSTR line)
{
	if (line == NULL || line[0] == '\0') {
		return;
	}

	TCHAR path[MAX_PATH];
	if (!GetStartupDiagnosticsPath(path, _countof(path))) {
		return;
	}

	HANDLE hFile = CreateFile(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}

	LARGE_INTEGER fileSize = {};
	if (GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart == 0) {
#ifdef UNICODE
		WCHAR bom = 0xFEFF;
		DWORD written = 0;
		WriteFile(hFile, &bom, sizeof(bom), &written, NULL);
#endif
	}

	static const TCHAR kLineBreak[] = _T("\r\n");
	DWORD written = 0;
	WriteFile(hFile, line, (DWORD)(lstrlen(line) * sizeof(TCHAR)), &written, NULL);
	WriteFile(hFile, kLineBreak, (DWORD)(lstrlen(kLineBreak) * sizeof(TCHAR)), &written, NULL);
	CloseHandle(hFile);
}

void ResetStartupDiagnostics()
{
	TCHAR path[MAX_PATH];
	if (GetStartupDiagnosticsPath(path, _countof(path))) {
		DeleteFile(path);
	}
}

void ReportStartupFailure(LPCTSTR context, DWORD error, LPCTSTR detail)
{
	SYSTEMTIME st = {};
	GetLocalTime(&st);

	TCHAR line[1024];
	if (error != ERROR_SUCCESS) {
		TCHAR errorText[256];
		FormatWin32Error(error, errorText, _countof(errorText));
		StringCchPrintf(line, _countof(line),
			_T("%04d-%02d-%02d %02d:%02d:%02d startup failure: %s failed: %s (%lu)%s%s"),
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			context != NULL ? context : _T("startup"), errorText, error,
			detail != NULL && detail[0] != '\0' ? _T(". ") : _T(""),
			detail != NULL ? detail : _T(""));
	}
	else {
		StringCchPrintf(line, _countof(line),
			_T("%04d-%02d-%02d %02d:%02d:%02d startup failure: %s%s%s"),
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			context != NULL ? context : _T("startup"),
			detail != NULL && detail[0] != '\0' ? _T(". ") : _T(""),
			detail != NULL ? detail : _T(""));
	}

	AppendStartupDiagnosticsLine(line);
	OutputDebugString(line);
	OutputDebugString(_T("\r\n"));

	TCHAR path[MAX_PATH];
	TCHAR message[1400];
	if (GetStartupDiagnosticsPath(path, _countof(path))) {
		StringCchPrintf(message, _countof(message),
			_T("%s\r\n\r\nDetails were written to:\r\n%s"), line, path);
	}
	else {
		StringCchCopy(message, _countof(message), line);
	}

	MessageBox(NULL, message, _T("WinPosKeeper Startup Failure"),
		MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST | MB_TASKMODAL);
}

static ULONGLONG FileTimeToUInt64ForSort(const FILETIME& value)
{
	ULARGE_INTEGER result = {};
	result.LowPart = value.dwLowDateTime;
	result.HighPart = value.dwHighDateTime;
	return result.QuadPart;
}

static void AppendHistoryLogEvent(LPCTSTR type, LPCTSTR detail, const FILETIME& recordedUtc)
{
	auto& inst = InstanceData::g_Instance;
	if (!inst._HistoryTrackingEnabled) {
		return;
	}

	HistoryLogEntry entry;
	entry.recordedUtc = recordedUtc;
	entry.configId = GetActiveConfigIdForLog();
	entry.sequence = ++inst._HistoryNextSequence;
	entry.type = type != NULL ? type : _T("");
	entry.detail = detail != NULL ? detail : _T("");
	inst._HistoryLog.push_back(entry);
	if (inst._HistoryLog.size() > MAX_HISTORY_LOG_EVENTS) {
		inst._HistoryLog.erase(inst._HistoryLog.begin(),
			inst._HistoryLog.begin() + (inst._HistoryLog.size() - MAX_HISTORY_LOG_EVENTS));
	}
	RefreshWindowHistoryInspector();
}

void LogEvent(LPCTSTR type, LPCTSTR detail)
{
	FILETIME recordedUtc = {};
	GetSystemTimeAsFileTime(&recordedUtc);
	AppendHistoryLogEvent(type, detail, recordedUtc);

	if (!InstanceData::g_Instance.LoggingEnabled)
		return;

	SYSTEMTIME st;
	GetLocalTime(&st);
	int configId = GetActiveConfigIdForLog();
	TCHAR configTag[32];
	FormatConfigTag(configId, configTag, _countof(configTag));
	TCHAR buf[1024];
	StringCchPrintf(buf, _countof(buf), _T("%02d:%02d:%02d [%-7s] %-11s %s"),
		st.wHour, st.wMinute, st.wSecond, type, configTag, detail);

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
		// If the time is today, only display the time; otherwise time and date:
		SYSTEMTIME now = {};
		GetLocalTime(&now);
		if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay) {
			StringCchPrintf(buffer, cchBuffer, _T("%02d:%02d:%02d"), st.wHour, st.wMinute, st.wSecond);
			return;
		}
		StringCchPrintf(buffer, cchBuffer, _T("%02d:%02d:%02d %4d-%02d-%02d"), st.wHour, st.wMinute, st.wSecond, st.wYear, st.wMonth, st.wDay);
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

static void CaptureInspectorSelection()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hConfigList == NULL || inst._InspectorConfigHashes.empty()) {
		return;
	}

	int selection = (int)SendMessage(inst._hConfigList, LB_GETCURSEL, 0, 0);
	if (selection != LB_ERR && selection >= 0 && selection < (int)inst._InspectorConfigHashes.size()) {
		inst._InspectorSelectedConfigHash = inst._InspectorConfigHashes[(size_t)selection];
	}
}

static void CollectKnownConfigHashes(std::vector<UINT64>& configHashes)
{
	auto& inst = InstanceData::g_Instance;
	configHashes.clear();

	for (const auto& pair : inst._ConfigIds) {
		configHashes.push_back(pair.first);
	}
	for (const auto& pair : inst._ConfigSnapshots) {
		configHashes.push_back(pair.first);
	}
	for (const auto& wd : inst._WindowData) {
		for (const auto& placement : wd.m_placements) {
			configHashes.push_back(placement.first);
		}
	}

	std::sort(configHashes.begin(), configHashes.end(), [&](UINT64 left, UINT64 right) {
		int leftId = inst.GetOrCreateConfigId(left);
		int rightId = inst.GetOrCreateConfigId(right);
		if (leftId != rightId) {
			return leftId < rightId;
		}
		return left < right;
	});
	configHashes.erase(std::unique(configHashes.begin(), configHashes.end()), configHashes.end());
}

static BOOL IsLayoutsViewVisible()
{
	auto& inst = InstanceData::g_Instance;
	return inst._MainWnd != NULL && IsWindowVisible(inst._MainWnd) &&
		inst._hConfigList != NULL && inst._hConfigSummary != NULL && inst._hPlacementList != NULL &&
		IsWindowVisible(inst._hConfigList) && IsWindowVisible(inst._hConfigSummary) &&
		IsWindowVisible(inst._hPlacementList);
}

static LPCTSTR DescribePlacementRecordState(const SavedWindowData& wd)
{
	return (wd.m_hwnd != NULL && IsWindow(wd.m_hwnd) && wd.m_nUnusedCount <= 2) ? _T("open") : _T("saved");
}

static void GetSnapshotMonitorSummary(UINT64 configHash, const ConfigSnapshotInfo& snapshotInfo, BOOL hasSnapshot,
	TCHAR* buffer, size_t cchBuffer)
{
	auto& inst = InstanceData::g_Instance;
	if (configHash == inst._ConfigHash) {
		GetCurrentMonitorSummary(buffer, cchBuffer);
	}
	else if (hasSnapshot && !snapshotInfo.monitorLayout.empty()) {
		FormatMonitorSummary(snapshotInfo.monitorLayout, buffer, cchBuffer);
	}
	else {
		StringCchCopy(buffer, cchBuffer,
			_T("Not recorded for this historical layout yet. It will appear after that layout becomes active again."));
	}
}

static void UpdatePlacementInspectorDetails(UINT64 selectedHash)
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hConfigSummary == NULL || inst._hPlacementList == NULL) {
		return;
	}

	SendMessage(inst._hPlacementList, WM_SETREDRAW, FALSE, 0);
	SendMessage(inst._hPlacementList, LB_RESETCONTENT, 0, 0);

	if (selectedHash == 0) {
		SetWindowText(inst._hConfigSummary, _T("No saved layout snapshots yet."));
		SendMessage(inst._hPlacementList, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(inst._hPlacementList, NULL, TRUE);
		return;
	}

	ConfigSnapshotInfo snapshotInfo;
	BOOL hasSnapshot = inst.TryGetSnapshotInfo(selectedHash, snapshotInfo);
	TCHAR lastSaved[32];
	if (hasSnapshot) {
		FormatFileTimeLocal(&snapshotInfo.lastSavedUtc, lastSaved, _countof(lastSaved));
	}
	else {
		StringCchCopy(lastSaved, _countof(lastSaved), _T("unknown"));
	}

	TCHAR monitorSummary[1024];
	GetSnapshotMonitorSummary(selectedHash, snapshotInfo, hasSnapshot, monitorSummary, _countof(monitorSummary));

	TCHAR summary[2048];
	StringCchPrintf(summary, _countof(summary),
		_T("Saved layout:        #%-3d   %s\r\n")
		_T("Config hash:         0x%016I64X\r\n")
		_T("Saved placements:    %d window record(s)\r\n")
		_T("Last full snapshot:  %lu visible top-level window(s)\r\n")
		_T("Snapshot time:       %s\r\n")
		_T("Monitor layout:      %s"),
		inst.GetOrCreateConfigId(selectedHash),
		selectedHash == inst._ConfigHash ? _T("current layout") : _T("historical layout"),
		selectedHash,
		inst.CountPlacementsForConfig(selectedHash),
		hasSnapshot ? snapshotInfo.windowCount : 0,
		lastSaved,
		monitorSummary);
	SetWindowText(inst._hConfigSummary, summary);
	
	SendMessage(inst._hPlacementList, LB_ADDSTRING, 0,
		(LPARAM)_T("Window   Left    Top  Width Height Show State     Process / Window Title"));
	SendMessage(inst._hPlacementList, LB_ADDSTRING, 0,
		(LPARAM)_T("------ ------ ------ ------ ------ -------------  --------------------------------"));

	for (const auto& wd : inst._WindowData) {
		auto it = wd.m_placements.find(selectedHash);
		if (it == wd.m_placements.end()) {
			continue;
		}

		const WINDOWPLACEMENT& place = it->second;
		TCHAR identity[512];
		TCHAR line[1400];
		FormatWindowIdentity(wd.m_hwnd, wd.m_processId, wd.m_wndClass, wd.m_processPath, wd.m_windowTitle, identity, _countof(identity));
		StringCchPrintf(line, _countof(line),
			_T("%-6s %6d %6d %6d %6d %-13s  %s"),
			DescribePlacementRecordState(wd),
			place.rcNormalPosition.left,
			place.rcNormalPosition.top,
			place.rcNormalPosition.right - place.rcNormalPosition.left,
			place.rcNormalPosition.bottom - place.rcNormalPosition.top,
			TranslateShowCommand(place.showCmd),
			identity);
		SendMessage(inst._hPlacementList, LB_ADDSTRING, 0, (LPARAM)line);
	}

	if (SendMessage(inst._hPlacementList, LB_GETCOUNT, 0, 0) == 0) {
		SendMessage(inst._hPlacementList, LB_ADDSTRING, 0,
			(LPARAM)_T("No stored window placements for this config."));
	}

	SendMessage(inst._hPlacementList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(inst._hPlacementList, NULL, TRUE);
}

static void NormalizeLineEndings(std::basic_string<TCHAR>& text);

static BOOL DecodeTextBytes(const BYTE* bytes, DWORD byteCount, std::basic_string<TCHAR>& text)
{
	if (bytes == NULL || byteCount == 0) {
		return FALSE;
	}

	DWORD offset = 0;
	if (byteCount >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
		offset = 3;
	}

	int wideLength = MultiByteToWideChar(CP_UTF8, 0,
		reinterpret_cast<LPCCH>(bytes + offset), (int)(byteCount - offset), NULL, 0);
	UINT codePage = CP_UTF8;
	if (wideLength == 0) {
		codePage = CP_ACP;
		wideLength = MultiByteToWideChar(codePage, 0,
			reinterpret_cast<LPCCH>(bytes + offset), (int)(byteCount - offset), NULL, 0);
	}
	if (wideLength == 0) {
		return FALSE;
	}

	std::wstring wideText((size_t)wideLength, L'\0');
	if (MultiByteToWideChar(codePage, 0,
		reinterpret_cast<LPCCH>(bytes + offset), (int)(byteCount - offset),
		&wideText[0], wideLength) == 0) {
		return FALSE;
	}

	text.assign(wideText.begin(), wideText.end());
	NormalizeLineEndings(text);
	return TRUE;
}

static void NormalizeLineEndings(std::basic_string<TCHAR>& text)
{
	std::basic_string<TCHAR> normalized;
	normalized.reserve(text.size() + 32);

	for (size_t index = 0; index < text.size(); ++index) {
		TCHAR ch = text[index];
		if (ch == '\r') {
			normalized.append(_T("\r\n"));
			if (index + 1 < text.size() && text[index + 1] == '\n') {
				index++;
			}
		}
		else if (ch == '\n') {
			normalized.append(_T("\r\n"));
		}
		else {
			normalized.push_back(ch);
		}
	}

	text.swap(normalized);
}

static BOOL TryLoadReadmeResource(std::basic_string<TCHAR>& text)
{
	HRSRC hResource = FindResource(hInst, MAKEINTRESOURCE(IDR_EMBEDDED_README), RT_RCDATA);
	if (hResource == NULL) {
		return FALSE;
	}

	DWORD resourceSize = SizeofResource(hInst, hResource);
	if (resourceSize == 0) {
		return FALSE;
	}

	HGLOBAL hLoadedResource = LoadResource(hInst, hResource);
	if (hLoadedResource == NULL) {
		return FALSE;
	}

	const BYTE* bytes = reinterpret_cast<const BYTE*>(LockResource(hLoadedResource));
	if (bytes == NULL) {
		return FALSE;
	}

	return DecodeTextBytes(bytes, resourceSize, text);
}

void RefreshReadmeView()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hReadmeView == NULL) {
		return;
	}

	if (inst._ReadmeText.empty()) {
		if (!TryLoadReadmeResource(inst._ReadmeText)) {
			inst._ReadmeText =
				_T("The embedded README resource could not be loaded.\r\n\r\n")
				_T("This build is supposed to carry its README inside the executable so deployment stays a single standalone .exe.");
		}
	}

	SetWindowText(inst._hReadmeView, inst._ReadmeText.c_str());
}

void RefreshPlacementInspector()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hConfigList == NULL || inst._hConfigSummary == NULL || inst._hPlacementList == NULL) {
		return;
	}
	if (!IsLayoutsViewVisible()) {
		return;
	}

	CaptureInspectorSelection();

	std::vector<UINT64> configHashes;
	CollectKnownConfigHashes(configHashes);
	inst._InspectorConfigHashes = configHashes;

	SendMessage(inst._hConfigList, WM_SETREDRAW, FALSE, 0);
	SendMessage(inst._hConfigList, LB_RESETCONTENT, 0, 0);

	for (UINT64 configHash : configHashes) {
		TCHAR line[1400];
		TCHAR configId[16];
		ConfigSnapshotInfo snapshotInfo;
		BOOL hasSnapshot = inst.TryGetSnapshotInfo(configHash, snapshotInfo);
		TCHAR lastSaved[32];
		TCHAR monitorSummary[768];
		if (hasSnapshot) {
			FormatFileTimeLocal(&snapshotInfo.lastSavedUtc, lastSaved, _countof(lastSaved));
		}
		else {
			StringCchCopy(lastSaved, _countof(lastSaved), _T("unknown"));
		}
		GetSnapshotMonitorSummary(configHash, snapshotInfo, hasSnapshot, monitorSummary, _countof(monitorSummary));
		StringCchPrintf(configId, _countof(configId), _T("#%d"), inst.GetOrCreateConfigId(configHash));

		StringCchPrintf(line, _countof(line),
			_T("%-6s %-10s placements=%-4d snapshot=%-4lu last=%-19s %s"),
			configId,
			configHash == inst._ConfigHash ? _T("current") : _T("saved"),
			inst.CountPlacementsForConfig(configHash),
			hasSnapshot ? snapshotInfo.windowCount : 0,
			lastSaved,
			monitorSummary);
		SendMessage(inst._hConfigList, LB_ADDSTRING, 0, (LPARAM)line);
	}

	int selectedIndex = LB_ERR;
	if (!configHashes.empty()) {
		UINT64 preferredHash = inst._InspectorSelectedConfigHash != 0 ? inst._InspectorSelectedConfigHash : inst._ConfigHash;
		for (size_t index = 0; index < configHashes.size(); ++index) {
			if (configHashes[index] == preferredHash) {
				selectedIndex = (int)index;
				break;
			}
		}
		if (selectedIndex == LB_ERR) {
			selectedIndex = 0;
		}
		inst._InspectorSelectedConfigHash = configHashes[(size_t)selectedIndex];
		SendMessage(inst._hConfigList, LB_SETCURSEL, selectedIndex, 0);
	}
	else {
		inst._InspectorSelectedConfigHash = 0;
	}

	SendMessage(inst._hConfigList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(inst._hConfigList, NULL, TRUE);
	UpdatePlacementInspectorDetails(inst._InspectorSelectedConfigHash);
}

struct HistoryTimelineDisplayEntry {
	FILETIME recordedUtc;
	ULONGLONG sequence;
	BOOL isWindowEvent;
	RECT rect;
	BOOL hasPlacement;
	int showCmd;
	std::basic_string<TCHAR> source;
	std::basic_string<TCHAR> detail;
	std::basic_string<TCHAR> windowTitle;
};

static void FormatFileTimePreciseLocal(const FILETIME* fileTimeUtc, TCHAR* buffer, size_t cchBuffer)
{
	if (fileTimeUtc == NULL || (fileTimeUtc->dwLowDateTime == 0 && fileTimeUtc->dwHighDateTime == 0)) {
		StringCchCopy(buffer, cchBuffer, _T("never"));
		return;
	}

	FILETIME localTime = {};
	SYSTEMTIME st = {};
	if (!FileTimeToLocalFileTime(fileTimeUtc, &localTime) || !FileTimeToSystemTime(&localTime, &st)) {
		StringCchCopy(buffer, cchBuffer, _T("unknown"));
		return;
	}

	SYSTEMTIME now = {};
	GetLocalTime(&now);
	if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay) {
		StringCchPrintf(buffer, cchBuffer, _T("%02d:%02d:%02d.%03d"),
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		return;
	}

	StringCchPrintf(buffer, cchBuffer, _T("%04d-%02d-%02d %02d:%02d:%02d.%03d"),
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static void FormatHistoryRect(const RECT* rect, TCHAR* buffer, size_t cchBuffer)
{
	if (rect == NULL) {
		StringCchCopy(buffer, cchBuffer, _T("-"));
		return;
	}

	StringCchPrintf(buffer, cchBuffer, _T("(%6d,%6d %5dx%-5d)"),
		rect->left,
		rect->top,
		rect->right - rect->left,
		rect->bottom - rect->top);
}

static BOOL IsHistoryViewVisible()
{
	auto& inst = InstanceData::g_Instance;
	return inst._MainWnd != NULL && IsWindowVisible(inst._MainWnd) &&
		inst._hHistoryWindowList != NULL && inst._hHistorySummary != NULL && inst._hHistoryTimelineList != NULL &&
		IsWindowVisible(inst._hHistoryWindowList) && IsWindowVisible(inst._hHistorySummary) &&
		IsWindowVisible(inst._hHistoryTimelineList);
}

static void CaptureHistorySelection()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hHistoryWindowList == NULL || inst._HistoryWindowKeys.empty()) {
		return;
	}

	int selection = (int)SendMessage(inst._hHistoryWindowList, LB_GETCURSEL, 0, 0);
	if (selection != LB_ERR && selection >= 0 && selection < (int)inst._HistoryWindowKeys.size()) {
		inst._HistorySelectedHwnd = inst._HistoryWindowKeys[(size_t)selection];
	}
}

static void CollectWindowHistoryKeys(std::vector<UINT_PTR>& hwndValues)
{
	auto& inst = InstanceData::g_Instance;
	hwndValues.clear();
	for (const auto& pair : inst._WindowHistory) {
		if (!pair.second.entries.empty()) {
			hwndValues.push_back(pair.first);
		}
	}

	std::sort(hwndValues.begin(), hwndValues.end(), [&](UINT_PTR left, UINT_PTR right) {
		const WindowHistoryData& leftHistory = inst._WindowHistory[left];
		const WindowHistoryData& rightHistory = inst._WindowHistory[right];
		ULONGLONG leftTime = FileTimeToUInt64ForSort(leftHistory.lastRecordedUtc);
		ULONGLONG rightTime = FileTimeToUInt64ForSort(rightHistory.lastRecordedUtc);
		if (leftTime != rightTime) {
			return leftTime > rightTime;
		}
		return left < right;
	});
}

static BOOL ShouldMergeHistoryLogEntryForWindow(const HistoryLogEntry& entry, DWORD processId)
{
	if (entry.type == _T("RESTORE") || entry.type == _T("DISK")) {
		TCHAR pidToken[32];
		StringCchPrintf(pidToken, _countof(pidToken), _T("pid=%6lu"), processId);
		return _tcsstr(entry.detail.c_str(), pidToken) != NULL;
	}
	return TRUE;
}

static void BuildWindowHistoryTimeline(UINT_PTR selectedHwnd,
	const WindowHistoryData*& historyData,
	std::vector<HistoryTimelineDisplayEntry>& rows)
{
	auto& inst = InstanceData::g_Instance;
	rows.clear();
	historyData = NULL;

	auto historyIt = inst._WindowHistory.find(selectedHwnd);
	if (historyIt == inst._WindowHistory.end()) {
		return;
	}

	historyData = &historyIt->second;
	rows.reserve(historyIt->second.entries.size() + inst._HistoryLog.size());
	for (const auto& entry : historyIt->second.entries) {
		HistoryTimelineDisplayEntry row = {};
		row.recordedUtc = entry.recordedUtc;
		row.sequence = entry.sequence;
		row.isWindowEvent = TRUE;
		row.rect = entry.rect;
		row.hasPlacement = entry.hasPlacement;
		row.showCmd = entry.showCmd;
		row.source = entry.source;
		row.detail = PrefixDetailWithConfigId(entry.configId, entry.detail.c_str());
		row.windowTitle = entry.windowTitle;
		rows.push_back(row);
	}

	for (const auto& entry : inst._HistoryLog) {
		if (!ShouldMergeHistoryLogEntryForWindow(entry, historyIt->second.processId)) {
			continue;
		}
		HistoryTimelineDisplayEntry row = {};
		row.recordedUtc = entry.recordedUtc;
		row.sequence = entry.sequence;
		row.isWindowEvent = FALSE;
		row.hasPlacement = FALSE;
		row.showCmd = SW_HIDE;
		row.source = _T("app:");
		row.source.append(entry.type);
		row.detail = PrefixDetailWithConfigId(entry.configId, entry.detail.c_str());
		rows.push_back(row);
	}

	std::sort(rows.begin(), rows.end(), [](const HistoryTimelineDisplayEntry& left,
		const HistoryTimelineDisplayEntry& right) {
		ULONGLONG leftTime = FileTimeToUInt64ForSort(left.recordedUtc);
		ULONGLONG rightTime = FileTimeToUInt64ForSort(right.recordedUtc);
		if (leftTime != rightTime) {
			return leftTime < rightTime;
		}
		return left.sequence < right.sequence;
	});
}

static void UpdateWindowHistoryDetails(UINT_PTR selectedHwnd)
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hHistorySummary == NULL || inst._hHistoryTimelineList == NULL) {
		return;
	}

	SendMessage(inst._hHistoryTimelineList, WM_SETREDRAW, FALSE, 0);
	SendMessage(inst._hHistoryTimelineList, LB_RESETCONTENT, 0, 0);

	if (!inst._HistoryTrackingEnabled) {
		SetWindowText(inst._hHistorySummary,
			_T("Enable in-memory window history tracking from the Settings tab to start recording window geometry changes and merged app events."));
		EnableWindow(inst._hHistoryExportButton, FALSE);
		SendMessage(inst._hHistoryTimelineList, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(inst._hHistoryTimelineList, NULL, TRUE);
		return;
	}

	const WindowHistoryData* historyData = NULL;
	std::vector<HistoryTimelineDisplayEntry> rows;
	BuildWindowHistoryTimeline(selectedHwnd, historyData, rows);
	if (selectedHwnd == 0 || historyData == NULL) {
		SetWindowText(inst._hHistorySummary, _T("No in-memory history entries have been captured yet."));
		EnableWindow(inst._hHistoryExportButton, FALSE);
		SendMessage(inst._hHistoryTimelineList, WM_SETREDRAW, TRUE, 0);
		InvalidateRect(inst._hHistoryTimelineList, NULL, TRUE);
		return;
	}

	UINT mergedAppEventCount = 0;
	for (const auto& row : rows) {
		if (!row.isWindowEvent) {
			mergedAppEventCount++;
		}
	}

	TCHAR lastSeen[64];
	FormatFileTimePreciseLocal(&historyData->lastRecordedUtc, lastSeen, _countof(lastSeen));
	TCHAR summary[2048];
	StringCchPrintf(summary, _countof(summary),
		_T("HWND:               0x%08IX\r\n")
		_T("PID:                %lu\r\n")
		_T("Class:              %s\r\n")
		_T("Process:            %s\r\n")
		_T("Latest title:       %s\r\n")
		_T("Window entries:     %u / %d\r\n")
		_T("Merged app events:  %u\r\n")
		_T("Last recorded:      %s"),
		historyData->hwndValue,
		historyData->processId,
		historyData->windowClass.empty() ? _T("<unknown>") : historyData->windowClass.c_str(),
		historyData->processPath.empty() ? _T("<unknown>") : historyData->processPath.c_str(),
		historyData->latestTitle.empty() ? _T("<untitled>") : historyData->latestTitle.c_str(),
		(UINT)historyData->entries.size(),
		MAX_HISTORY_ENTRIES_PER_WINDOW,
		mergedAppEventCount,
		lastSeen);
	SetWindowText(inst._hHistorySummary, summary);
	EnableWindow(inst._hHistoryExportButton, TRUE);

	SendMessage(inst._hHistoryTimelineList, LB_ADDSTRING, 0,
		(LPARAM)_T("Time                    Source           Rect                         Show              Detail"));
	SendMessage(inst._hHistoryTimelineList, LB_ADDSTRING, 0,
		(LPARAM)_T("----------------------- ---------------- --------------------------- ----------------- ----------------------------------------------"));
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		const HistoryTimelineDisplayEntry& row = *it;
		TCHAR timeText[64];
		TCHAR rectText[64];
		TCHAR detailText[1024];
		TCHAR line[1600];
		FormatFileTimePreciseLocal(&row.recordedUtc, timeText, _countof(timeText));
		FormatHistoryRect(row.hasPlacement ? &row.rect : NULL, rectText, _countof(rectText));
		if (row.isWindowEvent && !row.windowTitle.empty()) {
			StringCchPrintf(detailText, _countof(detailText), _T("%s | \"%s\""),
				row.detail.c_str(), row.windowTitle.c_str());
		}
		else {
			StringCchCopy(detailText, _countof(detailText), row.detail.c_str());
		}
		StringCchPrintf(line, _countof(line), _T("%-23s %-16s %-27s %-17s %s"),
			timeText,
			row.source.c_str(),
			row.hasPlacement ? rectText : _T("-"),
			row.hasPlacement ? TranslateShowCommand(row.showCmd) : _T("-"),
			detailText);
		SendMessage(inst._hHistoryTimelineList, LB_ADDSTRING, 0, (LPARAM)line);
	}

	SendMessage(inst._hHistoryTimelineList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(inst._hHistoryTimelineList, NULL, TRUE);
}

void RefreshWindowHistoryInspector()
{
	auto& inst = InstanceData::g_Instance;
	if (inst._hHistoryWindowList == NULL || inst._hHistorySummary == NULL || inst._hHistoryTimelineList == NULL) {
		return;
	}
	if (!IsHistoryViewVisible()) {
		return;
	}

	CaptureHistorySelection();
	CollectWindowHistoryKeys(inst._HistoryWindowKeys);
	SendMessage(inst._hHistoryWindowList, WM_SETREDRAW, FALSE, 0);
	SendMessage(inst._hHistoryWindowList, LB_RESETCONTENT, 0, 0);
	for (UINT_PTR hwndValue : inst._HistoryWindowKeys) {
		const WindowHistoryData& historyData = inst._WindowHistory[hwndValue];
		TCHAR lastSeen[64];
		TCHAR line[1024];
		FormatFileTimePreciseLocal(&historyData.lastRecordedUtc, lastSeen, _countof(lastSeen));
		StringCchPrintf(line, _countof(line),
			_T("0x%08IX pid=%6lu entries=%-4u last=%-23s %s"),
			historyData.hwndValue,
			historyData.processId,
			(UINT)historyData.entries.size(),
			lastSeen,
			historyData.latestTitle.empty() ? _T("<untitled>") : historyData.latestTitle.c_str());
		SendMessage(inst._hHistoryWindowList, LB_ADDSTRING, 0, (LPARAM)line);
	}

	int selectedIndex = LB_ERR;
	if (!inst._HistoryWindowKeys.empty()) {
		UINT_PTR preferredHwnd = inst._HistorySelectedHwnd != 0 ? inst._HistorySelectedHwnd : inst._HistoryWindowKeys.front();
		for (size_t index = 0; index < inst._HistoryWindowKeys.size(); ++index) {
			if (inst._HistoryWindowKeys[index] == preferredHwnd) {
				selectedIndex = (int)index;
				break;
			}
		}
		if (selectedIndex == LB_ERR) {
			selectedIndex = 0;
		}
		inst._HistorySelectedHwnd = inst._HistoryWindowKeys[(size_t)selectedIndex];
		SendMessage(inst._hHistoryWindowList, LB_SETCURSEL, selectedIndex, 0);
	}
	else {
		inst._HistorySelectedHwnd = 0;
	}

	SendMessage(inst._hHistoryWindowList, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(inst._hHistoryWindowList, NULL, TRUE);
	UpdateWindowHistoryDetails(inst._HistorySelectedHwnd);
}

static void SanitizeFileNameComponent(std::basic_string<TCHAR>& text)
{
	for (auto& ch : text) {
		if (ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
			ch = '_';
		}
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '.')) {
		text.pop_back();
	}
	if (text.empty()) {
		text = _T("window-history");
	}
}

void ExportSelectedWindowHistory(HWND hWndOwner)
{
	auto& inst = InstanceData::g_Instance;
	const WindowHistoryData* historyData = NULL;
	std::vector<HistoryTimelineDisplayEntry> rows;
	BuildWindowHistoryTimeline(inst._HistorySelectedHwnd, historyData, rows);
	if (historyData == NULL) {
		MessageBox(hWndOwner, _T("Select a tracked window in the History tab first."),
			_T("WinPosKeeper History Export"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	std::basic_string<TCHAR> baseName = historyData->latestTitle.empty() ? _T("window-history") : historyData->latestTitle;
	SanitizeFileNameComponent(baseName);
	if (baseName.size() > 48) {
		baseName.resize(48);
	}
	TCHAR defaultName[MAX_PATH];
	StringCchPrintf(defaultName, _countof(defaultName), _T("%s-0x%08IX.tsv"), baseName.c_str(), historyData->hwndValue);

	TCHAR filter[] = _T("Tab-separated values (*.tsv)\0*.tsv\0All Files (*.*)\0*.*\0\0");
	TCHAR path[MAX_PATH];
	StringCchCopy(path, _countof(path), defaultName);
	OPENFILENAME ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWndOwner;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = path;
	ofn.nMaxFile = _countof(path);
	ofn.lpstrDefExt = _T("tsv");
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	if (!GetSaveFileName(&ofn)) {
		return;
	}

	std::basic_string<TCHAR> text;
	text.append(_T("time\tkind\tsource\thwnd\tpid\tclass\tprocess\ttitle\tleft\ttop\twidth\theight\tshow\tdetail\r\n"));
	for (const auto& row : rows) {
		TCHAR timeText[64];
		TCHAR showText[32];
		TCHAR line[2048];
		FormatFileTimePreciseLocal(&row.recordedUtc, timeText, _countof(timeText));
		StringCchCopy(showText, _countof(showText), row.hasPlacement ? TranslateShowCommand(row.showCmd) : _T(""));
		StringCchPrintf(line, _countof(line),
			_T("%s\t%s\t%s\t0x%08IX\t%lu\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\t%s\r\n"),
			timeText,
			row.isWindowEvent ? _T("window") : _T("app-log"),
			row.source.c_str(),
			historyData->hwndValue,
			historyData->processId,
			historyData->windowClass.c_str(),
			historyData->processPath.c_str(),
			row.isWindowEvent ? row.windowTitle.c_str() : _T(""),
			row.hasPlacement ? row.rect.left : 0,
			row.hasPlacement ? row.rect.top : 0,
			row.hasPlacement ? row.rect.right - row.rect.left : 0,
			row.hasPlacement ? row.rect.bottom - row.rect.top : 0,
			showText,
			row.detail.c_str());
		text.append(line);
	}

	HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		LogWin32Error(_T("WARNING"), _T("CreateFile for history export"), GetLastError());
		return;
	}

	DWORD written = 0;
	WORD bom = 0xFEFF;
	WriteFile(hFile, &bom, sizeof(bom), &written, NULL);
	WriteFile(hFile, text.c_str(), (DWORD)(text.size() * sizeof(TCHAR)), &written, NULL);
	CloseHandle(hFile);
	LOG_EVENTF(_T("INFO"), _T("Exported history for HWND 0x%08IX to %s"), historyData->hwndValue, path);
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
		_T("Saved layouts: %d  |  Window records: %d  |  Placements for current layout: %d  |  Placements total: %d  |  Open tracked windows: %d\r\n")
		_T("Last full snapshot: %s  |  Last disk sync: %s  |  Data file: %d KB  |  Persist: %s  |  Restore on disconnect: %s  |  Retry: %d ms x %d  |  Window history: %s  |  Autostart: %s  |  Logging: %s"),
		configId, inst._ConfigHash,
		monitorSummary,
		(int)configs.size(), windowRecords, currentConfigPlacements, totalPlacements, trackedWindows,
		lastCapture, lastPersist,
		fileSize / 1024, inst.PersistPositions ? _T("ON") : _T("OFF"),
		inst.RestoreOnDisconnect ? _T("ON") : _T("OFF"),
		inst._RestoreRetryDelayMs, inst._RestoreRetryLimit,
		inst._HistoryTrackingEnabled ? _T("ON") : _T("OFF"),
		IsAutostartEnabled() ? _T("ON") : _T("OFF"),
		inst.LoggingEnabled ? _T("ON") : _T("OFF"));

	SetWindowText(hStatus, text);
	RefreshPlacementInspector();
}