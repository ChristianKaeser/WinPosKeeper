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
#define VERIFY_TIMER_MS 2000
#define RESTORE_RETRY_LIMIT 4
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

class SavedWindowData {
public:
	SavedWindowData();

	int m_nUnusedCount;
	DWORD m_lastRestoreError;
	BOOL m_retryPending;
	std::map<UINT64, WINDOWPLACEMENT> m_placements;
	HWND m_hwnd;
	TCHAR m_wndClass[40];
	TCHAR m_processPath[MAX_PATH];
	TCHAR m_windowTitle[256];

	BOOL SetData(HWND hwnd, UINT64 configHash);
	BOOL HasPlacement(UINT64 configHash) const;
	int MatchIdentityScore(LPCTSTR wndClass, LPCTSTR processPath, LPCTSTR windowTitle) const;
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
	SavedWindowData* FindWindowSlot(HWND hwnd, LPCTSTR wndClass, LPCTSTR processPath, LPCTSTR windowTitle);

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
	HFONT _hLogFont;
	BOOL InChangingState;
	BOOL RestoreOnDisconnect;
	BOOL PersistPositions;
	BOOL LoggingEnabled;
	BOOL AlreadyRunning;
	int _RestoreRetryCount;
	BOOL _AwaitingRestoreRetry;
	HANDLE _MutexSingleInstance;
	UINT64 _InspectorSelectedConfigHash;
	std::vector<UINT64> _InspectorConfigHashes;
	std::basic_string<TCHAR> _ReadmeText;
	FILETIME _LastCaptureUtc;
	FILETIME _LastPersistUtc;
};