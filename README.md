# WinPosKeeper

Automatically restores window positions when your monitor or desktop configuration changes.

WinPosKeeper is a continuation of the original MonitorKeeper project by Garr Godfrey.

Original Author: Garr Godfrey

2026 Modifications: Christian Kaser

License: MIT

## What it does

When a monitor is disconnected, powered off, or switched away, Windows usually repacks top-level windows onto the remaining desktop area. When that layout later comes back, WinPosKeeper re-applies the saved positions for that specific monitor arrangement.

The app does not only look at the monitor count. It hashes the current monitor layout from the device names and monitor rectangles, so different two-monitor arrangements are treated as different saved layouts.

## Core concepts

The app uses a few terms repeatedly in the UI and log.

Saved layout: a monitor configuration identified by a 64-bit hash. Each saved layout can have its own saved window placements.

Window record: one tracked window identity in memory. While the app is running, the strong key is the live `HWND` plus the owning process ID and window class. The stored process path and title are kept for diagnostics only. Each window record can carry up to `MAX_CONFIGSLOTS` saved placements across different layouts.

Saved placement: one `WINDOWPLACEMENT` value for one window record under one saved layout. This is the actual rectangle and show-state that can be restored later.

Full snapshot: a capture pass taken while a saved layout is active. A snapshot stores when that layout was last seen, how many visible top-level windows were captured during that pass, and the monitor arrangement itself.

Open tracked window: a window record that is currently matched to a live top-level window handle in the running session. In the Layouts tab, `open` means the app currently sees that window; `saved` means only the stored record remains.

## Features

- Automatic save and restore of window positions per saved layout
- Optional same-session restart recovery in `%APPDATA%\WinPosKeeper\positions.dat`
- Optional autostart via the tray menu
- Optional restore when monitors disconnect
- Built-in diagnostics through the Log, Layouts, and README tabs
- Per-monitor DPI aware manifest
- Tray icon recovery after Explorer restarts

## Using the app

WinPosKeeper runs in the system tray. Right-click the tray icon for options:

| Menu item | Description |
|-----------|-------------|
| About | Version information |
| Show Window | Open the main status window |
| Also restore positions when monitors disconnect | Re-apply saved placements when a monitor disappears |
| Start with Windows | Toggle autostart at logon |
| Persist Positions to Disk | Save and reload placements if WinPosKeeper itself restarts during the same Windows session |
| Enable Event Logging | Toggle the on-screen log |
| Exit | Quit the app |

The Layouts tab is intended as a diagnostic view.

The selector at the top shows each saved layout with these fields:

- Layout ID: the short sequential number shown in the UI.
- `current` or `saved`: whether that saved layout matches the desktop right now.
- `placements=`: how many saved placements currently exist for that layout.
- `snapshot=`: how many visible top-level windows were captured during the last full snapshot while that layout was active.
- `last=`: when that layout was last fully captured.
- Monitor summary: the saved monitor arrangement for that layout.

The detail pane below explains the currently selected saved layout:

- Saved placements: current count of stored `WINDOWPLACEMENT` records for that layout.
- Last full snapshot: how many windows were actually seen during the last full capture pass for that layout.
- Snapshot time: when that full capture happened.
- Monitor layout: the saved monitor arrangement for that layout. Older persisted files may not have this data until that layout is seen again.

The placement list uses these row states:

- `open`: the record is currently attached to a live window handle in this session.
- `saved`: the record still has saved placements, but the app does not currently see a matching live window.

The README tab shows this file from an embedded resource so deployment stays a single standalone `.exe`.

## Persistence format

When disk persistence is enabled, the app writes `%APPDATA%\WinPosKeeper\positions.dat`.

The current file format is a simple binary format with a version magic followed by two tables:

1. Saved layout snapshots.
2. Window records and their saved placements.

Each saved layout snapshot stores:

- Layout hash
- Last snapshot time
- Last full-snapshot window count
- Saved monitor layout list, including monitor rectangles, device names, and friendly names

Each persisted window record stores:

- A boot/session marker in the file header to prove the file belongs to the current Windows session
- The live `HWND` value
- The owning process ID
- The window class
- A list of saved placements keyed by layout hash

Each saved placement stores the raw Win32 `WINDOWPLACEMENT` structure for one window record under one saved layout.

Persisted records are only accepted if all of these still match when the app starts again:

- The file was written during the current Windows boot and logon session.
- The saved `HWND` still exists.
- The saved process ID still owns that `HWND`.
- The saved window class still matches.

If any of those checks fail, the persisted record is ignored rather than guessed back onto a different window.

## Persistence limitations

- Disk persistence is intentionally conservative. It is meant to recover from WinPosKeeper itself restarting during the same Windows session, not to carry exact window identity safely across a reboot or after target applications have been closed and restarted.
- `HWND` values are only stable while the target window continues to exist. If a target application recreates its window or restarts, the persisted record will be discarded.
- Older persisted files from earlier heuristic formats are intentionally ignored because they cannot be matched back strongly enough.
- Only the newest `MAX_CONFIGSLOTS` layouts are kept per window record. Very old layouts are pruned per window record, not globally.

## Runtime limitations

- Only visible top-level windows that look like normal application windows are captured.
- Elevated windows cannot usually be moved by a non-elevated instance of the app.
- Some applications overwrite their own window position after the app restores them, which is why the app verifies and retries restores.
- A saved layout can legitimately show fewer saved placements than the current number of open tracked windows if some windows were opened after that layout was last active.

## Building

Run `build.bat` from a command prompt. It uses `vswhere` to find Visual Studio, writes the full output to `build_log.txt`, and now also prints the final MSBuild exit code directly in the console.

Requirements: Visual Studio 2022 with the Desktop development with C++ workload.

Output: `Release\Win32\WinPosKeeper.exe`

## Quick test

1. Connect two monitors and place some windows across both.
2. Start WinPosKeeper.
3. Change the layout so Windows moves everything onto one display.
4. Restore the original monitor layout.
5. Confirm the windows return to their saved positions.
