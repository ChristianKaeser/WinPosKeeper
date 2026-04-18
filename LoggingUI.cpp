#include "LoggingUI.h"

#include "AppState.h"

#include "MonitorConfig.h"
#include "Persistence.h"
#include "WindowTracking.h"

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
	if (selectedHash == inst._ConfigHash) {
		GetCurrentMonitorSummary(monitorSummary, _countof(monitorSummary));
	}
	else if (hasSnapshot && !snapshotInfo.monitorLayout.empty()) {
		FormatMonitorSummary(snapshotInfo.monitorLayout, monitorSummary, _countof(monitorSummary));
	}
	else {
		StringCchCopy(monitorSummary, _countof(monitorSummary),
			_T("Not recorded for this historical layout yet. It will appear after that layout becomes active again."));
	}

	TCHAR summary[1400];
	StringCchPrintf(summary, _countof(summary),
		_T("Config ID/Hash:          #%-3d   0x%016I64X   %s\r\n")
		_T("Stored/Snapshot Windows: %d / %lu\r\n")
		_T("Last Saved:              %s\r\n")
		_T("Desktop Layout:          %s"),
		inst.GetOrCreateConfigId(selectedHash),
		selectedHash,
		selectedHash == inst._ConfigHash ? _T("<current layout>") : _T("<not current layout>"),
		inst.CountPlacementsForConfig(selectedHash),
		hasSnapshot ? snapshotInfo.windowCount : 0,
		lastSaved,
		monitorSummary);
	SetWindowText(inst._hConfigSummary, summary);
	
	SendMessage(inst._hPlacementList, LB_ADDSTRING, 0,
		(LPARAM)_T("Status     X   Y     Width Height State          Process / Window Title"));
	SendMessage(inst._hPlacementList, LB_ADDSTRING, 0,
		(LPARAM)_T("------  ----- -----  ----- -----  -------------  --------------------------------"));

	for (const auto& wd : inst._WindowData) {
		auto it = wd.m_placements.find(selectedHash);
		if (it == wd.m_placements.end()) {
			continue;
		}

		const WINDOWPLACEMENT& place = it->second;
		TCHAR identity[512];
		TCHAR line[1400];
		FormatWindowIdentity(wd.m_hwnd, wd.m_wndClass, identity, _countof(identity));
		StringCchPrintf(line, _countof(line),
			_T("%-6s %5d |%5d %5d x%5d  %-13s  %s"),
			(wd.m_hwnd != NULL && IsWindow(wd.m_hwnd) && wd.m_nUnusedCount <= 2) ? _T("live") : _T("stale"),
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
		TCHAR line[256];
		TCHAR configId[16];
		ConfigSnapshotInfo snapshotInfo;
		BOOL hasSnapshot = inst.TryGetSnapshotInfo(configHash, snapshotInfo);
		TCHAR lastSaved[32];
		if (hasSnapshot) {
			FormatFileTimeLocal(&snapshotInfo.lastSavedUtc, lastSaved, _countof(lastSaved));
		}
		else {
			StringCchCopy(lastSaved, _countof(lastSaved), _T("unknown"));
		}
		StringCchPrintf(configId, _countof(configId), _T("#%d"), inst.GetOrCreateConfigId(configHash));

		StringCchPrintf(line, _countof(line),
			_T("%-3s  %016I64X %4d  %-8s  %s"),
			configId,
			configHash,
			inst.CountPlacementsForConfig(configHash),
			lastSaved,
			configHash == inst._ConfigHash ? _T("current") : _T(""));
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
	RefreshPlacementInspector();
}