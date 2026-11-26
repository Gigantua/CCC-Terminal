#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include "terminal.h"

inline void writeStdout(const std::string& s) { os::write(os::stdHandle(1), s.data(), s.size()); }

class Headless {
public:
    int run(const std::string& path) {
        std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())));
        if (!in) { print("cannot open " + path + "\n"); return 1; }
        term.metrics.cellW = term.metrics.cellH = 1;
        term.barWidth = 0;
        term.host.con.onChange = requestFrame;
        term.onCloseKey = [] { print("CLOSE-KEY\n"); };
        term.setClipboard = [this](const std::string& t) { clip = t; };
        term.requestClipboard = [this] { term.clipboardArrived(clip); };
        term.host.srv.bell = [this] { note("bell"); };
        term.host.srv.progress = [this](int state, int value) { note("progress:" + std::to_string(state) + ":" + std::to_string(value)); };
        term.start();
        std::string line;
        int status = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t space = line.find(' ');
            std::string cmd = line.substr(0, space), arg = space == std::string::npos ? "" : line.substr(space + 1);
            if (cmd.empty() || cmd[0] == '#') continue;
            if (cmd == "size") { std::istringstream s(arg); s >> cols >> rows; }
            else if (cmd == "sleep") std::this_thread::sleep_for(std::chrono::milliseconds(atoi(arg.c_str())));
            else if (cmd == "type") typeText(arg);
            else if (cmd == "enter") key(VK_RETURN, 0);
            else if (cmd == "key") keyCommand(arg);
            else if (cmd == "paste") { InputEvent e{ InputEvent::Text }; e.text = arg; send(e); }
            else if (cmd == "wheel") { InputEvent e{ InputEvent::Wheel }; std::istringstream s(arg); s >> e.value >> e.x >> e.y; send(e); }
            else if (cmd == "mouse") mouseCommand(arg);
            else if (cmd == "selection") {
                term.layout(cols, rows);
                std::string got = term.selectedText();
                print("SELECTION [" + got + "]\n");
                if (!arg.empty() && got != arg) { print("MISMATCH expected [" + arg + "]\n"); status = 1; }
            }
            else if (cmd == "selline") {
                term.layout(cols, rows);
                std::string got = term.selectionLine();
                print("SELLINE [" + got + "]\n");
                if (got != arg) { print("MISMATCH expected [" + arg + "]\n"); status = 1; }
            }
            else if (cmd == "events") {
                std::string got;
                { std::lock_guard<std::mutex> l(eventMutex); got.swap(events); }
                if (!got.empty()) got.pop_back();
                print("EVENTS [" + got + "]\n");
                if (got != arg) { print("MISMATCH expected [" + arg + "]\n"); status = 1; }
            }
            else if (cmd == "link") {
                term.layout(cols, rows);
                std::istringstream s(arg);
                std::string text, want;
                s >> text >> want;
                const Console::Frame& f = term.frame;
                std::string screen;
                for (uint32_t c : f.cells) screen += c < 0x80 ? (char)c : '?';
                size_t at = screen.find(text);
                std::string got = at == std::string::npos ? "<no " + text + ">" : term.linkTarget((int)at);
                print("LINK " + text + " [" + got + "]\n");
                if (got != (want == "-" ? "" : want)) { print("MISMATCH expected [" + want + "]\n"); status = 1; }
            }
            else if (cmd == "attr") {
                size_t sp = arg.find(' ');
                std::string text = arg.substr(0, sp), want = sp == std::string::npos ? "" : arg.substr(sp + 1);
                std::string got = attrAt(text);
                print("ATTR " + text + " [" + got + "]\n");
                if (!want.empty() && got != want) { print("MISMATCH expected [" + want + "]\n"); status = 1; }
            }
            else if (cmd == "pictures") {
                term.layout(cols, rows);
                std::string got;
                for (const Picture& p : term.frame.pictures)
                    got += (got.empty() ? "" : ";") + std::to_string(p.col) + "," + std::to_string(p.row) + " " + std::to_string(p.cols) + "x" + std::to_string(p.rows) + " " +
                           std::to_string(p.srcW) + "x" + std::to_string(p.srcH) + " z" + std::to_string(p.z);
                print("PICTURES [" + got + "]\n");
                if (got != arg) { print("MISMATCH expected [" + arg + "]\n"); status = 1; }
            }
            else if (cmd == "pixels") {
                size_t bar = arg.rfind(" | ");
                std::string want = arg.substr(0, bar), got;
                auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(bar == std::string::npos ? 3000 : atoi(arg.c_str() + bar + 3));
                do {
                    term.layout(cols, rows);
                    got.clear();
                    for (const Picture& p : term.frame.pictures)
                        for (size_t k = 0; k < (std::min)((size_t)2, p.bitmap->pixels.size()); ++k) {
                            char hex[16];
                            snprintf(hex, sizeof hex, "%s%08x", got.empty() ? "" : ",", p.bitmap->pixels[k]);
                            got += hex;
                        }
                    if (got == want) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                } while (std::chrono::steady_clock::now() < until);
                print("PIXELS [" + got + "]\n");
                if (got != want) { print("MISMATCH expected [" + want + "]\n"); status = 1; }
            }
            else if (cmd == "clipboard") {
                std::string got = clip;
                for (char& c : got) if (c == '\n') c = '|';
                print("CLIPBOARD [" + got + "]\n");
                if (got != arg) { print("MISMATCH expected [" + arg + "]\n"); status = 1; }
            }
            else if (cmd == "focus") send({ InputEvent::Focus, atoi(arg.c_str()) });
            else if (cmd == "modes")print("MODES " + term.host.srv.modes() + "\n");
            else if (cmd == "dump") dump();
            else if (cmd == "colors") colors(atoi(arg.c_str()));
            else if (cmd == "view") print("VIEW line=" + std::to_string(term.host.con.view.line) + " skip=" + std::to_string(term.host.con.view.skip) + " follow=" + std::to_string(term.host.con.view.follow) + " total=" + std::to_string(term.host.con.totalBytes()) + "\n");
            else if (cmd == "waitfor" && !waitFor(arg)) { print("TIMEOUT waiting for: " + arg + "\n"); dump(); status = 1; break; }
            else if (cmd == "quit") break;
        }
        term.stop();
        return status;
    }

