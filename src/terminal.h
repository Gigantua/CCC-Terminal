#pragma once
#include <algorithm>
#include <filesystem>
#include <memory>
#include <thread>
#include "folders.h"
#include "host.h"
#include "input.h"
#include "lineedit.h"
#include "screen.h"
#include "selection.h"

class Terminal {
public:
    WindowHost host;
    Console::Frame frame;
    FontMetrics metrics;
    int barWidth = 12;
    float defaultPoints = 12;
    std::atomic<int> exitCode{ 0 };
    std::function<void()> onExit, onCloseKey;
    std::function<void(const std::string&)> setClipboard, openUri;
    std::function<void()> requestClipboard;

    void clipboardArrived(const std::string& text) {
        switch (std::exchange(clipboardUse, ClipboardUse::None)) {
        case ClipboardUse::Paste: paste(text); break;
        case ClipboardUse::PasteText: pasteText(text); break;
        case ClipboardUse::Answer: host.srv.answerClipboard(text, true); break;
        case ClipboardUse::None: break;
        }
    }

    void start() {
        stop();
        host.closed = false;
        host.exited = false;
        if (!host.srv.started()) {
            host.srv.changed = requestFrame;
            host.srv.titleChanged = [this](const std::wstring& t) { host.showTitle(utf8FromWide(t)); };
            host.srv.clipboard = [this](const std::string& t) { if (setClipboard) setClipboard(t); };
            host.srv.clipboardRequest = [this] { clipboardAsked = true; requestFrame(); };
            host.srv.cwd = [this](const std::string& reported) { std::lock_guard<std::mutex> l(dirMutex); programDir = programDirectory(reported); };
            host.srv.start(host.cols, host.rows);
        }
        shell = std::make_unique<ccc::Shell>(host);
        host.atPrompt = &shell->atPrompt;
        shellThread = std::thread([this] {
            host.con.write(ccc::Shell::banner());
            try {
                shell->applyRegistryDefaults();
                shell->applyDefaultColor();
                shell->runAutoRun();
                exitCode = shell->run();
            }
            catch (ccc::ExitShell& e) { exitCode = e.code; }
            catch (...) {}
            host.exited = true;
            if (onExit) onExit();
        });
    }

    void stop() {
        if (host.srv.started()) host.srv.shutdown();
        if (!shellThread.joinable()) return;
        shell->interrupt();
        host.close();
        shellThread.join();
    }

    bool idleAtPrompt() const { return shell && shell->atPrompt && host.waiting && host.child == os::NO_HANDLE; }

    std::string directory() {
        {
            std::lock_guard<std::mutex> l(dirMutex);
            if (!consoleLive()) programDir.clear();
            if (!programDir.empty()) return programDir;
        }
        return os::currentDirectory();
    }

    void submitLine(const std::string& text) {
        line.set(text);
        submitInput();
    }

    void handle(const InputEvent& e) {
        if (!frame.cols) return;
        if (e.kind == InputEvent::Key && !e.release && e.ctrl && e.shift && !e.alt && e.value == 'F') {
            if (searching) closeSearch();
            else searching = true;
            return;
        }
        if (e.kind == InputEvent::Clipboard) { clipboardArrived(e.text); return; }
        if (searching && searchInput(e)) return;
        if (clipboardAsked && clipboardAnswer(e)) return;
        if (e.kind == InputEvent::Key && !e.release && e.ctrl && e.shift && !e.alt && e.value == 'O') {
            std::string output = host.con.lastOutput();
            if (!output.empty() && setClipboard) setClipboard(output);
            return;
        }
        if (e.kind == InputEvent::Key && !e.release && e.ctrl && e.shift && !e.alt && (e.value == VK_UP || e.value == VK_DOWN) && !host.srv.altScreen()) {
            host.con.setScreenTail(consoleLive() ? host.srv.screenText() : "");
            if (host.con.jumpPrompt(e.value == VK_UP, frame.cols, frame.rows)) sel.shown = false;
            return;
        }
        if (reportMouse(e)) return;
        switch (e.kind) {
        case InputEvent::Hover: hoverCell = cellAt(e.x, e.y); hoverCtrl = e.ctrl; return;
        case InputEvent::LeftDown:
            if (!(e.ctrl && e.x < pixelW - barWidth && openLink(cellAt(e.x, e.y)))) onLeftDown(e.x, e.y, e.shift);
            return;
        case InputEvent::LeftUp: onLeftUp(); return;
        case InputEvent::LeftDouble: selectAt(cellAt(e.x, e.y), e.value == 3); return;
        case InputEvent::Move: onMove(e.x, e.y); return;
        case InputEvent::RightDown: if (sel.shown) copySelection(); else askClipboard(ClipboardUse::Paste); return;
        case InputEvent::Wheel: if (e.ctrl) { host.resizeFont(e.value > 0 ? 1.0f : -1.0f); return; } break;
        case InputEvent::Key: if (e.value == VK_CONTROL) hoverCtrl = !e.release && hoverCell >= 0; break;
        default: break;
        }
        if (consoleLive()) onConsole(e);
        else onPrompt(e);
    }

