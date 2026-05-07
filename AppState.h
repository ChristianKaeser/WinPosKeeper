#pragma once

#include "MonitorKeeper.h"
#include "MonitorConfig.h"

#define MAX_LOADSTRING 100
#define MAX_CONFIGSLOTS 16
#define MAX_LOG_ENTRIES 500
#define STATUS_HEIGHT 136
#define STATUS_ICON_SIZE 128
#define CAPTURE_TIMER_ID 2
#define PERSIST_TIMER_ID 3
#define PERSIST_TIMER_MS (5 * 60 * 1000)
#define DISPLAY_SETTLE_TIMER_ID 99
#define DISPLAY_SETTLE_MS 2000
#define VERIFY_TIMER_ID 4
#define HISTORY_FLUSH_TIMER_ID 5
#define DEFAULT_RESTORE_RETRY_DELAY_SECONDS 2
#define MIN_RESTORE_RETRY_DELAY_SECONDS 1
#define MAX_RESTORE_RETRY_DELAY_SECONDS 30
#define DEFAULT_RESTORE_RETRY_LIMIT 4
#define MIN_RESTORE_RETRY_LIMIT 0
#define MAX_RESTORE_RETRY_LIMIT 10
#define HISTORY_RATE_LIMIT_WINDOW_MS 1000
#define MAX_HISTORY_ENTRIES_PER_WINDOW 1000
#define MAX_HISTORY_LOG_EVENTS 4000
#define PLACEMENT_TOLERANCE 20

extern HINSTANCE hInst;
extern WCHAR szTitle[MAX_LOADSTRING];
extern WCHAR szWindowClass[MAX_LOADSTRING];
extern UINT WM_TASKBARCREATED;

struct ConfigSnapshotInfo {
	FILETIME lastSavedUtc;
	DWORD windowCount;
	std::vector<MonitorInfo> monitorLayout;

	ConfigSnapshotInfo();
};

struct WindowHistoryEntry {
	FILETIME recordedUtc;
	RECT rect;
	int showCmd;
	BOOL hasPlacement;
	ULONGLONG sequence;
	std::basic_string<TCHAR> source;
	std::basic_string<TCHAR> detail;
	std::basic_string<TCHAR> windowTitle;

	WindowHistoryEntry();
};

struct HistoryLogEntry {
	FILETIME recordedUtc;
	ULONGLONG sequence;
	std::basic_string<TCHAR> type;
	std::basic_string<TCHAR> detail;

	HistoryLogEntry();
};

struct WindowHistoryData {
	UINT_PTR hwndValue;
	DWORD processId;
	FILETIME lastRecordedUtc;
	WINDOWPLACEMENT lastPlacement;
	BOOL hasLastPlacement;
	ULONGLONG rateLimitUntilTick;
	ULONGLONG pendingSelfActionUntilTick;
	WINDOWPLACEMENT pendingPlacement;
	FILETIME pendingRecordedUtc;
	BOOL hasPendingPlacement;
	std::basic_string<TCHAR> windowClass;
	std::basic_string<TCHAR> processPath;
	std::basic_string<TCHAR> latestTitle;
	std::basic_string<TCHAR> pendingSource;
	std::basic_string<TCHAR> pendingDetail;
	std::basic_string<TCHAR> pendingWindowTitle;
	std::vector<WindowHistoryEntry> entries;

	WindowHistoryData();
};

class SavedWindowData {
public:
	SavedWindowData();

	int m_nUnusedCount;
	DWORD m_lastRestoreError;
	BOOL m_retryPending;
	std::map<UINT64, WINDOWPLACEMENT> m_placements;
	HWND m_hwnd;
	DWORD m_processId;
	TCHAR m_wndClass[40];
	TCHAR m_processPath[MAX_PATH];
	TCHAR m_windowTitle[256];

	BOOL SetData(HWND hwnd, UINT64 configHash);
	BOOL HasPlacement(UINT64 configHash) const;
	BOOL RestoreWindow(UINT64 configHash);
};

class InstanceData {
public:
	InstanceData();
	~InstanceData();

	void Shutdown();
	void TagWindowsUnused();
	int CountTrackedWindows() const;
	int CountSavedWindowRecords() const;
	int CountTotalPlacements() const;
	int CountPlacementsForConfig(UINT64 configHash) const;
	int GetOrCreateConfigId(UINT64 configHash);
	BOOL TryGetSnapshotInfo(UINT64 configHash, ConfigSnapshotInfo& info) const;
	int RestoreWindowPositions(UINT64 configHash);
	SavedWindowData* FindWindowSlot(HWND hwnd, DWORD processId, LPCTSTR wndClass);

	static BOOL GetPersistPath(TCHAR* path, DWORD cch);
	BOOL SaveToDisk(LPCTSTR reason);
	void LoadFromDisk();

	static InstanceData g_Instance;

	HWINEVENTHOOK _Hook;
	std::map<UINT64, int> _ConfigIds;
	int _NextConfigId;
	std::map<UINT64, ConfigSnapshotInfo> _ConfigSnapshots;
	std::vector<SavedWindowData> _WindowData;
	UINT64 _ConfigHash;
	int _NumMonitors;
	HWND _MainWnd;
	HWND _hMainTab;
	HWND _hStatus;
	HWND _hStatusIcon;
	HWND _hLogList;
	HWND _hConfigList;
	HWND _hConfigSummary;
	HWND _hPlacementList;
	HWND _hReadmeView;
	HWND _hSettingsIntro;
	HWND _hSettingsRestoreCheck;
	HWND _hSettingsAutostartCheck;
	HWND _hSettingsPersistCheck;
	HWND _hSettingsLoggingCheck;
	HWND _hSettingsHistoryCheck;
	HWND _hSettingsDelayLabel;
	HWND _hSettingsDelayEdit;
	HWND _hSettingsRetryLabel;
	HWND _hSettingsRetryEdit;
	HWND _hSettingsApplyButton;
	HWND _hHistoryWindowList;
	HWND _hHistorySummary;
	HWND _hHistoryTimelineList;
	HWND _hHistoryExportButton;
	HFONT _hLogFont;
	BOOL InChangingState;
	BOOL RestoreOnDisconnect;
	BOOL PersistPositions;
	BOOL LoggingEnabled;
	BOOL _HistoryTrackingEnabled;
	BOOL AlreadyRunning;
	int _RestoreRetryDelaySeconds;
	int _RestoreRetryLimit;
	int _RestoreRetryCount;
	BOOL _AwaitingRestoreRetry;
	std::map<UINT_PTR, WindowHistoryData> _WindowHistory;
	std::vector<HistoryLogEntry> _HistoryLog;
	std::vector<UINT_PTR> _HistoryWindowKeys;
	UINT_PTR _HistorySelectedHwnd;
	ULONGLONG _HistoryNextSequence;
	HANDLE _MutexSingleInstance;
	UINT64 _InspectorSelectedConfigHash;
	std::vector<UINT64> _InspectorConfigHashes;
	std::basic_string<TCHAR> _ReadmeText;
	FILETIME _LastCaptureUtc;
	FILETIME _LastPersistUtc;
};