private:
    Terminal term;
    int cols = 100, rows = 30;
    std::string clip;

    std::mutex eventMutex;
    std::string events;

    void note(const std::string& e) { std::lock_guard<std::mutex> l(eventMutex); events += e + " "; }

    static void print(const std::string& s) { writeStdout(s); }

    void send(const InputEvent& e) {
        term.layout(cols, rows);
        term.handle(e);
    }

    void mouseCommand(const std::string& arg) {
        static const std::pair<const char*, InputEvent::Kind> kinds[] = { { "down", InputEvent::LeftDown }, { "up", InputEvent::LeftUp },
            { "move", InputEvent::Move }, { "rdown", InputEvent::RightDown }, { "rup", InputEvent::RightUp },
            { "mdown", InputEvent::MiddleDown }, { "mup", InputEvent::MiddleUp }, { "hover", InputEvent::Hover } };
        std::istringstream s(arg);
        std::string name, mods;
        InputEvent e{ InputEvent::Move };
        s >> name >> e.x >> e.y >> mods;
        for (auto& k : kinds) if (name == k.first) e.kind = k.second;
        e.shift = mods.find('s') != std::string::npos;
        e.ctrl = mods.find('c') != std::string::npos;
        if (e.shift) e.state |= SHIFT_PRESSED;
        if (e.ctrl) e.state |= LEFT_CTRL_PRESSED;
        send(e);
    }

    void typeText(const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = (unsigned char)s[i++];
            int need = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
            if (need) cp &= 0x3F >> need;
            for (int k = 0; k < need && i < s.size(); ++k) cp = cp << 6 | ((unsigned char)s[i++] & 0x3F);
            InputEvent e{ InputEvent::Key };
            bool shift = false;
            e.value = cp < 0x80 ? vkForChar(cp, shift) : 0;
            e.ch = cp;
            e.shift = shift;
            e.state = e.shift ? SHIFT_PRESSED : 0;
            e.base = e.value ? layoutChar((unsigned)e.value, 0, false) : 0;
            e.shifted = e.value ? layoutChar((unsigned)e.value, 0, true) : 0;
            send(e);
            e.release = true;
            e.ch = 0;
            send(e);
        }
    }

    static constexpr unsigned REPEAT = 0x80000000u;

    void key(int vk, unsigned mods) {
        InputEvent e{ InputEvent::Key };
        e.value = vk;
        e.ctrl = (mods & LEFT_CTRL_PRESSED) != 0;
        e.shift = (mods & SHIFT_PRESSED) != 0;
        e.alt = (mods & LEFT_ALT_PRESSED) != 0;
        e.state = mods & ~REPEAT;
        uint32_t ch = charForVk(vk);
        if (vk == VK_RETURN) ch = '\r';
        if (e.ctrl && ch >= 'A' && ch <= 'Z') ch -= '@';
        else if (ch >= 'A' && ch <= 'Z' && !e.shift) ch += 32;
        e.ch = ch;
        e.scan = scanForVk(vk);
        e.base = layoutChar((unsigned)vk, (unsigned)e.scan, false);
        e.shifted = layoutChar((unsigned)vk, (unsigned)e.scan, true);
        send(e);
        if (mods & REPEAT) { e.repeat = true; send(e); e.repeat = false; }
        e.release = true;
        e.ch = 0;
        send(e);
    }

    void keyCommand(const std::string& arg) {
        std::istringstream s(arg);
        std::string name, mod;
        s >> name;
        unsigned mods = 0;
        while (s >> mod) mods |= mod == "ctrl" ? LEFT_CTRL_PRESSED : mod == "shift" ? SHIFT_PRESSED : mod == "alt" ? LEFT_ALT_PRESSED : mod == "repeat" ? REPEAT : 0;
        static const struct { const char* name; int vk; } names[] = {
            { "esc", VK_ESCAPE }, { "tab", VK_TAB }, { "back", VK_BACK }, { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT },
            { "right", VK_RIGHT }, { "home", VK_HOME }, { "end", VK_END }, { "del", VK_DELETE }, { "pgup", VK_PRIOR }, { "pgdn", VK_NEXT },
            { "f1", VK_F1 }, { "f10", VK_F10 }, { "space", VK_SPACE }, { "enter", VK_RETURN }, { "break", VK_CANCEL },
        };
        int vk = 0;
        for (auto& n : names) if (name == n.name) vk = n.vk;
        if (!vk && name.size() == 1) vk = toupper((unsigned char)name[0]);
        if (!vk) vk = (int)strtol(name.c_str(), nullptr, 0);
        key(vk, mods);
    }

    std::string screen() {
        term.layout(cols, rows);
        const Console::Frame& f = term.frame;
        std::string out;
        for (int y = 0; y < f.rows; ++y) {
            std::string row;
            for (int x = 0; x < f.cols; ++x) row += f.text(f.cells[(size_t)y * f.cols + x]);
            while (!row.empty() && row.back() == ' ') row.pop_back();
            out += row + "\n";
        }
        return out;
    }

    void dump() { print("---- screen\n" + screen() + "----\n"); }

    std::string attrAt(const std::string& text) {
        std::string s = screen();
        size_t at = s.find(text);
        if (at == std::string::npos) return "<no " + text + ">";
        size_t row = (size_t)std::count(s.begin(), s.begin() + at, '\n');
        size_t col = at - (row ? s.rfind('\n', at) + 1 : 0);
        const Console::Frame& f = term.frame;
        const vt::Attr& a = f.attrs[row * f.cols + col];
        char b[96];
        snprintf(b, sizeof(b), "fg=%08x bg=%08x flags=%04x uc=%08x", a.fg, a.bg, a.flags, a.underlineColor);
        return b;
    }

    void colors(int row) {
        term.layout(cols, rows);
        const Console::Frame& f = term.frame;
        std::string out = "COLORS row " + std::to_string(row) + ":";
        for (int x = 0; x < f.cols && row < f.rows; ++x) {
            const vt::Attr& a = f.attrs[(size_t)row * f.cols + x];
            char b[64];
            uint32_t ch = f.cells[(size_t)row * f.cols + x];
            std::string text = f.text(ch);
            std::string cps;
            for (size_t i = 0; i < text.size();) {
                unsigned char c0 = (unsigned char)text[i];
                int n = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
                uint32_t cp = n == 1 ? c0 : c0 & (0x7F >> n);
                for (int k = 1; k < n && i + k < text.size(); ++k) cp = cp << 6 | ((unsigned char)text[i + k] & 0x3F);
                char h[12];
                snprintf(h, sizeof(h), "%s%X", cps.empty() ? "" : "+", cp);
                cps += h;
                i += n;
            }
            snprintf(b, sizeof(b), " %d:U+%s:%08x", x, ch == Console::WIDE_TAIL ? "tail" : cps.c_str(), a.fg);
            if (f.cells[(size_t)row * f.cols + x] != ' ') out += b;
        }
        print(out + "\n");
    }

    bool waitFor(const std::string& arg) {
        std::string text = arg;
        int limit = 10000;
        size_t bar = arg.rfind(" | ");
        if (bar != std::string::npos) { text = arg.substr(0, bar); limit = atoi(arg.c_str() + bar + 3); }
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit);
        while (std::chrono::steady_clock::now() < until) {
            if (screen().find(text) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        return false;
    }
};
