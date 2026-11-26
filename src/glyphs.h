#pragma once
#ifdef _WIN32
#include "glyphs_directwrite.h"
#elif defined(__APPLE__)
#include "glyphs_coretext.h"
#else
#include "glyphs_freetype.h"
#endif
