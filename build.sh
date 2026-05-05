#!/usr/bin/env bash
set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if command -v cygpath >/dev/null 2>&1; then
    script_dir_win="$(cygpath -aw "$script_dir")"
else
    script_dir_win="$(cd "$script_dir" && pwd -W)"
fi

BUILD_SCRIPT_DIR_WIN="$script_dir_win" \
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command '& { Set-Location -LiteralPath $env:BUILD_SCRIPT_DIR_WIN; & ".\build.bat"; exit $LASTEXITCODE }'
exit $?