    void layout(int width, int height) {
        pixelW = width;
        pixelH = height;
        int cols = std::clamp((width - barWidth) / metrics.cellW, 1, 1024), rows = std::clamp(height / metrics.cellH, 1, 1024);
        host.cols = cols;
        host.rows = rows;
        if (consoleLive()) {
            host.srv.resize(cols, rows);
            if (host.con.view.follow) { host.srv.render(frame); drawSearchBox(); drawClipboardQuestion(); return; }
            host.con.setScreenTail(host.srv.screenText());
        }
        else {
            host.con.setScreenTail("");
            acceptTypeahead();
        }
        if (clipboardAsked) host.con.layout(frame, cols, rows, "A program wants to read the clipboard. Allow? (y/n)");
        else if (numberMode) host.con.layout(frame, cols, rows, "Enter command number: " + numberDigits);
        else if (!pendingPaste.empty()) host.con.layout(frame, cols, rows, "Run " + std::to_string(pasteLines) + " pasted lines? Enter = yes, other key = no");
        else host.con.layout(frame, cols, rows, line.text, (int)line.pos);
        if (markPending && frame.markRow >= 0) {
            int from = frame.markRow * frame.cols + frame.markCol;
            sel.set(from, from + (int)unicode::utf8Length(query) - 1, frame.cols);
            markPending = false;
        }
        drawSearchBox();
    }

    bool showsCursor() const { return !host.exited; }
    std::string selectedText() { return selectionText(); }

    std::string selectionLine() const {
        int from, to;
        if (!sel.visible(frame, from, to)) return "";
        int row = from / frame.cols;
        return cells::text(frame, row * frame.cols, (row + 1) * frame.cols);
    }

    std::string linkTarget(int cell) const {
        int from, to;
        if (uint16_t id = cells::hyperlink(frame, cell, from, to)) {
            std::string uri = vt::linkUri(id);
            return cells::openable(uri) ? uri : std::string();
        }
        return cells::link(frame, cell, from, to) ? cells::ascii(frame, from, to) : std::string();
    }
    screen::Highlight highlight() const {
        screen::Highlight h;
        if (showsCursor()) { h.cursorRow = frame.cursorRow; h.cursorCol = frame.cursorCol; }
        int style = consoleLive() ? host.srv.cursorStyle() : 0;
        h.barCursor = consoleLive() ? style == 3 || style == 4 : host.insertMode.load();
        h.beamCursor = style == 5 || style == 6;
        sel.visible(frame, h.selFrom, h.selTo);
        if (hoverCtrl) cells::link(frame, hoverCell, h.linkFrom, h.linkTo);
        return h;
    }

private:
    std::unique_ptr<ccc::Shell> shell;
    std::thread shellThread;
    LineEdit line;
    size_t historyPos = 0, searchPos = SIZE_MAX;
    int pendingKey = 0;
    bool numberMode = false;
    std::string numberDigits;
    std::string pendingPaste;
    size_t pasteLines = 0;
    Selection sel;
    bool scrollDrag = false, swallowChar = false, hoverCtrl = false;
    int pixelW = 0, pixelH = 0, mouseX = 0, mouseY = 0, hoverCell = -1;
    int wheelAccum = 0, notchAccum = 0;
    int reported = 0;
    bool searching = false, searchMiss = false, markPending = false;
    std::atomic<bool> clipboardAsked{ false };
    enum class ClipboardUse { None, Paste, PasteText, Answer };
    ClipboardUse clipboardUse = ClipboardUse::None;

