# WinPosKeeper

Puts your windows back where they were when a monitor comes back.

Windows 10 re-packs all open windows onto the remaining screens whenever a display disappears, and it does not move them back when the display returns. This can happen very frequently with **DisplayPort** / **USB-C** connected monitors: many of them drop their hot-plug signal when they go into standby, are switched off, or switch to another input. Windows treats that exactly like the cable being pulled and rearranges the whole desktop.

WinPosKeeper runs in the system tray, remembers window positions separately for each monitor arrangement, and restores them automatically when that arrangement is back.

It is aimed at setups where monitors come and go often: monitors that sleep or get switched off, docking and undocking, KVM switches, and multi-monitor desks with DisplayPort connections.

## Platform

- Built and tested on **Windows 10** (22H2) only.
- **Windows 11** has a comparable built-in option (Settings > System > Display > Multiple displays > "Remember window locations based on monitor connection"). WinPosKeeper has not been tested there and may not be needed.
- Single portable `.exe`, no installer, no runtime dependencies, no network access.

## Getting started

1. Download `WinPosKeeper-x64.exe` from the [Releases page](https://github.com/ChristianKaeser/WinPosKeeper/releases) (`WinPosKeeper-x86.exe` is for 32-bit Windows). Rename it to `WinPosKeeper.exe` if you like.
2. Put it somewhere permanent, for example `%LOCALAPPDATA%\Programs\WinPosKeeper\`, and run it.
3. The executable is not code-signed, so Windows SmartScreen may warn on first launch ("More info" > "Run anyway").
4. Right-click the tray icon and enable **Start with Windows** if you want it running all the time.

That's it. Arrange your windows, and the next time a monitor drops out and comes back, WinPosKeeper moves them back.

Starting the exe again while it is already running just opens the existing window.

## How it behaves

- **Saving:** about one second after windows stop moving, WinPosKeeper records the position, size and show state (normal, minimized, maximized) of every visible top-level application window under the current monitor layout.
- **Layouts:** a layout is identified by the position and resolution of every connected monitor. Different arrangements of the same monitors, or a changed resolution, count as different layouts. Each window keeps its placements for up to 16 layouts; the least recently seen layouts are dropped first.
- **Restoring:** after a display change settles (2 seconds), WinPosKeeper applies the saved placements for the new layout, then re-checks and re-applies them a few times, because some applications move themselves again after being restored. A window you move by hand during this phase is left alone.
- **Locked screen:** display changes that happen while the session is locked are handled after you unlock.
- **Monitor disconnects:** by default positions are also restored when a monitor goes away (so windows go back to where you had them on the smaller layout). Turn off "Also restore positions when monitors disconnect" to let Windows' own re-packing stand in that case.

## Tray menu

| Menu item | Description |
|-----------|-------------|
| Show Window... | Open the main window (clicking the tray icon does the same) |
| About | Version information |
| Also restore positions when monitors disconnect | Restore saved placements when the monitor count drops (default: on) |
| Start with Windows | Start WinPosKeeper at logon (default: off) |
| Persist Positions to Disk | Keep data on disk so a restart of WinPosKeeper itself doesn't lose it (default: off, see below) |
| Enable Event Logging | Show events in the Log tab (default: on) |
| Exit | Quit WinPosKeeper |

## Main window

- **Log:** what WinPosKeeper did and why: display changes, restores, verification passes and warnings. Right-click to copy or clear.
- **Layouts:** every monitor layout seen so far, and for the selected layout the saved placement of each window. `open` rows are windows that currently exist; `saved` rows are windows that are not visible right now (hidden, or closed since the last capture).
- **README:** this file.
- **Settings:** the tray options, plus:
  - *Verification pass delay* (100-30000 ms, default 2000): the pause before each check that restored windows are where they should be.
  - *Additional full verification passes* (0-10, default 4): how many times mismatched windows are re-applied.
  - *Window history* (default: off): keeps an in-memory timeline of every position change per window, for diagnosing applications that fight the restore. Shown in the History tab.
- **History:** the per-window timeline, merged with the log events that concern that window. **Export** writes it as a TSV file.

## Where data is stored

- Settings: registry, `HKEY_CURRENT_USER\Software\WinPosKeeper`
- Autostart: registry value `WinPosKeeper` under `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`
- Saved positions: in memory; with "Persist Positions to Disk" also in `%APPDATA%\WinPosKeeper\positions.dat` (written every 5 minutes and on exit)
- Startup problems: `%TEMP%\WinPosKeeper-startup-errors.log`, only written if the app fails to start

Nothing is sent anywhere. `positions.dat` holds window handles, process IDs, window classes, rectangles and monitor names, but no window titles. The Log and History tabs, clipboard copies and History exports do contain window titles and executable paths, so check them before sharing.

## Uninstalling

1. Turn off **Start with Windows** (or delete the `WinPosKeeper` value under the Run key above) and exit the app.
2. Delete the `.exe`.
3. Optionally delete `HKEY_CURRENT_USER\Software\WinPosKeeper` and `%APPDATA%\WinPosKeeper`.

## Limitations

- Windows are identified by their live window handle. That is exact, but it means positions only apply to windows that stay open: a window that is closed and reopened starts fresh, and nothing carries over a reboot.
- "Persist Positions to Disk" is intentionally conservative. It only restores data written during the same Windows boot and logon session, and only for windows that still exist with the same process and window class. It covers WinPosKeeper itself restarting (update, crash), not a reboot.
- Only visible top-level application windows are tracked; tool windows and popups are ignored.
- Windows of elevated (administrator) applications cannot be moved unless WinPosKeeper also runs elevated.
- Some applications reposition themselves after being restored. WinPosKeeper retries, but can lose that fight.
- Two monitor setups with exactly the same geometry are treated as the same layout.

## Building

Requirements: Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload and a Windows 10/11 SDK.

- `build.bat` builds Release for Win32 and x64; `build.bat x64` or `build.bat Win32` builds one. `build.sh` does the same from Git Bash.
- Output: `Release\x64\WinPosKeeper.exe` and `Release\Win32\WinPosKeeper.exe`. Full MSBuild output goes to `build_log.txt`.
- Or open `WinPosKeeper.vcxproj` in Visual Studio.

Release builds are made by the GitHub Actions workflow in `.github/workflows/build.yml`: every push builds both platforms, and pushing a `v*` tag that matches the version resource publishes a release with both executables, `SHA256SUMS.txt` and a build provenance attestation.

The executables link the C runtime statically, so they run without the Visual C++ Redistributable. The README is embedded as a resource, so the `.exe` is all you need to ship.

## Persistence file format

For the curious; the format may change between versions, and files from other versions are ignored.

`positions.dat` starts with a format magic, a boot-time marker and the Windows session ID, followed by two tables:

1. Layout snapshots: layout hash, time of the last full capture, number of windows captured, and the monitor list (rectangles, device names, friendly names).
2. Window records: window handle, process ID, window class, and the saved `WINDOWPLACEMENT` per layout hash.

A record is only loaded if the file belongs to the current boot and session, and the window handle still exists with the same process ID and window class.

## Credits and license

WinPosKeeper is a continuation of [MonitorKeeper](https://github.com/hunkydoryrepair/MonitorKeeper) by Garr Godfrey, reworked and extended in 2026 by Christian Käser.

MIT License, see [LICENSE](LICENSE).
