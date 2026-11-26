#pragma once
#include <unordered_map>
#include "condrv.h"
#include "emulator.h"
#include "aliases.h"
#include "keyboard.h"

namespace ccc {

using KeyInput = keyboard::KeyInput;

inline std::wstring wideKeys(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }

class ConsoleServer {
public:
    HWND pseudoWindow = nullptr;
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
    ~ConsoleServer() { if (thread.joinable()) thread.detach(); }

    bool start(int cols, int rows) {
        inputEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!device.open(inputEvent)) return false;
        SetConsoleCtrlHandler(nullptr, FALSE);
        mainBuffer = newBuffer(cols, rows);
        active = mainBuffer;
        pseudo = { nullptr, device.reference, GetCurrentProcess() };
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([this, ready] { pumpWindow(ready); }).detach();
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
        thread = std::thread([this] { run(); });
        return true;
    }

    bool started() const { return device.server != nullptr; }

    bool launch(const std::wstring& exe, std::wstring& cmdline, PROCESS_INFORMATION& pi, DWORD& error) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<char> list(size);
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
        condrv::PseudoConsole* handle = &pseudo;
        UpdateProcThreadAttribute(si.lpAttributeList, 0, condrv::PROC_THREAD_ATTRIBUTE_CONSOLE, handle, sizeof(handle), nullptr, nullptr);
        BOOL ok = CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
        error = ok ? 0 : GetLastError();
        DeleteProcThreadAttributeList(si.lpAttributeList);
        return ok != FALSE;
    }

    void begin(int cols, int rows) {
        Console::Frame f;
        log.follow();
        log.layout(f, cols, rows, "");
        {
            std::lock_guard<std::mutex> l(m);
            for (Buffer* b : buffers) if (b != mainBuffer) b->emu.resize(cols, rows);
            Emulator& e = mainBuffer->emu;
            e.reset(cols, rows);
            configure(*mainBuffer);
            e.snapshot(f);
            e.logging = true;
            mainBuffer->mode = DEFAULT_OUTPUT_MODE;
            applyMode(*mainBuffer);
            inputMode = DEFAULT_INPUT_MODE;
            active = mainBuffer;
            live = true;
        }
        notify();
    }

    void end() {
        std::string text;
        bool clearProgress = false;
        {
            std::lock_guard<std::mutex> l(m);
            clearProgress = progressState != 0;
            progressState = progressValue = 0;
            progressDirty = false;
            Emulator& e = mainBuffer->emu;
            text = e.pendingText(e.lastUsedRow());
            e.logging = false;
            live = false;
            active = mainBuffer;
            editing = false;
            for (const INPUT_RECORD& r : queue)
                if (r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.uChar.UnicodeChar) typeahead += r.Event.KeyEvent.uChar.UnicodeChar;
            queue.clear();
            ResetEvent(inputEvent);
            line.clear();
            ready.clear();
        }
        if (!text.empty()) log.write(text);
        if (clearProgress && progress) progress(0, 0);
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
        Emulator& e = active->emu;
        if (e.sync) {
            ULONGLONG now = GetTickCount64();
            if (!syncHeld) { syncHeld = true; syncSince = now; }
            if (now - syncSince < 250) return false;
        }
        else syncHeld = false;
        int next = e.animate();
        e.render(f);
        f.nextFrameMs = next;
        return true;
    }

    int cursorStyle() const { std::lock_guard<std::mutex> l(m); return active->emu.cursorShape; }

    void resize(int cols, int rows) {
        bool sized = false;
        {
            std::lock_guard<std::mutex> l(m);
            if (!mainBuffer) return;
            const Emulator& me = mainBuffer->emu;
            if (cols == me.main.cols && rows == me.main.rows && cellW == me.cellW && cellH == me.cellH) return;
            for (Buffer* b : buffers) { b->emu.setCellSize(cellW, cellH); b->emu.resize(cols, rows); }
            if (repliesQueued) { repliesQueued = false; inputChanged(); }
            if (inputMode & ENABLE_WINDOW_INPUT) {
                INPUT_RECORD r = {};
                r.EventType = WINDOW_BUFFER_SIZE_EVENT;
                r.Event.WindowBufferSizeEvent.dwSize = { (SHORT)cols, (SHORT)rows };
                queue.push_back(r);
                inputChanged();
            }
            sized = true;
        }
        if (sized && pseudoWindow) NotifyWinEvent(EVENT_CONSOLE_LAYOUT, pseudoWindow, 0, 0);
        notify();
    }

    bool processedInput() const { std::lock_guard<std::mutex> l(m); return (inputMode & ENABLE_PROCESSED_INPUT) != 0; }
    bool altScreen() const { std::lock_guard<std::mutex> l(m); return active != mainBuffer || active->emu.altActive; }

    std::string screenText() const {
        std::lock_guard<std::mutex> l(m);
        const Emulator& e = mainBuffer->emu;
        if (!live || active != mainBuffer || e.altActive) return "";
        return e.pendingText(e.main.rows - 1);
    }

    std::string modes() const {
        std::lock_guard<std::mutex> l(m);
        const Emulator& e = active->emu;
        return "mouse=" + std::to_string(e.mouse) + " sgr=" + std::to_string(e.mouseSgr) + " alt=" + std::to_string(active != mainBuffer || e.altActive) +
               " vtinput=" + std::to_string((inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0) + " focus=" + std::to_string(e.focusEvents);
    }

    void key(const KeyInput& k) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live) {
                if (k.down && k.ch) typeahead += k.ch;
            }
            else {
                if (inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) {
                    if (active->emu.kittyFlags() & 9) pushChars(wideKeys(keyboard::kittyKey(k, active->emu.kittyFlags())));
                    else if (k.down) pushChars(wideKeys(keyboard::legacyKey(k, active->emu.appCursor, active->emu.modifyOtherKeys)));
                }
                else pushRecord(k);
                inputChanged();
            }
        }
        notify();
    }

    void paste(const std::string& utf8) {
        std::wstring w;
        for (size_t i = 0; i < utf8.size();) {
            uint32_t cp = unicode::decodeUtf8(utf8, i);
            if (cp == '\n' && !w.empty() && w.back() == '\r') continue;
            unicode::appendUtf16(w, cp == '\n' ? '\r' : cp);
        }
        {
            std::lock_guard<std::mutex> l(m);
            bool vt = (inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0;
            if (!live) typeahead += w;
            else if (vt) { if (!active->emu.announcePaste(utf8)) pushChars(active->emu.bracketedPaste ? L"\x1b[200~" + w + L"\x1b[201~" : w); }
            else for (wchar_t ch : w) {
                KeyInput k;
                k.ch = ch;
                k.vk = ch == '\r' ? VK_RETURN : 0;
                k.down = true;
                pushRecord(k);
                k.down = false;
                pushRecord(k);
            }
            if (live) inputChanged();
        }
        notify();
    }

    void answerClipboard(const std::string& text, bool granted) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live) return;
            active->emu.answerClipboard(text, granted);
            if (repliesQueued) { repliesQueued = false; inputChanged(); }
        }
        notify();
    }

    bool mouseWheel(int notches, int col, int row, int px, int py, DWORD keyState) {
        {
            std::lock_guard<std::mutex> l(m);
            const Emulator& e = active->emu;
            if (!live) return false;
            if (inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) {
                if (!e.mouse) return false;
                int button = notches > 0 ? 64 : 65;
                std::wstring seq;
                for (int i = 0; i < std::abs(notches); ++i) seq += mouseSeq(e, button, false, col, row, px, py);
                pushChars(seq);
            }
            else {
                if (!(inputMode & ENABLE_MOUSE_INPUT) || (inputMode & ENABLE_QUICK_EDIT_MODE)) return false;
                INPUT_RECORD r = {};
                r.EventType = MOUSE_EVENT;
                r.Event.MouseEvent.dwMousePosition = { (SHORT)col, (SHORT)row };
                r.Event.MouseEvent.dwButtonState = (DWORD)(WORD)(SHORT)(notches * WHEEL_DELTA) << 16;
                r.Event.MouseEvent.dwControlKeyState = keyState;
                r.Event.MouseEvent.dwEventFlags = MOUSE_WHEELED;
                queue.push_back(r);
            }
            inputChanged();
        }
        notify();
        return true;
    }

    bool mouseButton(int button, bool down, bool motion, int col, int row, int px, int py, DWORD keyState) {
        {
            std::lock_guard<std::mutex> l(m);
            const Emulator& e = active->emu;
            if (!live) return false;
            if (inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) {
                if (!e.mouse) return false;
                if (motion && e.mouse < (button == 3 ? 1003 : 1002)) return true;
                if (repeatedMotion(motion, e.mousePixels ? py * 65536 + px : row * 65536 + col)) return true;
                int mods = (keyState & SHIFT_PRESSED ? 4 : 0) | (keyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED) ? 8 : 0) |
                           (keyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED) ? 16 : 0);
                pushChars(mouseSeq(e, button + mods + (motion ? 32 : 0), !down && !motion, col, row, px, py));
            }
            else {
                if (!(inputMode & ENABLE_MOUSE_INPUT) || (inputMode & ENABLE_QUICK_EDIT_MODE)) return false;
                if (repeatedMotion(motion, row * 65536 + col)) return true;
                static const DWORD bits[4] = { FROM_LEFT_1ST_BUTTON_PRESSED, FROM_LEFT_2ND_BUTTON_PRESSED, RIGHTMOST_BUTTON_PRESSED, 0 };
                if (!motion) heldButtons = down ? heldButtons | bits[button] : heldButtons & ~bits[button];
                INPUT_RECORD r = {};
                r.EventType = MOUSE_EVENT;
                r.Event.MouseEvent.dwMousePosition = { (SHORT)col, (SHORT)row };
                r.Event.MouseEvent.dwButtonState = heldButtons;
                r.Event.MouseEvent.dwControlKeyState = keyState;
                r.Event.MouseEvent.dwEventFlags = motion ? MOUSE_MOVED : 0;
                queue.push_back(r);
            }
            inputChanged();
        }
        notify();
        return true;
    }

    bool repeatedMotion(bool motion, int at) {
        if (motion && at == lastMotion) return true;
        lastMotion = at;
        return false;
    }

    void focus(bool on) {
        {
            std::lock_guard<std::mutex> l(m);
            if (!live || !(inputMode & ENABLE_VIRTUAL_TERMINAL_INPUT) || !active->emu.focusEvents) return;
            pushChars(on ? L"\x1b[I" : L"\x1b[O");
            inputChanged();
        }
        notify();
    }

    void ctrlEvent(DWORD event) {
        std::vector<DWORD> pids;
        {
            std::lock_guard<std::mutex> l(m);
            for (Client* c : clients) pids.push_back(c->pid);
            cancelReads(event == CTRL_C_EVENT);
        }
        for (DWORD pid : pids) endTask(pid, event);
    }

    void shutdown() {
        std::vector<DWORD> pids;
        { std::lock_guard<std::mutex> l(m); for (Client* c : clients) pids.push_back(c->pid); }
        for (DWORD pid : pids) endTask(pid, CTRL_CLOSE_EVENT);
        for (int i = 0; i < 50 && !pids.empty(); ++i) {
            { std::lock_guard<std::mutex> l(m); if (clients.empty()) return; }
            Sleep(20);
        }
        std::lock_guard<std::mutex> l(m);
        for (Client* c : clients)
            if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, c->pid)) { TerminateProcess(p, 0xC000013A); CloseHandle(p); }
    }

