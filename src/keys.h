#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#ifdef _WIN32
#include "../shell/os.h"

inline char32_t layoutChar(unsigned vk, unsigned scan, bool shift) {
    BYTE keys[256] = {};
    if (shift) keys[VK_SHIFT] = 0x80;
    wchar_t buf[8];
    int n = ToUnicodeEx(vk, scan, keys, buf, 8, 4, GetKeyboardLayout(0));
    return n != 0 && buf[0] >= 0x20 && buf[0] != 0x7F ? (char32_t)buf[0] : 0;
}

inline bool producesChar(int vk) { return MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) != 0; }

inline int vkForChar(uint32_t cp, bool& shift) {
    SHORT r = VkKeyScanW((wchar_t)cp);
    shift = (r & 0x100) != 0;
    return r & 0xFF;
}

inline uint32_t charForVk(int vk) { return MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7FFF; }
inline int scanForVk(int vk) { return (int)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC); }
#else
enum : int {
    VK_CANCEL = 0x03, VK_BACK = 0x08, VK_TAB = 0x09, VK_CLEAR = 0x0C, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D, VK_NUMPAD0 = 0x60, VK_NUMPAD9 = 0x69, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B,
    VK_SEPARATOR = 0x6C, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2 = 0x71, VK_F3 = 0x72, VK_F4 = 0x73,
    VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77, VK_F9 = 0x78, VK_F10 = 0x79, VK_F11 = 0x7A, VK_F12 = 0x7B, VK_F13 = 0x7C,
    VK_F24 = 0x87, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_VOLUME_MUTE = 0xAD, VK_VOLUME_DOWN = 0xAE, VK_VOLUME_UP = 0xAF, VK_MEDIA_NEXT_TRACK = 0xB0,
    VK_MEDIA_PREV_TRACK = 0xB1, VK_MEDIA_STOP = 0xB2, VK_MEDIA_PLAY_PAUSE = 0xB3, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC,
    VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};
enum : unsigned {
    RIGHT_ALT_PRESSED = 0x1, LEFT_ALT_PRESSED = 0x2, RIGHT_CTRL_PRESSED = 0x4, LEFT_CTRL_PRESSED = 0x8, SHIFT_PRESSED = 0x10,
    NUMLOCK_ON = 0x20, SCROLLLOCK_ON = 0x40, CAPSLOCK_ON = 0x80, ENHANCED_KEY = 0x100,
};
enum : int { WHEEL_DELTA = 120 };
enum : unsigned { CTRL_C_EVENT = 0, CTRL_BREAK_EVENT = 1, CTRL_CLOSE_EVENT = 2 };

struct UsKey { int scan; char plain, shifted; int vk; };

inline const std::vector<UsKey>& usKeys() {
    static const std::vector<UsKey> keys = [] {
        static const struct { int scan; const char* plain; const char* shifted; } rows[] = {
            { 0x02, "1234567890-=", "!@#$%^&*()_+" }, { 0x10, "qwertyuiop[]", "QWERTYUIOP{}" }, { 0x1E, "asdfghjkl;'`", "ASDFGHJKL:\"~" },
            { 0x2B, "\\", "|" }, { 0x2C, "zxcvbnm,./", "ZXCVBNM<>?" }, { 0x39, " ", " " },
        };
        static const std::pair<char, int> symbols[] = { { '-', VK_OEM_MINUS }, { '=', VK_OEM_PLUS }, { '[', VK_OEM_4 }, { ']', VK_OEM_6 }, { ';', VK_OEM_1 },
                                                        { '\'', VK_OEM_7 }, { '`', VK_OEM_3 }, { '\\', VK_OEM_5 }, { ',', VK_OEM_COMMA },
                                                        { '.', VK_OEM_PERIOD }, { '/', VK_OEM_2 }, { ' ', VK_SPACE } };
        std::vector<UsKey> out;
        for (auto& r : rows)
            for (int i = 0; r.plain[i]; ++i) {
                char c = r.plain[i];
                int vk = c >= 'a' && c <= 'z' ? c - 32 : c >= '0' && c <= '9' ? c : 0;
                for (auto& s : symbols) if (s.first == c) vk = s.second;
                out.push_back({ r.scan + i, c, r.shifted[i], vk });
            }
        return out;
    }();
    return keys;
}

inline bool producesChar(int vk) {
    return std::any_of(usKeys().begin(), usKeys().end(), [&](const UsKey& k) { return k.vk == vk; }) || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE);
}

inline int vkForChar(uint32_t cp, bool& shift) {
    for (const UsKey& k : usKeys())
        if ((uint32_t)(unsigned char)k.plain == cp || (uint32_t)(unsigned char)k.shifted == cp) { shift = (uint32_t)(unsigned char)k.plain != cp; return k.vk; }
    shift = false;
    return 0;
}

inline uint32_t charForVk(int vk) {
    for (const UsKey& k : usKeys()) if (k.vk == vk) return (uint32_t)(k.plain >= 'a' && k.plain <= 'z' ? k.plain - 32 : k.plain);
    return 0;
}

inline int scanForVk(int vk) {
    for (const UsKey& k : usKeys()) if (k.vk == vk) return k.scan;
    return 0;
}

inline char32_t layoutChar(unsigned vk, unsigned, bool shift) {
    for (const UsKey& k : usKeys()) if (k.vk == (int)vk) return (char32_t)(unsigned char)(shift ? k.shifted : k.plain);
    return 0;
}
#endif
