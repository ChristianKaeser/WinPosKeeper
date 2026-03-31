#pragma once

#include "resource.h"
#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>
#include <shlobj.h>
#include <cguid.h>

#include <tchar.h>
#include <strsafe.h>
#include <map>
#include <vector>
#include <algorithm>