    void askClipboard(ClipboardUse use) {
        clipboardUse = use;
        if (requestClipboard) requestClipboard();
    }
    std::mutex dirMutex;
    std::string programDir;

    bool clipboardAnswer(const InputEvent& e) {
        if (e.kind != InputEvent::Key || e.release || isModifier(e.value)) return e.kind == InputEvent::Key || e.kind == InputEvent::Char;
        clipboardAsked = false;
        swallowChar = true;
        if (e.value == 'Y') askClipboard(ClipboardUse::Answer);
        else host.srv.answerClipboard(std::string(), false);
        return true;
    }

    void drawClipboardQuestion() {
        if (!clipboardAsked || !frame.rows) return;
        drawBottomRow("A program wants to read the clipboard. Allow? (y/n)");
    }
    std::string query;
    uint64_t matchAt = Console::NOT_FOUND;

    void closeSearch() {
        searching = searchMiss = markPending = false;
        query.clear();
        matchAt = Console::NOT_FOUND;
        host.con.clearMark();
    }

    void runSearch(bool older, bool fresh) {
        searchMiss = false;
        if (query.empty()) { matchAt = Console::NOT_FOUND; return; }
        host.con.setScreenTail(consoleLive() ? host.srv.screenText() : "");
        uint64_t from = !fresh && matchAt != Console::NOT_FOUND ? matchAt : older ? Console::NOT_FOUND : 0;
        uint64_t g = host.con.find(query, from, older);
        if (g == Console::NOT_FOUND) { searchMiss = true; return; }
        matchAt = g;
        host.con.reveal(g, frame.cols, frame.rows);
        markPending = true;
    }

    bool searchInput(const InputEvent& e) {
        auto type = [&](uint32_t ch) {
            unicode::appendUtf8(query, ch);
            runSearch(true, true);
        };
        switch (e.kind) {
        case InputEvent::Char:
            if (e.value >= 0x20 && e.value != 0x7F) type((uint32_t)e.value);
            return true;
        case InputEvent::Key:
            if (e.release || isModifier(e.value)) return true;
            if (e.value == VK_ESCAPE) { closeSearch(); return true; }
            if (e.value == VK_RETURN || e.value == VK_F3) { runSearch(!e.shift, false); return true; }
            if (e.value == VK_BACK) {
                if (!query.empty()) query.erase(unicode::utf8Prev(query, query.size()));
                runSearch(true, true);
                return true;
            }
            if (e.ctrl && (e.value == 'C' || e.value == VK_INSERT)) { copySelection(); return true; }
            if (e.ch >= 0x20 && e.ch != 0x7F && !e.ctrl) type(e.ch);
            return true;
        case InputEvent::Text:
            for (size_t i = 0; i < e.text.size();) {
                uint32_t cp = unicode::decodeUtf8(e.text, i);
                if (cp >= 0x20) unicode::appendUtf8(query, cp);
            }
            runSearch(true, true);
            return true;
        default:
            return false;
        }
    }

    void drawSearchBox() {
        if (!searching || !frame.rows) return;
        drawBottomRow("Find: " + query + (searchMiss ? "   (not found)" : "") + "   Enter: older  Shift+Enter: newer  Esc: close");
        frame.cursorCol = (std::min)(6 + (int)unicode::utf8Length(query), frame.cols - 1);
    }