private:
    static constexpr DWORD DEFAULT_OUTPUT_MODE = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    static constexpr DWORD DEFAULT_INPUT_MODE = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_INSERT_MODE |
                                                ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS | ENABLE_AUTO_POSITION;
    static constexpr USHORT READ_NOREMOVE = 1, READ_NOWAIT = 2;

    struct CpBody { ULONG codePage; BOOLEAN output; };
    struct GetInputBody { ULONG records; USHORT flags; BOOLEAN unicode; };
    struct ReadBody { BOOLEAN unicode, controlZ; USHORT exeLength; ULONG initialBytes, wakeupMask, controlKeyState, bytes; };
    struct WriteBody { ULONG bytes; BOOLEAN unicode; };
    struct FillBody { COORD at; ULONG type; USHORT element; ULONG length; };
    struct CtrlBody { ULONG event, group; };
    struct CursorInfoBody { ULONG size; BOOLEAN visible; };
    struct InfoBody { COORD size, cursor, scroll; USHORT attributes; COORD window, maxWindow; USHORT popup; BOOLEAN fullscreen; COLORREF colors[16]; };
    struct ScrollBody { SMALL_RECT scroll, clip; BOOLEAN useClip, unicode; COORD dest; CHAR_INFO fill; };
    struct StringBody { COORD at; ULONG type, count; };
    struct WriteInputBody { ULONG count; BOOLEAN unicode, append; };
    struct RegionBody { SMALL_RECT region; BOOLEAN unicode; };
    struct TitleBody { ULONG length; BOOLEAN unicode, original; };
    struct FontBody { BOOLEAN maximum; ULONG index; COORD size; ULONG family, weight; WCHAR face[LF_FACESIZE]; };
    struct FontSizeBody { ULONG index; COORD size; };
    struct LayoutBody { WCHAR name[9]; BOOLEAN ansi; };
    struct HistoryBody { ULONG size, buffers, flags; };
    struct AliasBody { USHORT source, target, exe; BOOLEAN unicode; };
    struct LengthBody { ULONG length; BOOLEAN unicode; };
    struct ListBody { BOOLEAN unicode; ULONG length; };
    struct ExpungeBody { BOOLEAN unicode; };

    enum StringType : ULONG { STRING_ASCII = 1, STRING_UNICODE, STRING_ATTRIBUTE, STRING_FALSE_UNICODE };

    struct Buffer {
        Emulator emu;
        DWORD mode = DEFAULT_OUTPUT_MODE;
        DWORD cursorSize = 25;
    };
    struct Client {
        DWORD pid = 0;
        ULONG group = 0;
    };
    struct Handle {
        bool input = false;
        Buffer* buffer = nullptr;
    };
    enum class ReadKind { CONSOLE, RAW, RECORDS };
    struct Pending {
        condrv::Message msg;
        ReadKind kind;
    };

    Console& log;
    condrv::Device device;
    condrv::PseudoConsole pseudo = {};
    HANDLE inputEvent = nullptr;
    std::thread thread;
    mutable std::mutex m;
    std::atomic<bool> live{ false };
    std::vector<Buffer*> buffers;
    Buffer* mainBuffer = nullptr;
    Buffer* active = nullptr;
    std::vector<Client*> clients;
    std::unordered_map<ULONG_PTR, std::unique_ptr<Handle>> handles;
    std::deque<INPUT_RECORD> queue;
    std::deque<Pending> pending;
    DWORD inputMode = DEFAULT_INPUT_MODE;
    DWORD heldButtons = 0;
    int lastMotion = -1;

    static std::wstring mouseSeq(const Emulator& e, int code, bool release, int col, int row, int px, int py) {
        if (e.mousePixels) { col = px; row = py; }
        if (e.mouseSgr || e.mousePixels) return L"\x1b[<" + std::to_wstring(code) + L";" + std::to_wstring(col + 1) + L";" + std::to_wstring(row + 1) + (release ? L"m" : L"M");
        std::wstring s = L"\x1b[M";
        s += (wchar_t)(32 + (release ? (code & ~3) | 3 : code));
        s += (wchar_t)(32 + (std::min)(col + 1, 223));
        s += (wchar_t)(32 + (std::min)(row + 1, 223));
        return s;
    }
    UINT inputPage = 0;
    std::wstring title = L"ccc", originalTitle = L"ccc";
    bool titleDirty = false, bellDirty = false, progressDirty = false;
    int progressState = 0, progressValue = 0;
    bool repliesQueued = false;
    std::vector<char> scratch;
    std::string clipText;
    bool clipDirty = false;
    bool syncHeld = false;
    std::vector<std::function<void()>> outbox;
    ULONGLONG syncSince = 0;

    std::wstring line, ready, typeahead;
    size_t linePos = 0;
    bool editing = false;
    int echoStart = 0, echoWidth = 0;
    ConsoleAliases aliases;
    CommandHistories histories;
    std::wstring readExe;
    size_t historyPos = 0;

    condrv::Message msg = {};
    condrv::Completion reply = {};
    condrv::Connection connection = {};

    void notify() { if (changed) changed(); }

    void pumpWindow(HANDLE ready) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ccc.console";
        RegisterClassExW(&wc);
        pseudoWindow = CreateWindowExW(0, wc.lpszClassName, L"ccc", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        SetEvent(ready);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    }

    Buffer* newBuffer(int cols, int rows) {
        Buffer* b = new Buffer;
        b->emu.reset(cols, rows);
        configure(*b);
        applyMode(*b);
        buffers.push_back(b);
        return b;
    }

    void configure(Buffer& b) {
        Emulator& e = b.emu;
        e.onReply = [this](const std::string& s) {
            std::wstring w;
            for (char ch : s) w += (wchar_t)(unsigned char)ch;
            pushChars(w);
            repliesQueued = true;
        };
        e.onTitle = [this](const std::wstring& t) { title = t; titleDirty = true; };
        e.onClipboard = [this](const std::string& s) { clipText = s; clipDirty = true; };
        e.onBell = [this] { bellDirty = true; };
        e.onProgress = [this](int state, int value) { progressState = state; progressValue = value; progressDirty = true; };
        e.onCwd = [this](const std::string& p) { outbox.push_back([this, p] { if (cwd) cwd(p); }); };
        e.onNotify = [this](const std::string& t, const std::string& b) { outbox.push_back([this, t, b] { if (notification) notification(t, b); }); };
        e.onClipboardRead = [this] { outbox.push_back([this] { if (clipboardRequest) clipboardRequest(); }); };
        e.cellW = cellW;
        e.cellH = cellH;
        if (&b == mainBuffer || !mainBuffer) {
            e.onScrollOut = [this](const char* p, size_t n) { log.write(p, n); };
            e.onClear = [this] { log.clear(); };
        }
    }

    void applyMode(Buffer& b) {
        b.emu.wrapMode = (b.mode & ENABLE_WRAP_AT_EOL_OUTPUT) != 0;
        b.emu.newlineReturns = !(b.mode & DISABLE_NEWLINE_AUTO_RETURN);
    }

    UINT page() const { return unicode::byteCodePage(); }
    UINT readPage() const { return inputPage ? inputPage : page(); }

    static void endTask(DWORD pid, DWORD event) {
        using Control = LONG(WINAPI*)(int, void*, DWORD);
        static Control control = reinterpret_cast<Control>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "ConsoleControl"));
        if (!control) return;
        struct { HANDLE pid; HWND hwnd; ULONG event, flags; } task = { ULongToHandle(pid), nullptr, event,
                                                                           event == CTRL_C_EVENT ? 1u : event == CTRL_BREAK_EVENT ? 2u : 4u };
        control(7, &task, sizeof(task));
    }

    void inputChanged() {
        service();
        if (queue.empty()) ResetEvent(inputEvent);
        else SetEvent(inputEvent);
    }

    void pushRecord(const KeyInput& k) {
        INPUT_RECORD r = {};
        r.EventType = KEY_EVENT;
        r.Event.KeyEvent.bKeyDown = k.down;
        r.Event.KeyEvent.wRepeatCount = 1;
        r.Event.KeyEvent.wVirtualKeyCode = k.vk;
        r.Event.KeyEvent.wVirtualScanCode = k.scan;
        r.Event.KeyEvent.uChar.UnicodeChar = k.ch;
        r.Event.KeyEvent.dwControlKeyState = k.state;
        queue.push_back(r);
    }

    void pushChars(const std::wstring& s) {
        for (wchar_t ch : s) {
            KeyInput k;
            k.ch = ch;
            k.down = true;
            pushRecord(k);
        }
    }

    void run() {
        bool haveReply = false;
        for (;;) {
            if (!device.read(haveReply ? &reply : nullptr, msg)) { haveReply = false; continue; }
            std::wstring newTitle;
            std::string newClip;
            bool sendTitle = false, sendClip = false, ring = false, sendProgress = false;
            int pState = 0, pValue = 0;
            bool wrote = false;
            std::vector<std::function<void()>> events;
            {
                std::lock_guard<std::mutex> l(m);
                reply = {};
                reply.id = msg.d.id;
                haveReply = dispatch(wrote);
                if (repliesQueued) { repliesQueued = false; inputChanged(); }
                if (titleDirty) { titleDirty = false; newTitle = title; sendTitle = true; }
                if (clipDirty) { clipDirty = false; newClip = std::move(clipText); sendClip = true; }
                ring = std::exchange(bellDirty, false);
                if (std::exchange(progressDirty, false)) { sendProgress = true; pState = progressState; pValue = progressValue; }
                events.swap(outbox);
            }
            for (auto& event : events) event();
            if (wrote) notify();
            if (sendTitle && titleChanged) titleChanged(newTitle);
            if (sendClip && clipboard) clipboard(newClip);
            if (ring && bell) bell();
            if (sendProgress && progress) progress(pState, pValue);
        }
    }

    Handle* handleOf(ULONG_PTR id) {
        auto it = handles.find(id);
        return it == handles.end() ? nullptr : it->second.get();
    }

    Buffer* outputOf(const condrv::Message& mm) {
        Handle* h = handleOf(mm.d.object);
        return h && !h->input ? h->buffer : nullptr;
    }

    bool inputOf(const condrv::Message& mm) {
        Handle* h = handleOf(mm.d.object);
        return h && h->input;
    }

    ULONG_PTR addHandle(bool input, Buffer* b) {
        auto h = std::make_unique<Handle>();
        h->input = input;
        h->buffer = b;
        ULONG_PTR id = reinterpret_cast<ULONG_PTR>(h.get());
        handles.emplace(id, std::move(h));
        return id;
    }

    void status(NTSTATUS s, ULONG_PTR info = 0) {
        reply.status.Status = s;
        reply.status.Information = info;
    }

    void bodyReply() { reply.write = { msg.body, msg.apiSize, 0 }; }

    bool dispatch(bool& wrote) {
        switch (msg.d.function) {
        case condrv::CONNECT: return connect();
        case condrv::DISCONNECT: disconnect(); status(condrv::SUCCESS); return true;
        case condrv::CREATE_OBJECT: return createObject();
        case condrv::CLOSE_OBJECT: closeObject(); status(condrv::SUCCESS); return true;
        case condrv::RAW_WRITE: {
            Buffer* b = outputOf(msg);
            if (!b) { status(condrv::INVALID_HANDLE); return true; }
            ULONG n = msg.d.inputSize;
            scratch.resize(n);
            device.input(msg, 0, scratch.data(), n);
            write(*b, scratch.data(), n, false);
            wrote = true;
            status(condrv::SUCCESS, n);
            return true;
        }
        case condrv::RAW_READ:
            if (!inputOf(msg)) { status(condrv::INVALID_HANDLE); return true; }
            return readOrPend(ReadKind::RAW);
        case condrv::RAW_FLUSH: queue.clear(); inputChanged(); status(condrv::SUCCESS); return true;
        case condrv::USER_DEFINED:
            if (msg.d.inputSize < 8 || msg.apiSize > condrv::BODY_BYTES || msg.apiSize > msg.d.inputSize - 8) { status(condrv::ILLEGAL_FUNCTION); return true; }
            bodyReply();
            status(condrv::SUCCESS);
            return api(wrote);
        }
        status(condrv::UNSUCCESSFUL);
        return true;
    }

    bool connect() {
        condrv::ConnectData data = {};
        device.input(msg, 0, &data, sizeof(data));
        Client* c = new Client;
        c->pid = (DWORD)msg.d.process;
        c->group = data.processGroup;
        clients.push_back(c);
        connection = { reinterpret_cast<ULONG_PTR>(c), addHandle(true, nullptr), addHandle(false, active) };
        status(condrv::SUCCESS, sizeof(connection));
        reply.write = { &connection, sizeof(connection), 0 };
        return true;
    }

    void disconnect() {
        Client* c = reinterpret_cast<Client*>(msg.d.process);
        auto it = std::find(clients.begin(), clients.end(), c);
        if (it == clients.end()) return;
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& p) { return p.msg.d.process == msg.d.process; }), pending.end());
        clients.erase(it);
        delete c;
    }

    bool createObject() {
        condrv::CreateObject info = msg.create;
        if (info.type == condrv::GENERIC) {
            DWORD rw = info.access & (GENERIC_READ | GENERIC_WRITE);
            info.type = rw == GENERIC_READ ? condrv::CURRENT_INPUT : rw == GENERIC_WRITE ? condrv::CURRENT_OUTPUT : 0;
        }
        ULONG_PTR id;
        switch (info.type) {
        case condrv::CURRENT_INPUT: id = addHandle(true, nullptr); break;
        case condrv::CURRENT_OUTPUT: id = addHandle(false, active); break;
        case condrv::NEW_OUTPUT: id = addHandle(false, newBuffer(mainBuffer->emu.cols(), mainBuffer->emu.rows())); break;
        default: status(condrv::INVALID_PARAMETER); return true;
        }
        status(condrv::SUCCESS, id);
        return true;
    }

    void closeObject() {
        auto it = handles.find(msg.d.object);
        if (it == handles.end()) return;
        Buffer* b = it->second->buffer;
        handles.erase(it);
        if (!b || b == mainBuffer) return;
        for (auto& h : handles) if (h.second->buffer == b) return;
        if (active == b) active = mainBuffer;
        buffers.erase(std::find(buffers.begin(), buffers.end(), b));
        delete b;
    }

    void write(Buffer& b, const char* p, size_t n, bool wide) {
        if (&b == mainBuffer && !live) {
            if (!wide) { log.write(p, n); return; }
            int k = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(p), (int)(n / 2), nullptr, 0, nullptr, nullptr);
            std::string s(k, '\0');
            WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(p), (int)(n / 2), s.data(), k, nullptr, nullptr);
            log.write(s);
            return;
        }
        if (wide) b.emu.writeWide(reinterpret_cast<const char16_t*>(p), n / 2);
        else b.emu.writeBytes(p, n, page());
        if (editing && &b == active) echoStart = linear(b.emu) - echoWidth;
    }

    static vt::Attr fromLegacy(WORD a) {
        vt::Attr r;
        WORD fg = a & 15, bg = a >> 4 & 15;
        r.fg = fg == 7 ? vt::COLOR_DEFAULT : vt::COLOR_INDEXED | swapRB(fg);
        r.bg = bg == 0 ? vt::COLOR_DEFAULT : vt::COLOR_INDEXED | swapRB(bg);
        if (a & COMMON_LVB_REVERSE_VIDEO) r.flags |= vt::REVERSE;
        if (a & COMMON_LVB_UNDERSCORE) r.flags |= vt::UNDERLINE;
        return r;
    }

    static const uint32_t* palette() {
        static const uint32_t ansi[16] = { 0x0C0C0C, 0x1F0FC5, 0x0EA113, 0x009CC1, 0xDA3700, 0x981788, 0xDD963A, 0xCCCCCC,
                                           0x767676, 0x5648E7, 0x0CC616, 0xA5F1F9, 0xFF783B, 0x9E00B4, 0xD6D661, 0xF2F2F2 };
        return ansi;
    }

    static WORD nearestIndex(uint32_t c, bool bright) {
        if (c & vt::COLOR_INDEXED) {
            uint32_t i = c & 255;
            if (i < 16) return (WORD)(bright && i < 8 ? i + 8 : i);
            if (i >= 232) { uint32_t g = 8 + (i - 232) * 10; c = g | g << 8 | g << 16; }
            else { static const uint32_t level[6] = { 0, 95, 135, 175, 215, 255 }; i -= 16; c = level[i / 36] | level[i / 6 % 6] << 8 | level[i % 6] << 16; }
        }
        int best = 0;
        long bestD = LONG_MAX;
        for (int i = 0; i < 16; ++i) {
            uint32_t p = palette()[i];
            long dr = (long)(c & 255) - (long)(p & 255), dg = (long)(c >> 8 & 255) - (long)(p >> 8 & 255), db = (long)(c >> 16 & 255) - (long)(p >> 16 & 255);
            long d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return (WORD)best;
    }

    static WORD toLegacy(const vt::Attr& a) {
        WORD fg = (a.fg & vt::COLOR_DEFAULT) ? 7 : (WORD)swapRB(nearestIndex(a.fg, (a.flags & vt::BOLD) != 0));
        WORD bg = (a.bg & vt::COLOR_DEFAULT) ? 0 : (WORD)swapRB(nearestIndex(a.bg, false));
        WORD r = (WORD)(fg | bg << 4);
        if (a.flags & vt::REVERSE) r |= COMMON_LVB_REVERSE_VIDEO;
        if (a.flags & vt::UNDERLINE) r |= COMMON_LVB_UNDERSCORE;
        return r;
    }

    static int swapRB(int c) { return ((c & 1) << 2) | (c & 2) | ((c & 4) >> 2) | (c & 8); }

    static int linear(const Emulator& e) { return e.cur.y * e.cols() + e.cur.x + (e.cur.pending ? 1 : 0); }

    uint32_t cellChar(const Emulator& e, int x, int y) const {
        uint32_t ch = e.g->row(y)[x].ch;
        if (ch == Console::WIDE_TAIL) return ' ';
        if (ch >= Console::CLUSTER_BASE) {
            size_t k = ch - Console::CLUSTER_BASE;
            return k < e.clusters.size() && !e.clusters[k].empty() ? e.clusters[k][0] : ' ';
        }
        return ch;
    }

    static wchar_t narrowToWide(char c) {
        return (wchar_t)unicode::oemByte((unsigned char)c);
    }

    char wideToNarrow(uint32_t cp) const {
        if (cp < 0x80) return (char)cp;
        wchar_t w = (wchar_t)(cp < 0x10000 ? cp : '?');
        char out[4];
        int k = WideCharToMultiByte(page() == CP_UTF8 ? GetOEMCP() : page(), 0, &w, 1, out, sizeof(out), nullptr, nullptr);
        return k == 1 ? out[0] : '?';
    }

    bool api(bool& wrote) {
        switch (msg.layer()) {
        case 1: return layer1(wrote);
        case 2: return layer2(wrote);
        case 3: return layer3();
        }
        status(condrv::ILLEGAL_FUNCTION);
        return true;
    }

    bool layer1(bool& wrote) {
        switch (msg.call()) {
        case 0: { auto& b = msg.as<CpBody>(); b.codePage = b.output ? page() : readPage(); return true; }
        case 1: {
            Handle* h = handleOf(msg.d.object);
            if (!h) { status(condrv::INVALID_HANDLE); return true; }
            msg.as<ULONG>() = h->input ? inputMode : h->buffer->mode;
            return true;
        }
        case 2: {
            Handle* h = handleOf(msg.d.object);
            if (!h) { status(condrv::INVALID_HANDLE); return true; }
            ULONG mode = msg.as<ULONG>();
            if (h->input) { inputMode = mode; inputChanged(); }
            else { h->buffer->mode = mode; applyMode(*h->buffer); }
            return true;
        }
        case 3: if (!inputOf(msg)) { status(condrv::INVALID_HANDLE); return true; } msg.as<ULONG>() = (ULONG)queue.size(); return true;
        case 4: if (!inputOf(msg)) { status(condrv::INVALID_HANDLE); return true; } return readOrPend(ReadKind::RECORDS);
        case 5: if (!inputOf(msg)) { status(condrv::INVALID_HANDLE); return true; } return readOrPend(ReadKind::CONSOLE);
        case 6: {
            Buffer* b = outputOf(msg);
            if (!b) { status(condrv::INVALID_HANDLE); return true; }
            auto& body = msg.as<WriteBody>();
            ULONG n = msg.userInput();
            scratch.resize(n);
            device.input(msg, msg.apiSize + 8, scratch.data(), n);
            write(*b, scratch.data(), n, body.unicode != 0);
            wrote = true;
            body.bytes = n;
            status(condrv::SUCCESS, n);
            return true;
        }
        case 7: return true;
        case 8: status(condrv::NOT_SUPPORTED); return true;
        }
        status(condrv::NOT_IMPLEMENTED);
        return true;
    }

    bool layer2(bool& wrote) {
        ULONG call = msg.call();
        if (call == 1) {
            auto& b = msg.as<CtrlBody>();
            std::vector<DWORD> pids;
            for (Client* c : clients) if (b.group == 0 || c->group == b.group || c->pid == b.group) pids.push_back(c->pid);
            for (DWORD pid : pids) endTask(pid, b.event);
            return true;
        }
        if (call == 3) { queue.clear(); inputChanged(); return true; }
        if (call == 4) {
            auto& b = msg.as<CpBody>();
            CPINFO info;
            if (b.codePage != CP_UTF8 && !GetCPInfo(b.codePage, &info)) { status(condrv::INVALID_PARAMETER); return true; }
            if (b.output) unicode::setByteCodePage(b.codePage);
            else inputPage = b.codePage;
            return true;
        }
        if (call == 16) return writeInput();
        if (call == 20) return getTitle();
        if (call == 21) return setTitle();
        Buffer* b = outputOf(msg);
        if (!b) { status(condrv::INVALID_HANDLE); return true; }
        Emulator& e = b->emu;
        wrote = true;
        switch (call) {
        case 0: return fill(e);
        case 2: active = b; return true;
        case 5: { auto& c = msg.as<CursorInfoBody>(); c.size = b->cursorSize; c.visible = e.cursorVisible; return true; }
        case 6: { auto& c = msg.as<CursorInfoBody>(); b->cursorSize = c.size; e.cursorVisible = c.visible != 0; return true; }
        case 7: {
            auto& i = msg.as<InfoBody>();
            i.size = { (SHORT)e.cols(), (SHORT)e.rows() };
            i.cursor = { (SHORT)e.cur.x, (SHORT)e.cur.y };
            i.scroll = { 0, 0 };
            i.attributes = toLegacy(e.cur.attr);
            i.window = { (SHORT)e.cols(), (SHORT)e.rows() };
            i.maxWindow = { (SHORT)e.cols(), (SHORT)e.rows() };
            i.popup = 0xF5;
            i.fullscreen = FALSE;
            for (int k = 0; k < 16; ++k) i.colors[k] = e.colors.table[swapRB(k)];
            return true;
        }
        case 8: {
            auto& i = msg.as<InfoBody>();
            e.cur.attr = fromLegacy(i.attributes);
            for (int k = 0; k < 16; ++k) e.colors.table[swapRB(k)] = i.colors[k] & 0xFFFFFF;
            e.colors.changed = e.colors.table != palette::standardTable();
            return true;
        }
        case 9: case 14: return true;
        case 10: {
            COORD c = msg.as<COORD>();
            if (c.X < 0 || c.Y < 0 || c.X >= e.cols() || c.Y >= e.rows()) { status(condrv::INVALID_PARAMETER); return true; }
            e.setCursor(c.X, c.Y);
            return true;
        }
        case 11: msg.as<COORD>() = { (SHORT)(std::max)(e.cols(), 512), (SHORT)(std::max)(e.rows(), 256) }; return true;
        case 12: return scroll(e);
        case 13: e.cur.attr = fromLegacy(msg.as<USHORT>()); return true;
        case 15: return readString(e);
        case 17: return writeCells(e);
        case 18: return writeString(e);
        case 19: return readCells(e);
        }
        status(condrv::NOT_IMPLEMENTED);
        return true;
    }

    bool layer3() {
        ULONG call = msg.call();
        switch (call) {
        case 0: msg.as<ULONG>() = 1; return true;
        case 1: msg.as<ULONG>() = 3; return true;
        case 3: { auto& f = msg.as<FontSizeBody>(); f.size = { (SHORT)cellW.load(), (SHORT)cellH.load() }; return true; }
        case 4: {
            auto& f = msg.as<FontBody>();
            f.index = 0;
            f.size = { (SHORT)cellW.load(), (SHORT)cellH.load() };
            f.family = FF_MODERN | FIXED_PITCH;
            f.weight = FW_NORMAL;
            wcsncpy(f.face, L"Cascadia Mono", LF_FACESIZE);
            return true;
        }
        case 17: msg.as<ULONG>() = 0; return true;
        case 18: case 19: return alias(call);
        case 20: case 21: case 22: case 23: return aliasList(call);
        case 24: case 25: case 26: case 27: return commandHistory(call);
        case 30: {
            auto& k = msg.as<LayoutBody>();
            wchar_t name[KL_NAMELENGTH] = {};
            GetKeyboardLayoutNameW(name);
            if (k.ansi) { char* a = reinterpret_cast<char*>(k.name); for (int i = 0; i < 9; ++i) a[i] = (char)name[i]; }
            else memcpy(k.name, name, sizeof(k.name));
            return true;
        }
        case 31: msg.as<ULONG64>() = (ULONG64)(ULONG_PTR)pseudoWindow; return true;
        case 35: msg.body[0] = TRUE; msg.body[1] = FALSE; return true;
        case 40: memset(msg.body, 0, sizeof(CONSOLE_SELECTION_INFO)); return true;
        case 41: {
            ULONG cap = (msg.d.outputSize - msg.apiSize) / sizeof(DWORD);
            std::vector<DWORD> pids;
            for (auto it = clients.rbegin(); it != clients.rend(); ++it) pids.push_back((*it)->pid);
            msg.as<ULONG>() = (ULONG)pids.size();
            if (pids.size() <= cap) device.output(msg, msg.apiSize, pids.data(), (ULONG)(pids.size() * sizeof(DWORD)));
            return true;
        }
        case 42: { auto& h = msg.as<HistoryBody>(); h.size = 50; h.buffers = 4; h.flags = 0; return true; }
        }
        return true;
    }

    std::wstring callString(ULONG offset, ULONG bytes, bool unicode) {
        std::string raw(bytes, '\0');
        device.input(msg, msg.apiSize + 8 + offset, raw.data(), bytes);
        if (unicode) return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), bytes / 2);
        std::wstring w(raw.size(), L'\0');
        w.resize((size_t)MultiByteToWideChar(page(), 0, raw.data(), (int)raw.size(), w.data(), (int)w.size()));
        return w;
    }

    std::string callBytes(const std::wstring& s, bool unicode) {
        if (unicode) return std::string(reinterpret_cast<const char*>(s.data()), s.size() * 2);
        int n = WideCharToMultiByte(page(), 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out((size_t)n, '\0');
        WideCharToMultiByte(page(), 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    bool answerBytes(const std::string& bytes, ULONG& written) {
        ULONG cap = msg.d.outputSize - msg.apiSize;
        if (bytes.size() > cap) { written = 0; status(condrv::BUFFER_TOO_SMALL); return true; }
        device.output(msg, msg.apiSize, bytes.data(), (ULONG)bytes.size());
        written = (ULONG)bytes.size();
        reply.status.Information = written;
        return true;
    }

    bool alias(ULONG call) {
        auto& a = msg.as<AliasBody>();
        bool unicode = a.unicode != 0;
        std::wstring exe = callString(0, a.exe, unicode), source = callString(a.exe, a.source, unicode);
        if (call == 18) { aliases.add(exe, source, callString((ULONG)a.exe + a.source, a.target, unicode)); return true; }
        std::wstring target;
        if (!aliases.find(exe, source, target)) { a.target = 0; status(condrv::UNSUCCESSFUL); return true; }
        ULONG written = 0;
        answerBytes(callBytes(target + L'\0', unicode), written);
        a.target = (USHORT)written;
        return true;
    }

    bool aliasList(ULONG call) {
        if (call == 20 || call == 21) {
            auto& l = msg.as<LengthBody>();
            l.length = (ULONG)callBytes(call == 20 ? aliases.list(callString(0, msg.userInput(), l.unicode != 0)) : aliases.exes(), l.unicode != 0).size();
            return true;
        }
        if (call == 22) {
            auto& l = msg.as<ListBody>();
            return answerBytes(callBytes(aliases.list(callString(0, msg.userInput(), l.unicode != 0)), l.unicode != 0), l.length);
        }
        auto& l = msg.as<LengthBody>();
        return answerBytes(callBytes(aliases.exes(), l.unicode != 0), l.length);
    }

    bool commandHistory(ULONG call) {
        if (call == 24) { auto& e = msg.as<ExpungeBody>(); histories.expunge(callString(0, msg.userInput(), e.unicode != 0)); return true; }
        auto& l = msg.as<LengthBody>();
        std::wstring exe = callString(0, msg.userInput(), l.unicode != 0);
        if (call == 25) { histories.limit = (std::max)(l.length, 1ul); return true; }
        std::string bytes = callBytes(histories.joined(exe), l.unicode != 0);
        if (call == 26) { l.length = (ULONG)bytes.size(); return true; }
        return answerBytes(bytes, l.length);
    }

    bool fill(Emulator& e) {
        auto& f = msg.as<FillBody>();
        int x = f.at.X, y = f.at.Y, cols = e.cols(), rows = e.rows();
        ULONG done = 0;
        if (x < 0 || y < 0 || x >= cols || y >= rows) { f.length = 0; status(condrv::INVALID_PARAMETER); return true; }
        vt::Attr a = fromLegacy(f.element);
        uint32_t ch = f.type == STRING_ASCII ? unicode::oemByte((unsigned char)f.element) : f.element;
        for (; done < f.length && y < rows; ++done) {
            e.touched(y, x);
            Cell& c = e.g->row(y)[x];
            if (f.type == STRING_ATTRIBUTE) c.attr = a;
            else c.ch = ch;
            if (++x == cols) { x = 0; ++y; }
        }
        f.length = done;
        return true;
    }

    bool scroll(Emulator& e) {
        auto& s = msg.as<ScrollBody>();
        int cols = e.cols(), rows = e.rows();
        auto clipTo = [](SMALL_RECT r, SMALL_RECT c) {
            r.Left = (std::max)(r.Left, c.Left); r.Top = (std::max)(r.Top, c.Top);
            r.Right = (std::min)(r.Right, c.Right); r.Bottom = (std::min)(r.Bottom, c.Bottom);
            return r;
        };
        SMALL_RECT bounds = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
        SMALL_RECT src = clipTo(s.scroll, bounds);
        SMALL_RECT clip = s.useClip ? clipTo(s.clip, bounds) : bounds;
        if (src.Left > src.Right || src.Top > src.Bottom) return true;
        int w = src.Right - src.Left + 1, h = src.Bottom - src.Top + 1;
        std::vector<Cell> copy((size_t)w * h);
        for (int y = 0; y < h; ++y) std::copy(e.g->row(src.Top + y) + src.Left, e.g->row(src.Top + y) + src.Left + w, copy.begin() + (size_t)y * w);
        Cell fillCell = { s.unicode ? (uint32_t)s.fill.Char.UnicodeChar : unicode::oemByte((unsigned char)s.fill.Char.AsciiChar), fromLegacy(s.fill.Attributes) };
        auto inside = [](int x, int y, const SMALL_RECT& r) { return x >= r.Left && x <= r.Right && y >= r.Top && y <= r.Bottom; };
        SMALL_RECT dst = { s.dest.X, s.dest.Y, (SHORT)(s.dest.X + w - 1), (SHORT)(s.dest.Y + h - 1) };
        for (int y = src.Top; y <= src.Bottom; ++y)
            for (int x = src.Left; x <= src.Right; ++x)
                if (inside(x, y, clip) && !inside(x, y, dst)) { e.touched(y, x); e.g->row(y)[x] = fillCell; }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int tx = s.dest.X + x, ty = s.dest.Y + y;
                if (inside(tx, ty, clip) && inside(tx, ty, bounds)) { e.touched(ty, tx); e.g->row(ty)[tx] = copy[(size_t)y * w + x]; }
            }
        return true;
    }

    bool writeCells(Emulator& e) {
        auto& r = msg.as<RegionBody>();
        SMALL_RECT region = r.region;
        int w = region.Right - region.Left + 1, h = region.Bottom - region.Top + 1;
        if (w <= 0 || h <= 0) return true;
        std::vector<CHAR_INFO> data((size_t)w * h);
        ULONG bytes = (std::min)((ULONG)(data.size() * sizeof(CHAR_INFO)), msg.userInput());
        device.input(msg, msg.apiSize + 8, data.data(), bytes);
        int cols = e.cols(), rows = e.rows();
        SMALL_RECT written = { (SHORT)(std::max)((int)region.Left, 0), (SHORT)(std::max)((int)region.Top, 0),
                               (SHORT)(std::min)((int)region.Right, cols - 1), (SHORT)(std::min)((int)region.Bottom, rows - 1) };
        for (int y = written.Top; y <= written.Bottom; ++y)
            for (int x = written.Left; x <= written.Right; ++x) {
                const CHAR_INFO& ci = data[(size_t)(y - region.Top) * w + (x - region.Left)];
                uint32_t ch = r.unicode ? (uint32_t)ci.Char.UnicodeChar : unicode::oemByte((unsigned char)ci.Char.AsciiChar);
                if (ci.Attributes & COMMON_LVB_TRAILING_BYTE) ch = Console::WIDE_TAIL;
                e.touched(y, x);
                e.g->row(y)[x] = { ch, fromLegacy(ci.Attributes & ~(COMMON_LVB_LEADING_BYTE | COMMON_LVB_TRAILING_BYTE)) };
            }
        r.region = written.Left > written.Right || written.Top > written.Bottom ? SMALL_RECT{ region.Left, region.Top, (SHORT)(region.Left - 1), (SHORT)(region.Top - 1) } : written;
        return true;
    }

    bool readCells(Emulator& e) {
        auto& r = msg.as<RegionBody>();
        SMALL_RECT region = r.region;
        int cols = e.cols(), rows = e.rows();
        SMALL_RECT got = { (SHORT)(std::max)((int)region.Left, 0), (SHORT)(std::max)((int)region.Top, 0),
                           (SHORT)(std::min)((int)region.Right, cols - 1), (SHORT)(std::min)((int)region.Bottom, rows - 1) };
        if (got.Left > got.Right || got.Top > got.Bottom) { r.region = { region.Left, region.Top, (SHORT)(region.Left - 1), (SHORT)(region.Top - 1) }; return true; }
        int w = got.Right - got.Left + 1, h = got.Bottom - got.Top + 1;
        std::vector<CHAR_INFO> data((size_t)w * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Cell& c = e.g->row(got.Top + y)[got.Left + x];
                CHAR_INFO& ci = data[(size_t)y * w + x];
                uint32_t ch = cellChar(e, got.Left + x, got.Top + y);
                if (r.unicode) ci.Char.UnicodeChar = (WCHAR)(ch < 0x10000 ? ch : 0xFFFD);
                else ci.Char.AsciiChar = wideToNarrow(ch);
                ci.Attributes = toLegacy(c.attr);
                if (c.ch == Console::WIDE_TAIL) ci.Attributes |= COMMON_LVB_TRAILING_BYTE;
                else if (got.Left + x + 1 < cols && e.g->row(got.Top + y)[got.Left + x + 1].ch == Console::WIDE_TAIL) ci.Attributes |= COMMON_LVB_LEADING_BYTE;
            }
        ULONG cap = msg.d.outputSize - msg.apiSize;
        ULONG bytes = (std::min)((ULONG)(data.size() * sizeof(CHAR_INFO)), cap);
        device.output(msg, msg.apiSize, data.data(), bytes);
        r.region = got;
        return true;
    }

    bool writeString(Emulator& e) {
        auto& s = msg.as<StringBody>();
        ULONG n = msg.userInput();
        scratch.resize(n);
        device.input(msg, msg.apiSize + 8, scratch.data(), n);
        int x = s.at.X, y = s.at.Y, cols = e.cols(), rows = e.rows();
        if (x < 0 || y < 0 || x >= cols || y >= rows) { s.count = 0; status(condrv::INVALID_PARAMETER); return true; }
        ULONG count = s.type == STRING_ASCII ? n : n / 2, done = 0;
        const WORD* words = reinterpret_cast<const WORD*>(scratch.data());
        for (; done < count && y < rows; ++done) {
            e.touched(y, x);
            Cell& c = e.g->row(y)[x];
            if (s.type == STRING_ATTRIBUTE) c.attr = fromLegacy(words[done]);
            else if (s.type == STRING_ASCII) c.ch = unicode::oemByte((unsigned char)scratch[done]);
            else c.ch = words[done];
            if (++x == cols) { x = 0; ++y; }
        }
        s.count = done;
        return true;
    }

    bool readString(Emulator& e) {
        auto& s = msg.as<StringBody>();
        int x = s.at.X, y = s.at.Y, cols = e.cols(), rows = e.rows();
        ULONG cap = msg.d.outputSize - msg.apiSize;
        ULONG unit = s.type == STRING_ASCII ? 1 : 2, count = cap / unit, done = 0;
        std::vector<char> out(cap);
        if (x >= 0 && y >= 0 && x < cols && y < rows) {
            for (; done < count && y < rows; ++done) {
                const Cell& c = e.g->row(y)[x];
                if (s.type == STRING_ATTRIBUTE) reinterpret_cast<WORD*>(out.data())[done] = toLegacy(c.attr);
                else if (s.type == STRING_ASCII) out[done] = wideToNarrow(cellChar(e, x, y));
                else { uint32_t ch = cellChar(e, x, y); reinterpret_cast<WORD*>(out.data())[done] = (WORD)(ch < 0x10000 ? ch : 0xFFFD); }
                if (++x == cols) { x = 0; ++y; }
            }
        }
        device.output(msg, msg.apiSize, out.data(), done * unit);
        s.count = done;
        reply.status.Information = done * unit;
        return true;
    }

    bool writeInput() {
        auto& b = msg.as<WriteInputBody>();
        ULONG n = msg.userInput() / sizeof(INPUT_RECORD);
        std::vector<INPUT_RECORD> records(n);
        device.input(msg, msg.apiSize + 8, records.data(), n * sizeof(INPUT_RECORD));
        if (!b.unicode)
            for (auto& r : records)
                if (r.EventType == KEY_EVENT) r.Event.KeyEvent.uChar.UnicodeChar = narrowToWide(r.Event.KeyEvent.uChar.AsciiChar);
        if (b.append) queue.insert(queue.end(), records.begin(), records.end());
        else queue.insert(queue.begin(), records.begin(), records.end());
        b.count = n;
        inputChanged();
        return true;
    }

    bool getTitle() {
        auto& t = msg.as<TitleBody>();
        const std::wstring& s = t.original ? originalTitle : title;
        ULONG cap = msg.d.outputSize - msg.apiSize;
        if (t.unicode) {
            ULONG n = (std::min)((ULONG)s.size(), cap / 2 ? cap / 2 - 1 : 0);
            std::wstring out = s.substr(0, n);
            out += L'\0';
            device.output(msg, msg.apiSize, out.data(), (std::min)((ULONG)(out.size() * 2), cap));
            t.length = (ULONG)s.size();
            reply.status.Information = n * 2;
        }
        else {
            int k = WideCharToMultiByte(page(), 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
            std::string a(k, '\0');
            WideCharToMultiByte(page(), 0, s.data(), (int)s.size(), a.data(), k, nullptr, nullptr);
            ULONG n = (std::min)((ULONG)a.size(), cap ? cap - 1 : 0);
            a.resize(n);
            a += '\0';
            device.output(msg, msg.apiSize, a.data(), (std::min)((ULONG)a.size(), cap));
            t.length = (ULONG)k;
            reply.status.Information = n;
        }
        return true;
    }

    bool setTitle() {
        bool unicode = msg.as<BOOLEAN>() != 0;
        ULONG n = msg.userInput();
        scratch.resize(n);
        device.input(msg, msg.apiSize + 8, scratch.data(), n);
        if (unicode) title.assign(reinterpret_cast<const wchar_t*>(scratch.data()), n / 2);
        else {
            int k = MultiByteToWideChar(page(), 0, scratch.data(), (int)n, nullptr, 0);
            title.assign(k, L'\0');
            MultiByteToWideChar(page(), 0, scratch.data(), (int)n, title.data(), k);
        }
        while (!title.empty() && title.back() == 0) title.pop_back();
        titleDirty = true;
        return true;
    }

    bool readOrPend(ReadKind kind) {
        if (kind == ReadKind::RAW) reply.write = {};
        if (tryRead(msg, kind, reply)) return true;
        if (kind == ReadKind::RECORDS && (msg.as<GetInputBody>().flags & READ_NOWAIT)) return true;
        pending.push_back({ msg, kind });
        return false;
    }

    void service() {
        for (size_t i = 0; i < pending.size();) {
            Pending& p = pending[i];
            condrv::Completion c = {};
            c.id = p.msg.d.id;
            c.status.Status = condrv::SUCCESS;
            if (p.kind != ReadKind::RAW) c.write = { p.msg.body, p.msg.apiSize, 0 };
            if (tryRead(p.msg, p.kind, c)) {
                device.complete(c);
                pending.erase(pending.begin() + i);
            }
            else ++i;
        }
    }

    void cancelReads(bool ctrlC) {
        bool lineMode = (inputMode & ENABLE_LINE_INPUT) != 0;
        for (size_t i = 0; i < pending.size();) {
            Pending& p = pending[i];
            bool cancel = p.kind != ReadKind::RECORDS && (!ctrlC || lineMode);
            if (!cancel) { ++i; continue; }
            condrv::Completion c = {};
            c.id = p.msg.d.id;
            c.status.Status = condrv::ALERTED;
            if (p.kind == ReadKind::CONSOLE) { p.msg.as<ReadBody>().bytes = 0; c.write = { p.msg.body, p.msg.apiSize, 0 }; }
            device.complete(c);
            pending.erase(pending.begin() + i);
        }
        if (lineMode) { editing = false; line.clear(); linePos = 0; }
    }

    bool tryRead(condrv::Message& mm, ReadKind kind, condrv::Completion& c) {
        if (kind == ReadKind::RECORDS) return readRecords(mm, c);
        bool unicode = kind == ReadKind::CONSOLE && mm.as<ReadBody>().unicode;
        bool controlZ = kind == ReadKind::RAW || mm.as<ReadBody>().controlZ;
        ULONG offset = kind == ReadKind::RAW ? 0 : mm.apiSize;
        ULONG cap = mm.d.outputSize - offset;
        std::wstring chars;
        if (inputMode & ENABLE_LINE_INPUT) {
            if (ready.empty()) {
                if (!editing) startEditing(mm, kind);
                if (!editLine()) return false;
            }
            size_t lineEnd = ready.find(L"\r\n");
            take(ready, chars, lineEnd == std::wstring::npos ? cap : (std::min)(cap, (ULONG)((lineEnd + 2) * (unicode ? 2 : 1))), unicode);
        }
        else {
            while (!queue.empty() && chars.size() < (unicode ? cap / 2 : cap)) {
                INPUT_RECORD r = queue.front();
                queue.pop_front();
                if (r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.uChar.UnicodeChar) chars += r.Event.KeyEvent.uChar.UnicodeChar;
            }
            if (chars.empty()) return false;
        }
        std::vector<char> out;
        if (unicode) out.assign(reinterpret_cast<const char*>(chars.data()), reinterpret_cast<const char*>(chars.data() + chars.size()));
        else {
            int k = WideCharToMultiByte(readPage(), 0, chars.data(), (int)chars.size(), nullptr, 0, nullptr, nullptr);
            out.resize(k);
            WideCharToMultiByte(readPage(), 0, chars.data(), (int)chars.size(), out.data(), k, nullptr, nullptr);
            if (out.size() > cap) out.resize(cap);
        }
        ULONG bytes = (ULONG)out.size();
        device.output(mm, offset, out.data(), bytes);
        if (controlZ && bytes && out[0] == 0x1A) bytes = 0;
        if (kind == ReadKind::CONSOLE) mm.as<ReadBody>().bytes = bytes;
        c.status.Information = bytes;
        if (queue.empty()) ResetEvent(inputEvent);
        return true;
    }

    void take(std::wstring& from, std::wstring& to, ULONG cap, bool unicode) {
        size_t n = 0;
        if (unicode) n = (std::min)((size_t)(cap / 2), from.size());
        else {
            ULONG used = 0;
            for (; n < from.size(); ++n) {
                char tmp[8];
                int k = WideCharToMultiByte(readPage(), 0, &from[n], 1, tmp, sizeof(tmp), nullptr, nullptr);
                if (used + (ULONG)(std::max)(k, 1) > cap) break;
                used += (ULONG)(std::max)(k, 1);
            }
        }
        to = from.substr(0, n);
        from.erase(0, n);
    }

    bool readRecords(condrv::Message& mm, condrv::Completion& c) {
        auto& b = mm.as<GetInputBody>();
        ULONG cap = (mm.d.outputSize - mm.apiSize) / sizeof(INPUT_RECORD);
        b.records = 0;
        if (queue.empty()) {
            c.status.Information = 0;
            return (b.flags & READ_NOWAIT) != 0;
        }
        size_t n = (std::min)((size_t)cap, queue.size());
        std::vector<INPUT_RECORD> out(queue.begin(), queue.begin() + n);
        if (!(b.flags & READ_NOREMOVE)) queue.erase(queue.begin(), queue.begin() + n);
        if (!b.unicode)
            for (auto& r : out)
                if (r.EventType == KEY_EVENT) {
                    wchar_t w = r.Event.KeyEvent.uChar.UnicodeChar;
                    r.Event.KeyEvent.uChar.UnicodeChar = 0;
                    r.Event.KeyEvent.uChar.AsciiChar = w < 0x80 ? (char)w : wideToNarrow(w);
                }
        device.output(mm, mm.apiSize, out.data(), (ULONG)(n * sizeof(INPUT_RECORD)));
        b.records = (ULONG)n;
        c.status.Information = n * sizeof(INPUT_RECORD);
        if (queue.empty()) ResetEvent(inputEvent);
        return true;
    }

    void startEditing(condrv::Message& mm, ReadKind kind) {
        editing = true;
        line.clear();
        linePos = 0;
        echoWidth = 0;
        if (kind == ReadKind::CONSOLE) {
            auto& b = mm.as<ReadBody>();
            readExe.assign(b.exeLength, L'\0');
            device.input(mm, mm.apiSize + 8, readExe.data(), b.exeLength * 2);
            if (b.initialBytes && b.unicode) {
                std::wstring initial(b.initialBytes / 2, L'\0');
                device.input(mm, mm.apiSize + 8 + b.exeLength * 2, initial.data(), b.initialBytes);
                line = initial;
                linePos = line.size();
                echoWidth = widthOf(line);
            }
        }
        echoStart = linear(active->emu) - echoWidth;
        historyPos = histories.of(readExe).size();
    }

    static int charWidth(wchar_t ch) { return ch < 0x20 ? 2 : unicode::isWide(ch) ? 2 : 1; }
    static int widthOf(const std::wstring& s, size_t n = std::wstring::npos) {
        int w = 0;
        for (size_t i = 0; i < (std::min)(n, s.size()); ++i) w += charWidth(s[i]);
        return w;
    }

    void echo() {
        if (!(inputMode & ENABLE_ECHO_INPUT)) return;
        Emulator& e = active->emu;
        int cols = e.cols();
        int start = (std::max)(0, echoStart);
        e.setCursor(start % cols, (std::min)(start / cols, e.rows() - 1));
        std::wstring shown;
        for (wchar_t ch : line) {
            if (ch < 0x20) { shown += L'^'; shown += (wchar_t)(ch + '@'); }
            else shown += ch;
        }
        int width = widthOf(line);
        e.writeWide(reinterpret_cast<const char16_t*>(shown.data()), shown.size());
        int after = linear(e);
        echoStart = after - width;
        if (width < echoWidth) {
            std::wstring pad(echoWidth - width, L' ');
            e.writeWide(reinterpret_cast<const char16_t*>(pad.data()), pad.size());
        }
        echoWidth = width;
        int target = echoStart + widthOf(line, linePos);
        e.setCursor(target % cols, (std::min)((std::max)(target, 0) / cols, e.rows() - 1));
        notify();
    }

    bool editLine() {
        while (!queue.empty()) {
            INPUT_RECORD r = queue.front();
            queue.pop_front();
            if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
            const KEY_EVENT_RECORD& k = r.Event.KeyEvent;
            wchar_t ch = k.uChar.UnicodeChar;
            WORD vk = k.wVirtualKeyCode;
            if (vk == VK_RETURN || ch == '\r') {
                linePos = line.size();
                echo();
                if (inputMode & ENABLE_ECHO_INPUT) active->emu.writeWide(u"\r\n", 2);
                histories.add(readExe, line);
                std::wstring expanded;
                ready = aliases.expand(readExe, line, expanded) ? expanded : line + L"\r\n";
                line.clear();
                linePos = 0;
                editing = false;
                notify();
                return true;
            }
            if (vk == VK_BACK || ch == 8) { if (linePos > 0) { line.erase(--linePos, 1); echo(); } continue; }
            if (ch == 0x1B || vk == VK_ESCAPE) { line.clear(); linePos = 0; echo(); continue; }
            if (!ch) {
                switch (vk) {
                case VK_LEFT: if (linePos > 0) --linePos; break;
                case VK_RIGHT: if (linePos < line.size()) ++linePos; break;
                case VK_HOME: linePos = 0; break;
                case VK_END: linePos = line.size(); break;
                case VK_DELETE: if (linePos < line.size()) line.erase(linePos, 1); break;
                case VK_UP: if (historyPos > 0) { line = histories.of(readExe)[--historyPos]; linePos = line.size(); } break;
                case VK_DOWN:
                    if (historyPos + 1 < histories.of(readExe).size()) { line = histories.of(readExe)[++historyPos]; linePos = line.size(); }
                    else { historyPos = histories.of(readExe).size(); line.clear(); linePos = 0; }
                    break;
                default: continue;
                }
                echo();
                continue;
            }
            line.insert(linePos++, 1, ch);
            echo();
        }
        return false;
    }
};

}
