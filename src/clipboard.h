#pragma once
#include <cstring>
#include <string>
#include "../shell/util.h"

namespace clipboard {

inline void set(HWND owner, const std::string& text) {
    std::wstring w = ccc::W(text);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(mem), w.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

inline std::string get(HWND owner) {
    std::string out;
    if (!OpenClipboard(owner)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) { out = ccc::U(p); GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}

}