    void drawBottomRow(const std::string& text) {
        std::u32string cps;
        for (size_t i = 0; i < text.size();) cps += (char32_t)unicode::decodeUtf8(text, i);
        int row = frame.rows - 1, cols = frame.cols;
        vt::Attr a;
        a.flags = vt::REVERSE;
        for (int x = 0; x < cols; ++x) {
            size_t i = (size_t)row * cols + x;
            frame.cells[i] = x < (int)cps.size() && cps[x] < Console::CLUSTER_BASE ? (uint32_t)cps[x] : ' ';
            frame.attrs[i] = a;
        }
        frame.cursorRow = row;
        frame.cursorCol = (std::min)((int)cps.size(), cols - 1);
    }

    bool consoleLive() const { return host.srv.isLive(); }

    bool reportMouse(const InputEvent& e) {
        int button = 0;
        bool down = true;
        switch (e.kind) {
        case InputEvent::LeftDown: case InputEvent::LeftDouble: break;
        case InputEvent::LeftUp: down = false; break;
        case InputEvent::MiddleDown: button = 1; break;
        case InputEvent::MiddleUp: button = 1; down = false; break;
        case InputEvent::RightDown: button = 2; break;
        case InputEvent::RightUp: button = 2; down = false; break;
        case InputEvent::Focus:
            if (consoleLive()) host.srv.focus(e.value != 0);
            return true;
        case InputEvent::Hover: {
            int cell = cellAt(e.x, e.y);
            if (reported || !consoleLive() || !host.con.view.follow) return false;
            host.srv.mouseButton(3, false, true, cell % frame.cols, cell / frame.cols, e.x, e.y, e.state);
            return false;
        }
        case InputEvent::Move: {
            if (!reported) return false;
            int cell = cellAt(e.x, e.y);
            int held = reported & 1 ? 0 : reported & 2 ? 1 : 2;
            host.srv.mouseButton(held, true, true, cell % frame.cols, cell / frame.cols, e.x, e.y, e.state);
            return true;
        }
        default: return false;
        }
        int bit = 1 << button, cell = cellAt(e.x, e.y);
        if (!down) {
            if (!(reported & bit)) return false;
            reported &= ~bit;
            host.srv.mouseButton(button, false, false, cell % frame.cols, cell / frame.cols, e.x, e.y, e.state);
            return true;
        }
        int from, to;
        if (e.shift || e.x >= pixelW - barWidth || !consoleLive() || !host.con.view.follow) return false;
        if (button == 0 && e.ctrl && cells::link(frame, cell, from, to)) return false;
        if (!host.srv.mouseButton(button, true, false, cell % frame.cols, cell / frame.cols, e.x, e.y, e.state)) return false;
        reported |= bit;
        sel.shown = sel.dragging = false;
        return true;
    }

    int cellAt(int x, int y) const {
        return std::clamp(y / metrics.cellH, 0, frame.rows - 1) * frame.cols + std::clamp(x / metrics.cellW, 0, frame.cols - 1);
    }

    void acceptTypeahead() {
        if (!host.srv.started()) return;
        std::wstring keys = host.srv.takeTypeahead();
        if (keys.empty()) return;
        for (wchar_t ch : keys) {
            if (ch == 13) { submitInput(); continue; }
            if (ch < 0x20) continue;
            std::string utf8;
            unicode::appendUtf8(utf8, ch);
            line.insert(utf8);
        }
        edited(false);
    }

    void onPrompt(const InputEvent& e) {
        acceptTypeahead();
        switch (e.kind) {
        case InputEvent::Char: onChar(e.value); break;
        case InputEvent::Key:
            if (e.release) break;
            onKey(e.value, e.ctrl, e.shift, e.alt, e.state);
            if (e.ch && !e.alt) onChar((int)e.ch);
            else swallowChar = false;
            break;
        case InputEvent::Wheel: scrollWheel(e.value); break;
        case InputEvent::Text: line.insert(e.text); edited(false); break;
        default: break;
        }
    }

