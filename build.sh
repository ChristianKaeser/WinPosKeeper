#!/usr/bin/env bash
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if command -v cygpath >/dev/null 2>&1; then
    script_dir_win="$(cygpath -aw "$script_dir")"
else
    script_dir_win="$(cd "$script_dir" && pwd -W)"
fi

# Optional first argument: Win32 or x64 (default: both)
BUILD_SCRIPT_DIR_WIN="$script_dir_win" BUILD_PLATFORM="${1:-}" \
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command '& { Set-Location -LiteralPath $env:BUILD_SCRIPT_DIR_WIN; & ".\build.bat" $env:BUILD_PLATFORM; exit $LASTEXITCODE }'
exit $?