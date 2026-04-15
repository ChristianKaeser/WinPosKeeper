# MonitorKeeper

Automatically restores window positions when your monitor configuration changes.

**Original Author:** Garr Godfrey

**2026 Modifications:** Christian Käser

**License:** MIT

## What it does

When a monitor is disconnected (power off, HDMI unplugged, display switch), Windows
moves all windows onto the remaining display(s). When the monitor comes back, MonitorKeeper
puts every window back where it was.

MonitorKeeper identifies each monitor layout by hashing device names and positions, so it
handles any configuration change — not just adding/removing monitors.

## Features

- **Automatic save & restore** of window positions per monitor configuration
- **Persistent storage** — optionally saves positions to disk so they survive reboots
  (`%APPDATA%\MonitorKeeper\positions.dat`)
- **Start with Windows** — optional autostart via the system tray menu
- **Restore on disconnect** — optionally re-apply saved positions after a monitor disappears
- **DPI-aware** — PerMonitorV2 manifest for correct behavior on mixed-DPI setups
- **Explorer restart resilient** — tray icon re-creates itself if Explorer crashes

## Usage

MonitorKeeper runs in the system tray. Right-click the tray icon for options:

| Menu item | Description |
|-----------|-------------|
| About | Version info |
| Show Window | Show the main status and event log window |
| Restore positions when monitors disconnect | When checked, MonitorKeeper re-applies saved positions after a monitor disconnects |
| Start with Windows | Toggle autostart at login |
| Persist Positions to Disk | Save/load positions across restarts |
| Exit | Quit MonitorKeeper |

## Building

Run `build.bat` from a command prompt. It uses `vswhere` to locate your Visual Studio
installation automatically. Requires Visual Studio 2022 with the C++ desktop workload.

Output: `Release\Win32\MonitorKeeper.exe`

## Quick test

1. Connect two monitors and arrange some windows across both.
2. Start MonitorKeeper.
3. In Display Settings, change "Multiple Displays" from "Extend" to "Duplicate".
4. Change it back to "Extend" — windows should return to their original positions.

## Limitations

- Cannot move windows owned by elevated (administrator) processes when running as a
  standard user. Use Task Scheduler to launch MonitorKeeper at login as administrator
  if needed.
- Windows are restored to the state (position, size, minimized/maximized) last seen for
  that monitor configuration.
