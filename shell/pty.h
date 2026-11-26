#pragma once
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "console.h"
#include "emulator.h"
#include "keyboard.h"
#include "os.h"

namespace ccc {

using KeyInput = keyboard::KeyInput;

class ConsoleServer {
public:
    void (*changed)() = nullptr;
    std::function<void(const std::wstring&)> titleChanged;
    std::function<void(const std::string&)> clipboard;
    std::function<void()> bell;
    std::function<void(int, int)> progress;
    std::function<void(const std::string&)> cwd;
    std::function<void(const std::string&, const std::string&)> notification;
    std::function<void()> clipboardRequest;
    std::atomic<int> cellW{ 8 }, cellH{ 16 };

    explicit ConsoleServer(Console& c) : log(c) {}
    ~ConsoleServer() { closeMaster(); }

    bool start(int cols, int rows) {
        emu.reset(cols, rows);
        configure();
        running = true;
        return true;
    }

    bool started() const { return running; }

    void begin(int cols, int rows) {
        Console::Frame f;
        log.follow();
        log.layout(f, cols, rows, "");
        {
            std::lock_guard<std::mutex> l(m);
            emu.reset(cols, rows);
            configure();
            emu.snapshot(f);
            emu.logging = true;
            live = true;
        }
        notify();
    }

    bool launch(const std::string& exe, const std::string& cmdline, os::Process& process, int& error) {
        int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || grantpt(master) || unlockpt(master)) { error = errno; if (master >= 0) ::close(master); return false; }
        std::string slave = ptsname(master);
        winsize ws = windowSize();
        ioctl(master, TIOCSWINSZ, &ws);
        std::vector<std::string> args = os::splitCommandLine(cmdline);
        if (args.empty() || args[0].empty()) args.assign(1, exe);
        auto argv = os::pointers(args);
        std::vector<std::string> env = os::environment();
        std::erase_if(env, [](const std::string& e) { return e.rfind("TERM=", 0) == 0 || e.rfind("COLORTERM=", 0) == 0; });
        env.push_back("TERM=xterm-256color");
        env.push_back("COLORTERM=truecolor");
        auto envp = os::pointers(env);
        pid_t pid = -1;
        int rc = os::spawnOnTerminal(exe, argv.data(), envp.data(), slave, ws, pid);
        if (rc) { error = rc; ::close(master); return false; }
        process.pid = pid;
        process.waited = false;
        os::Handle wakeRead = os::NO_HANDLE;
        os::createPipe(wakeRead, wake, 0);
        {
            std::lock_guard<std::mutex> l(m);
            fd = master;
            child = pid;
        }
        emulation = std::thread([this, master, wakeRead] { pump(master, wakeRead); });
        return true;
    }

    void end() {
        if (wake != os::NO_HANDLE) { char c = 1; os::write(wake, &c, 1); }
        if (emulation.joinable()) emulation.join();
        os::close(wake);
        wake = os::NO_HANDLE;
        std::string text;
        {
            std::lock_guard<std::mutex> l(m);
            text = emu.pendingText(emu.lastUsedRow());
            emu.logging = false;
            live = false;
            child = -1;
        }
        closeMaster();
        if (!text.empty()) log.write(text);
        notify();
    }

    bool isLive() const { return live.load(); }

    std::wstring takeTypeahead() {
        std::lock_guard<std::mutex> l(m);
        return std::exchange(typeahead, std::wstring());
    }

    bool render(Console::Frame& f) {
        std::lock_guard<std::mutex> l(m);
        if (!live) return false;
        if (emu.sync) {
            double now = secondsSinceStart();
            if (!syncHeld) { syncHeld = true; syncSince = now; }
            if (now - syncSince < 0.25) return false;
        }
        else syncHeld = false;
        int next = emu.animate();
        emu.render(f);
        f.nextFrameMs = next;
        return true;
    }

    int cursorStyle() const { std::lock_guard<std::mutex> l(m); return emu.cursorShape; }

    void resize(int cols, int rows) {
        {
            std::lock_guard<std::mutex> l(m);
            if (cols == emu.main.cols && rows == emu.main.rows && cellW == emu.cellW && cellH == emu.cellH) return;
            emu.setCellSize(cellW, cellH);
            emu.resize(cols, rows);
            if (fd >= 0) { winsize ws = windowSize(); ioctl(fd, TIOCSWINSZ, &ws); }
            flushReplies();
        }
        transmit();
        notify();
    }

    bool processedInput() const { return false; }
    bool altScreen() const { std::lock_guard<std::mutex> l(m); return emu.altActive; }