    void onConsole(const InputEvent& e) {
        ccc::ConsoleServer& srv = host.srv;
        switch (e.kind) {
        case InputEvent::Char: {
            ccc::KeyInput k;
            k.ch = (char16_t)e.value;
            srv.key(k);
            k.down = false;
            srv.key(k);
            break;
        }
        case InputEvent::Key: {
            if (!e.release && consoleKey(e)) return;
            if (!e.release && !isModifier(e.value)) follow();
            ccc::KeyInput k;
            k.vk = (uint16_t)e.value;
            k.scan = (uint16_t)e.scan;
            k.ch = (char16_t)(e.ch < 0x10000 ? e.ch : 0);
            k.state = e.state;
            k.base = e.base;
            k.shifted = e.shifted;
            k.super = e.super;
            k.down = !e.release;
            k.repeat = e.repeat;
            if (e.ch >= 0x10000) {
                std::wstring pair;
                unicode::appendUtf16(pair, e.ch);
                for (wchar_t w : pair) { k.ch = (char16_t)w; srv.key(k); }
            }
            else srv.key(k);
            break;
        }
        case InputEvent::Wheel:
            if (host.con.view.follow) {
                notchAccum += e.value;
                int notches = notchAccum / WHEEL_DELTA;
                notchAccum -= notches * WHEEL_DELTA;
                int cell = cellAt(e.x, e.y);
                if (notches && srv.mouseWheel(notches, cell % frame.cols, cell / frame.cols, e.x, e.y, e.state)) break;
                if (srv.altScreen()) break;
            }
            scrollWheel(e.value);
            break;
        case InputEvent::Text: paste(e.text); break;
        default: break;
        }
    }

    bool consoleKey(const InputEvent& e) {
        int vk = e.value;
        if (selectionKey(vk, e.ctrl) || zoomKey(vk, e.ctrl, e.alt, e.state)) return true;
        if ((e.ctrl && !e.alt && vk == 'V') || (e.shift && vk == VK_INSERT)) { askClipboard(ClipboardUse::Paste); return true; }
        if (e.ctrl && vk == 'C' && !e.shift && host.srv.processedInput()) { signal(CTRL_C_EVENT); return true; }
        if (vk == VK_CANCEL) { signal(CTRL_BREAK_EVENT); return true; }
        return !host.srv.altScreen() && pageKey(vk, e.shift);
    }

    void signal(unsigned event) {
        host.srv.ctrlEvent(event);
        if (shell) shell->markInterrupted();
    }

    static bool isModifier(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL || vk == VK_LWIN || vk == VK_RWIN; }

    bool selectionKey(int vk, bool ctrl) {
        if (!sel.shown) return false;
        if (vk == VK_RETURN || (ctrl && (vk == 'C' || vk == VK_INSERT))) copySelection();
        else if (vk == VK_ESCAPE) clearSelection();
        else return false;
        return true;
    }

    bool pageKey(int vk, bool shift) {
        if (!shift || (vk != VK_PRIOR && vk != VK_NEXT)) return false;
        scrollView(vk == VK_PRIOR ? -(frame.rows - 1) : frame.rows - 1);
        return true;
    }

