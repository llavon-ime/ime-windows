#pragma once

#include <windows.h>

// WinUser's legacy alias collides with WinRT animation GetCurrentTime methods.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