    std::string screenText() const {
        std::lock_guard<std::mutex> l(m);
        if (!live || emu.altActive) return "";
        return emu.pendingText(emu.main.rows - 1);
    }

    std::string modes() const {
        std::lock_guard<std::mutex> l(m);
        return "mouse=" + std::to_string(emu.mouse) + " sgr=" + std::to_string(emu.mouseSgr) + " alt=" + std::to_string(emu.altActive) + " vtinput=1 focus=" +
               std::to_string(emu.focusEvents);
    }

    void key(const KeyInput& k) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live) { if (k.down && k.ch) typeahead += (wchar_t)k.ch; return; }
            if (emu.kittyFlags() & 9) send(keyboard::kittyKey(k, emu.kittyFlags()));
            else if (k.down) send(keyboard::legacyKey(k, emu.appCursor, emu.modifyOtherKeys));
        }
        transmit();
        notify();
    }

    void paste(const std::string& utf8) {
        std::string text;
        for (size_t i = 0; i < utf8.size(); ++i) {
            if (utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') continue;
            text += utf8[i] == '\n' ? '\r' : utf8[i];
        }
        {
            std::lock_guard<std::mutex> l(m);
            if (!live) { for (size_t i = 0; i < text.size();) typeahead += (wchar_t)unicode::decodeUtf8(text, i); return; }
            if (emu.announcePaste(utf8)) flushReplies();
            else queue(emu.bracketedPaste ? "\x1b[200~" + text + "\x1b[201~" : text);
        }
        transmit();
    }

    void answerClipboard(const std::string& text, bool granted) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live) return;
            emu.answerClipboard(text, granted);
            flushReplies();
        }
        transmit();
    }

    bool mouseWheel(int notches, int col, int row, int px, int py, unsigned) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live || !emu.mouse) return false;
            for (int i = 0; i < std::abs(notches); ++i) queue(mouseSeq(notches > 0 ? 64 : 65, false, col, row, px, py));
        }
        transmit();
        return true;
    }

    bool mouseButton(int button, bool down, bool motion, int col, int row, int px, int py, unsigned keyState) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live || !emu.mouse) return false;
            if (motion && emu.mouse < (button == 3 ? 1003 : 1002)) return true;
            int at = emu.mousePixels ? py * 65536 + px : row * 65536 + col;
            if (motion && at == lastMotion) return true;
            lastMotion = at;
            int mods = (keyState & SHIFT_PRESSED ? 4 : 0) | (keyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED) ? 8 : 0) | (keyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED) ? 16 : 0);
            queue(mouseSeq(button + mods + (motion ? 32 : 0), !down && !motion, col, row, px, py));
        }
        transmit();
        return true;
    }

    void focus(bool on) {
        {
            std::lock_guard<std::mutex> l(m);
            if (live && emu.focusEvents) queue(on ? "\x1b[I" : "\x1b[O");
        }
        transmit();
    }

    void ctrlEvent(unsigned event) {
        std::lock_guard<std::mutex> l(m);
        if (child <= 0) return;
        pid_t group = fd >= 0 ? tcgetpgrp(fd) : -1;
        kill(-(group > 0 ? group : child), event == CTRL_C_EVENT ? SIGINT : SIGQUIT);
    }

    void shutdown() {
        pid_t pid;
        { std::lock_guard<std::mutex> l(m); pid = child; }
        if (pid > 0) kill(-pid, SIGHUP);
    }