    bool zoomKey(int vk, bool ctrl, bool alt, unsigned state) {
        if (!ctrl || alt || (state & RIGHT_ALT_PRESSED)) return false;
        if (vk == VK_OEM_PLUS || vk == VK_ADD) host.resizeFont(1.0f);
        else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) host.resizeFont(-1.0f);
        else if (vk == '0' || vk == VK_NUMPAD0) host.setFont("", defaultPoints);
        else return false;
        swallowChar = true;
        return true;
    }

    void paste(const std::string& text) {
        if (!consoleLive()) { pasteText(text); return; }
        host.con.follow();
        host.srv.paste(text);
    }

    void follow() {
        sel.shown = false;
        host.con.follow();
    }

    void select(int from, int last) {
        sel.set(from, last, frame.cols);
        if (!consoleLive()) host.con.freeze(frame.cols, frame.rows);
    }

    void selectAt(int cell, bool wholeLine) {
        int from, last;
        if (wholeLine) cells::line(frame, cell, from, last);
        else if (!cells::word(frame, cell, from, last)) return;
        select(from, last);
    }

    std::string selectionText() {
        if (!sel.valid(frame)) return "";
        int cols = frame.cols, n = cols * frame.rows, from = sel.first(), to = sel.last() + 1;
        const Console::Frame* f = &frame;
        Console::Frame tall;
        if ((from < 0 || to > n) && !(consoleLive() && host.con.view.follow)) {
            Console::View saved = host.con.view;
            auto rowOf = [cols](int cell) { return cell >= 0 ? cell / cols : -((-cell + cols - 1) / cols); };
            int top = (std::min)(rowOf(from), 0), bottom = (std::max)(rowOf(to - 1), frame.rows - 1);
            int moved = top < 0 ? host.con.scroll(top, cols, frame.rows) : 0;
            host.con.view.follow = false;
            host.con.layout(tall, cols, bottom - moved + 1, line.text, (int)line.pos);
            host.con.view = saved;
            from -= moved * cols;
            to -= moved * cols;
            f = &tall;
            n = cols * tall.rows;
        }
        return cells::text(*f, (std::max)(from, 0), (std::min)(to, n));
    }

    void clearSelection() {
        if (!sel.shown && !sel.dragging) return;
        sel.shown = sel.dragging = false;
        if (!consoleLive()) host.con.follow();
    }

    void copySelection() {
        std::string t = selectionText();
        if (!t.empty() && setClipboard) setClipboard(t);
        clearSelection();
    }

    bool openLink(int cell) {
        std::string target = linkTarget(cell);
        if (target.empty()) return false;
        if (openUri) openUri(target);
        return true;
    }

    void onLeftDown(int x, int y, bool shift) {
        if (x >= pixelW - barWidth) { scrollDrag = true; onMove(x, y); return; }
        mouseX = x;
        mouseY = y;
        if (!consoleLive()) host.con.freeze(frame.cols, frame.rows);
        if (shift && sel.valid(frame)) { sel.end = cellAt(x, y); sel.dragging = true; return; }
        sel.start(cellAt(x, y), frame.cols);
    }

    void onMove(int x, int y) {
        if (scrollDrag) { host.con.setScreenTail(consoleLive() ? host.srv.screenText() : ""); host.con.seek((double)y / (std::max)(pixelH, 1), frame.cols, frame.rows); sel.shown = false; return; }
        if (!sel.dragging) return;
        mouseX = x;
        mouseY = y;
        int below = y - frame.rows * metrics.cellH;
        if (y < 0) scrollView(-(1 + -y / metrics.cellH));
        else if (below >= 0) scrollView(1 + below / metrics.cellH);
        sel.extend(cellAt(x, y));
    }

    void onLeftUp() {
        scrollDrag = false;
        if (!sel.dragging) return;
        sel.dragging = false;
        if (!sel.shown && !consoleLive()) host.con.follow();
    }

    void scrollWheel(int delta) {
        wheelAccum += delta;
        int lines = wheelAccum * 3 / WHEEL_DELTA;
        wheelAccum -= lines * WHEEL_DELTA / 3;
        if (lines) scrollView(-lines);
    }

    void scrollView(int delta) {
        host.con.setScreenTail(consoleLive() ? host.srv.screenText() : "");
        sel.shift(host.con.scroll(delta, frame.cols, frame.rows));
        if (sel.dragging) sel.extend(cellAt(mouseX, mouseY));
    }

    void submitInput() {
        if (host.exited) return;
        std::string text = line.text;
        line.clear();
        host.con.write(text + "\r\n");
        host.con.markOutput();
        host.addHistory(text);
        historyPos = host.historyCount();
        if (os::Handle child = host.child; child != os::NO_HANDLE) { std::string l = text + "\r\n"; os::write(child, l.data(), l.size()); }
        else host.submit(text);
    }

    void pasteText(const std::string& clip) {
        size_t last = clip.find_last_not_of("\r\n");
        size_t breaks = last == std::string::npos ? 0 : (size_t)std::count(clip.begin(), clip.begin() + last, '\n');
        if (breaks) {
            pendingPaste = clip;
            pasteLines = breaks + 1;
            follow();
            return;
        }
        runPaste(clip);
    }

    void runPaste(const std::string& clip) {
        for (char c : clip) {
            if (c == '\r') continue;
            if (c == '\n') { submitInput(); continue; }
            line.insert(std::string(1, (unsigned char)c < 0x20 || c == 0x7F ? ' ' : c));
        }
        edited(false);
    }

    void interruptShell() {
        if (!shell || host.exited) return;
        if (idleAtPrompt()) {
            host.con.write(line.text + "^C");
            line.clear();
            host.submit("");
        }
        else {
            host.con.write("^C");
            shell->interrupt();
        }
        host.con.follow();
    }

    void edited(bool keepCompletion) {
        if (!keepCompletion) line.completing = false;
        follow();
    }

    void typeText(const std::string& text) {
        line.type(text, host.insertMode);
        edited(false);
    }

    std::string lastCommand() { size_t n = host.historyCount(); return n ? host.historyAt(n - 1) : std::string(); }

    void applyPendingKey(int c) {
        if (pendingKey == VK_F2) {
            std::string last = lastCommand();
            size_t at = last.find((char)c, line.pos + 1);
            if (at != std::string::npos) line.copyFrom(last, at);
        }
        else line.deleteUpTo((char)c);
        pendingKey = 0;
        edited(false);
    }

    void recallHistory(size_t index) {
        if (index >= host.historyCount()) return;
        historyPos = index;
        line.set(host.historyAt(index));
    }

    void searchHistory() {
        size_t n = host.historyCount();
        if (!n) return;
        std::string prefix = line.text.substr(0, line.pos);
        size_t from = searchPos == SIZE_MAX ? n : searchPos;
        for (size_t k = 1; k <= n; ++k) {
            size_t i = (from + n - k) % n;
            std::string h = host.historyAt(i);
            if (cells::startsWithIgnoringCase(h, prefix) && h != line.text) {
                line.text = h;
                searchPos = historyPos = i;
                break;
            }
        }
        follow();
    }

    void listHistory() {
        std::vector<std::string> lines = host.history();
        std::string out = "\r\n";
        for (size_t i = 0; i < lines.size(); ++i) out += std::to_string(i) + ": " + lines[i] + "\r\n";
        host.con.write(out + (shell ? shell->prompt() : std::string()));
        host.con.follow();
    }

    void onChar(int c) {
        if (swallowChar) { swallowChar = false; return; }
        std::string utf8;
        unicode::appendUtf8(utf8, (uint32_t)c);
        if (host.wantKey) { host.submit(utf8); return; }
        if (numberMode) { if (c >= '0' && c <= '9') numberDigits += (char)c; return; }
        if (pendingKey) { applyPendingKey(c); return; }
        if (c >= 0x20 && c != 0x7F) typeText(utf8);
    }

    bool onKeyForProgram(int vk, bool ctrl) {
        if (!host.wantKey || isModifier(vk)) return host.wantKey;
        if (vk == VK_RETURN) { host.submit("\r"); swallowChar = true; return true; }
        if (vk == VK_ESCAPE) { host.submit("\x1b"); swallowChar = true; return true; }
        if (!ctrl && producesChar(vk)) return true;
        host.submit("");
        return true;
    }

    bool onNumberKey(int vk) {
        if (!numberMode) return false;
        if (vk == VK_RETURN) { if (!numberDigits.empty()) recallHistory((size_t)atoi(numberDigits.c_str())); numberMode = false; swallowChar = true; }
        else if (vk == VK_ESCAPE) { numberMode = false; swallowChar = true; }
        else if (vk == VK_BACK && !numberDigits.empty()) numberDigits.pop_back();
        host.con.follow();
        return true;
    }

    bool onPasteConfirm(int vk) {
        if (pendingPaste.empty()) return false;
        if (isModifier(vk)) return true;
        std::string text;
        text.swap(pendingPaste);
        swallowChar = true;
        if (vk == VK_RETURN) runPaste(text);
        host.con.follow();
        return true;
    }

    void onKey(int vk, bool ctrl, bool shift, bool alt, unsigned state) {
        if (onPasteConfirm(vk)) return;
        if (ctrl && !shift && !alt && vk == 'W' && onCloseKey && idleAtPrompt()) { swallowChar = true; onCloseKey(); return; }
        if (zoomKey(vk, ctrl, alt, state)) return;
        if (ctrl && vk == 'A') { select(0, frame.rows * frame.cols - 1); return; }
        if (selectionKey(vk, ctrl)) return;
        if (ctrl && (vk == 'C' || vk == VK_INSERT)) {
            if (vk == 'C' && !shift) interruptShell();
            return;
        }
        if (vk == VK_CANCEL) { interruptShell(); return; }
        if (pageKey(vk, shift)) return;
        if (ctrl && (vk == VK_UP || vk == VK_DOWN)) { scrollView(vk == VK_UP ? -1 : 1); return; }
        if (onKeyForProgram(vk, ctrl)) return;
        if (onNumberKey(vk)) return;
        if (alt) {
            if (vk == VK_F7) { host.clearHistory(); historyPos = 0; }
            if (vk == VK_F10 && shell) shell->clearMacros();
            return;
        }
        if (vk != VK_F8) searchPos = SIZE_MAX;
        if (!editKey(vk, ctrl, shift)) return;
        edited(false);
    }

    bool editKey(int vk, bool ctrl, bool shift) {
        switch (vk) {
        case VK_TAB: line.complete(shift); edited(true); return false;
        case VK_BACK: if (ctrl) line.deleteWord(false); else line.backspace(); return true;
        case VK_DELETE: if (ctrl) line.deleteWord(true); else line.del(); return true;
        case VK_LEFT: line.pos = ctrl ? line.wordStart() : unicode::utf8Prev(line.text, line.pos); return true;
        case VK_RIGHT:
            if (ctrl) line.wordRight();
            else if (line.pos < line.text.size()) line.pos = unicode::utf8Next(line.text, line.pos);
            else line.copyFrom(lastCommand(), unicode::utf8Next(lastCommand(), line.pos));
            return true;
        case VK_HOME: if (ctrl) line.text.erase(0, line.pos); line.pos = 0; return true;
        case VK_END: if (ctrl) line.text.erase(line.pos); else line.pos = line.text.size(); return true;
        case VK_ESCAPE: line.clear(); return true;
        case VK_UP: case VK_F5: if (historyPos > 0) recallHistory(historyPos - 1); return true;
        case VK_DOWN:
            if (historyPos + 1 < host.historyCount()) recallHistory(historyPos + 1);
            else { historyPos = host.historyCount(); line.clear(); }
            return true;
        case VK_F1: line.copyFrom(lastCommand(), unicode::utf8Next(lastCommand(), line.pos)); return true;
        case VK_F2: case VK_F4: pendingKey = vk; return false;
        case VK_F3: line.copyFrom(lastCommand(), SIZE_MAX); return true;
        case VK_F6: typeText("\x1A"); return false;
        case VK_F7: listHistory(); return false;
        case VK_F8: searchHistory(); return false;
        case VK_F9: numberMode = true; numberDigits.clear(); host.con.follow(); return false;
        case 'Z': if (ctrl) { typeText("\x1A"); swallowChar = true; } return false;
        case 'V': if (ctrl) askClipboard(ClipboardUse::PasteText); return false;
        case VK_INSERT:
            if (shift) askClipboard(ClipboardUse::PasteText);
            else host.insertMode = !host.insertMode;
            return false;
        case VK_RETURN: submitInput(); return true;
        }
        return false;
    }
};
