#pragma once
#ifdef _WIN32
#include "window_win32.h"
#elif defined(__APPLE__)
#include "window_cocoa.h"
#else
#include "window_x11.h"
#endif