private:
    Console& log;
    Emulator emu;
    mutable std::mutex m;
    std::mutex writing;
    std::atomic<bool> live{ false };
    bool running = false, syncHeld = false;
    double syncSince = 0;
    int fd = -1;
    os::Handle wake = os::NO_HANDLE;
    pid_t child = -1;
    int lastMotion = -1;
    std::thread emulation;
    std::wstring typeahead;
    std::string replies, outgoing;
    std::vector<std::function<void()>> outbox;

    static double secondsSinceStart() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    void notify() { if (changed) changed(); }

    winsize windowSize() const {
        winsize ws = {};
        ws.ws_col = (unsigned short)emu.main.cols;
        ws.ws_row = (unsigned short)emu.main.rows;
        ws.ws_xpixel = (unsigned short)(emu.main.cols * cellW);
        ws.ws_ypixel = (unsigned short)(emu.main.rows * cellH);
        return ws;
    }

    void configure() {
        emu.cellW = cellW;
        emu.cellH = cellH;
        emu.onReply = [this](const std::string& s) { replies += s; };
        emu.onTitle = [this](const std::wstring& t) { outbox.push_back([this, t] { if (titleChanged) titleChanged(t); }); };
        emu.onClipboard = [this](const std::string& s) { outbox.push_back([this, s] { if (clipboard) clipboard(s); }); };
        emu.onBell = [this] { outbox.push_back([this] { if (bell) bell(); }); };
        emu.onProgress = [this](int state, int value) { outbox.push_back([this, state, value] { if (progress) progress(state, value); }); };
        emu.onCwd = [this](const std::string& p) { outbox.push_back([this, p] { if (cwd) cwd(p); }); };
        emu.onNotify = [this](const std::string& t, const std::string& b) { outbox.push_back([this, t, b] { if (notification) notification(t, b); }); };
        emu.onClipboardRead = [this] { outbox.push_back([this] { if (clipboardRequest) clipboardRequest(); }); };
        emu.onScrollOut = [this](const char* p, size_t n) { log.write(p, n); };
        emu.onClear = [this] { log.clear(); };
    }

    void queue(const std::string& s) { outgoing += s; }

    void transmit() {
        std::lock_guard<std::mutex> w(writing);
        std::string bytes;
        int target;
        {
            std::lock_guard<std::mutex> l(m);
            bytes.swap(outgoing);
            target = fd;
        }
        if (target >= 0 && !bytes.empty()) os::write(target, bytes.data(), bytes.size());
    }

    void send(const std::u16string& keys) { queue(os::fromUtf16(keys.data(), keys.size())); }

    void flushReplies() { queue(std::exchange(replies, std::string())); }

    std::string mouseSeq(int code, bool release, int col, int row, int px, int py) const {
        if (emu.mousePixels) { col = px; row = py; }
        if (emu.mouseSgr || emu.mousePixels) return "\x1b[<" + std::to_string(code) + ";" + std::to_string(col + 1) + ";" + std::to_string(row + 1) + (release ? "m" : "M");
        std::string s = "\x1b[M";
        s += (char)(32 + (release ? (code & ~3) | 3 : code));
        s += (char)(32 + (std::min)(col + 1, 223));
        s += (char)(32 + (std::min)(row + 1, 223));
        return s;
    }

    static constexpr size_t SLICE = 1 << 16;

    struct Inbox {
        static constexpr size_t LIMIT = 4 << 20;
        std::mutex m;
        std::condition_variable cv;
        std::string bytes;
        bool closed = false;
    };

    static void readInto(int master, int wakeRead, Inbox& in) {
        std::vector<char> buf(SLICE);
        bool stopping = false;
        for (;;) {
            pollfd ready[2] = { { master, POLLIN, 0 }, { wakeRead, POLLIN, 0 } };
            int r = poll(ready, stopping ? 1 : 2, stopping ? 0 : -1);
            if (r < 0 && errno == EINTR) continue;
            stopping |= r > 0 && (ready[1].revents & POLLIN);
            ssize_t n = r > 0 && ready[0].revents ? ::read(master, buf.data(), buf.size()) : 0;
            if (n < 0 && errno == EINTR) continue;
            std::unique_lock<std::mutex> l(in.m);
            if (n <= 0 && (stopping || r < 0 || ready[0].revents)) { in.closed = true; in.cv.notify_all(); return; }
            if (n <= 0) continue;
            in.cv.wait(l, [&] { return in.bytes.size() < Inbox::LIMIT; });
            bool wasEmpty = in.bytes.empty();
            in.bytes.append(buf.data(), (size_t)n);
            if (wasEmpty) in.cv.notify_all();
        }
    }

    void pump(int master, int wakeRead) {
        Inbox in;
        std::thread reading([master, wakeRead, &in] { readInto(master, wakeRead, in); });
        std::string batch;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(in.m);
                in.cv.wait(l, [&] { return !in.bytes.empty() || in.closed; });
                if (in.bytes.empty()) break;
                bool wasFull = in.bytes.size() >= Inbox::LIMIT;
                batch.clear();
                batch.swap(in.bytes);
                if (wasFull) in.cv.notify_all();
            }
            for (size_t at = 0; at < batch.size(); at += SLICE) {
                std::vector<std::function<void()>> events;
                {
                    std::lock_guard<std::mutex> l(m);
                    emu.writeBytes(batch.data() + at, (std::min)(SLICE, batch.size() - at), os::UTF8_PAGE);
                    flushReplies();
                    events.swap(outbox);
                }
                transmit();
                for (auto& e : events) e();
                notify();
            }
        }
        reading.join();
        os::close(wakeRead);
    }

    void closeMaster() {
        std::lock_guard<std::mutex> w(writing);
        std::lock_guard<std::mutex> l(m);
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

}
