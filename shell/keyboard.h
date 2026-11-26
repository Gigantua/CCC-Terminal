#pragma once
#include <cstdint>
#include <string>
#include "../src/keys.h"

namespace keyboard {

struct KeyInput {
    uint16_t vk = 0, scan = 0;
    char16_t ch = 0;
    uint32_t state = 0;
    char32_t base = 0, shifted = 0;
    bool down = true, repeat = false, super = false;
};

inline std::u16string number(int n) {
    std::string s = std::to_string(n);
    return std::u16string(s.begin(), s.end());
}

inline char32_t usLayoutChar(unsigned scan) {
    static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`", row4[] = "zxcvbnm,./";
    if (scan >= 0x02 && scan <= 0x0D) return (char32_t)row1[scan - 0x02];
    if (scan >= 0x10 && scan <= 0x1B) return (char32_t)row2[scan - 0x10];
    if (scan >= 0x1E && scan <= 0x29) return (char32_t)row3[scan - 0x1E];
    if (scan == 0x2B) return '\\';
    if (scan >= 0x2C && scan <= 0x35) return (char32_t)row4[scan - 0x2C];
    if (scan == 0x39) return ' ';
    return 0;
}

inline int kittyModifierKey(const KeyInput& k) {
    bool right = (k.state & ENHANCED_KEY) != 0;
    switch (k.vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return k.vk == VK_RSHIFT || k.scan == 0x36 ? 57447 : 57441;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return k.vk == VK_RCONTROL || right ? 57448 : 57442;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return k.vk == VK_RMENU || right ? 57449 : 57443;
    case VK_LWIN: return 57444;
    case VK_RWIN: return 57450;
    case VK_CAPITAL: return 57358;
    case VK_SCROLL: return 57359;
    case VK_NUMLOCK: return 57360;
    }
    return 0;
}

inline bool kittyFunctionKey(const KeyInput& k, bool all, int& code, char16_t& fin) {
    struct Key { int vk; int code; char16_t fin; int keypad; };
    static const Key keys[] = {
        { VK_ESCAPE, 27, 'u', 0 }, { VK_RETURN, 13, 'u', 57414 }, { VK_TAB, 9, 'u', 0 }, { VK_BACK, 127, 'u', 0 },
        { VK_INSERT, 2, '~', 57425 }, { VK_DELETE, 3, '~', 57426 }, { VK_LEFT, 1, 'D', 57417 }, { VK_RIGHT, 1, 'C', 57418 },
        { VK_UP, 1, 'A', 57419 }, { VK_DOWN, 1, 'B', 57420 }, { VK_PRIOR, 5, '~', 57421 }, { VK_NEXT, 6, '~', 57422 },
        { VK_HOME, 1, 'H', 57423 }, { VK_END, 1, 'F', 57424 }, { VK_CLEAR, 1, 'E', 57427 },
        { VK_F1, 1, 'P', 0 }, { VK_F2, 1, 'Q', 0 }, { VK_F3, 13, '~', 0 }, { VK_F4, 1, 'S', 0 }, { VK_F5, 15, '~', 0 },
        { VK_F6, 17, '~', 0 }, { VK_F7, 18, '~', 0 }, { VK_F8, 19, '~', 0 }, { VK_F9, 20, '~', 0 }, { VK_F10, 21, '~', 0 },
        { VK_F11, 23, '~', 0 }, { VK_F12, 24, '~', 0 }, { VK_SNAPSHOT, 57361, 'u', 0 }, { VK_PAUSE, 57362, 'u', 0 },
        { VK_APPS, 57363, 'u', 0 }, { VK_MEDIA_PLAY_PAUSE, 57430, 'u', 0 }, { VK_MEDIA_STOP, 57432, 'u', 0 },
        { VK_MEDIA_NEXT_TRACK, 57435, 'u', 0 }, { VK_MEDIA_PREV_TRACK, 57436, 'u', 0 }, { VK_VOLUME_DOWN, 57438, 'u', 0 },
        { VK_VOLUME_UP, 57439, 'u', 0 }, { VK_VOLUME_MUTE, 57440, 'u', 0 },
    };
    if (k.vk >= VK_F13 && k.vk <= VK_F24) { code = 57376 + (k.vk - VK_F13); fin = 'u'; return true; }
    bool enhanced = (k.state & ENHANCED_KEY) != 0;
    for (const Key& e : keys) {
        if (e.vk != k.vk) continue;
        bool keypad = e.keypad && (e.vk == VK_RETURN ? enhanced : !enhanced);
        if (all && keypad) { code = e.keypad; fin = 'u'; }
        else { code = e.code; fin = e.fin; }
        return true;
    }
    return false;
}

inline int kittyKeypadKey(int vk) {
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 57399 + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_DECIMAL: return 57409;
    case VK_DIVIDE: return 57410;
    case VK_MULTIPLY: return 57411;
    case VK_SUBTRACT: return 57412;
    case VK_ADD: return 57413;
    case VK_SEPARATOR: return 57416;
    }
    return 0;
}

inline std::u16string kittyKey(const KeyInput& k, int flags) {
    bool all = (flags & 8) != 0, events = (flags & 2) != 0, alternates = (flags & 4) != 0, withText = all && (flags & 16);
    if (!k.down && !events) return u"";
    int event = !k.down ? 3 : k.repeat && events ? 2 : 1;
    uint32_t st = k.state;
    bool altGr = (st & RIGHT_ALT_PRESSED) && (st & LEFT_CTRL_PRESSED);
    int bits = ((st & SHIFT_PRESSED) ? 1 : 0) | (!altGr && (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) ? 2 : 0) |
               (!altGr && (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) ? 4 : 0) | (k.super ? 8 : 0);
    if (all) bits |= ((st & CAPSLOCK_ON) ? 64 : 0) | ((st & NUMLOCK_ON) ? 128 : 0);
    bool commandMods = (bits & 14) != 0;
    bool printable = k.ch >= 0x20 && k.ch != 0x7F && !(k.ch >= 0xD800 && k.ch < 0xE000);
    std::u16string alts, text;
    if (withText && event != 3 && printable) text = number((int)k.ch);
    auto seq = [&](int code, char16_t fin) {
        bool needMods = bits || event != 1;
        bool letter = fin != u'u' && fin != u'~';
        std::u16string s = u"\x1b[";
        if (!letter || needMods) s += number(code) + alts;
        if (needMods || !text.empty()) {
            s += u';';
            if (needMods) {
                s += number(bits + 1);
                if (event != 1) s += u":" + number(event);
            }
        }
        if (!text.empty()) s += u";" + text;
        return s + fin;
    };
    auto plain = [&](const std::u16string& legacy) { return event == 3 ? std::u16string() : legacy; };
    if (int mod = kittyModifierKey(k)) return all ? seq(mod, 'u') : u"";
    int code;
    char16_t fin;
    if (kittyFunctionKey(k, all, code, fin)) {
        if (!all && !(bits & 15)) {
            if (k.vk == VK_RETURN) return plain(u"\r");
            if (k.vk == VK_TAB) return plain(u"\t");
            if (k.vk == VK_BACK) return plain(u"\x7f");
        }
        text.clear();
        return seq(code, fin);
    }
    if (int kp = kittyKeypadKey(k.vk)) {
        if (!all && !commandMods) return plain(printable ? std::u16string(1, k.ch) : u"");
        return seq(kp, 'u');
    }
    int base = k.vk ? (int)k.base : 0;
    if (!base || (!all && !commandMods)) return plain(k.ch && (printable || !base) ? std::u16string(1, k.ch) : u"");
    if (alternates) {
        int shifted = (bits & 1) ? (int)k.shifted : 0;
        int us = (int)usLayoutChar(k.scan);
        if (shifted && shifted != base) alts += u":" + number(shifted);
        if (us && us != base) alts += (alts.empty() ? u"::" : u":") + number(us);
    }
    return seq(base, 'u');
}

inline std::u16string legacyKey(const KeyInput& k, bool appCursor, int modifyOtherKeys) {
    bool shift = (k.state & SHIFT_PRESSED) != 0, ctrl = (k.state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0,
         alt = (k.state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    int mods = (shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0);
    std::u16string m = number(mods + 1);
    auto cursorKey = [&](char16_t c) { return mods ? u"\x1b[1;" + m + c : std::u16string(appCursor ? u"\x1bO" : u"\x1b[") + c; };
    auto tilde = [&](int n) { return u"\x1b[" + number(n) + (mods ? u";" + m : u"") + u"~"; };
    auto ss3 = [&](char16_t c) { return mods ? u"\x1b[1;" + m + c : std::u16string(u"\x1bO") + c; };
    switch (k.vk) {
    case VK_UP: return cursorKey('A');
    case VK_DOWN: return cursorKey('B');
    case VK_RIGHT: return cursorKey('C');
    case VK_LEFT: return cursorKey('D');
    case VK_HOME: return cursorKey('H');
    case VK_END: return cursorKey('F');
    case VK_INSERT: return tilde(2);
    case VK_DELETE: return tilde(3);
    case VK_PRIOR: return tilde(5);
    case VK_NEXT: return tilde(6);
    case VK_F1: return ss3('P');
    case VK_F2: return ss3('Q');
    case VK_F3: return ss3('R');
    case VK_F4: return ss3('S');
    case VK_F5: return tilde(15);
    case VK_F6: return tilde(17);
    case VK_F7: return tilde(18);
    case VK_F8: return tilde(19);
    case VK_F9: return tilde(20);
    case VK_F10: return tilde(21);
    case VK_F11: return tilde(23);
    case VK_F12: return tilde(24);
    case VK_BACK: return std::u16string(alt ? u"\x1b" : u"") + (ctrl ? u"\x08" : u"\x7f");
    case VK_TAB: return shift ? u"\x1b[Z" : u"\t";
    case VK_RETURN:
        if ((shift || ctrl) && modifyOtherKeys) return u"\x1b[27;" + m + u";13~";
        return alt ? u"\x1b\r" : u"\r";
    case VK_ESCAPE: return u"\x1b";
    case VK_SPACE: if (ctrl) return std::u16string(1, u'\0'); break;
    }
    if (!k.ch) return u"";
    bool altGr = ctrl && alt;
    return (alt && !altGr ? u"\x1b" : u"") + std::u16string(1, k.ch);
}

}
