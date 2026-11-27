#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <delayimp.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <functional>
#include "clipboard.h"
#include "input.h"
#include "menu.h"
#include "keys.h"
#include "pacing.h"
#include "../shell/pointer.h"
#include "settings.h"
#include <thread>
#include "strip.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

class Window {
public:
    HWND hwnd = nullptr;
    std::atomic<int> width{ 0 }, height{ 0 };
    std::atomic<UINT> dpi{ 96 };
    std::atomic<int> scrollbarWidth{ 12 };
    static constexpr UINT_PTR DRAG_TIMER = 1;
    static constexpr UINT WM_PROGRESS = WM_APP + 1;
    static constexpr ULONG_PTR HANDOFF_DATA = 0x63636374;
    std::atomic<int> cellW{ 0 }, cellH{ 0 }, tabCount{ 1 }, closedCount{ 0 };
    std::atomic<int> hoverButton{ -1 }, pressedButton{ -1 };
    std::atomic<int> hoverTab{ -1 };
    std::atomic<bool> hoverClose{ false }, hoverPlus{ false }, focused{ true };
    bool hoverCtrl = false;
    int hoverAt = -1;
    LONG doubleTime = 0;
    POINT doubleAt = {};

    Strip strip() const {
        if (fullscreen) return Strip();
        return Strip::make(width, dpi, cellW, cellH, tabCount);
    }

    StripHover stripHover() const { return { hoverTab.load(), hoverClose.load(), hoverPlus.load() }; }

    bool maximized() const { return IsZoomed(hwnd) != 0; }

    static bool loadVulkan() { return SUCCEEDED(__HrLoadAllImportsForDll("vulkan-1.dll")); }

    static std::vector<const char*> surfaceExtensions() { return { VK_KHR_WIN32_SURFACE_EXTENSION_NAME }; }

    static VkSurfaceKHR makeSurface(VkInstance instance, void* self) {
        VkWin32SurfaceCreateInfoKHR sc = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        sc.hinstance = GetModuleHandleW(nullptr);
        sc.hwnd = static_cast<Window*>(self)->hwnd;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        return vkCreateWin32SurfaceKHR(instance, &sc, nullptr, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
    }

    static void allowForeground(unsigned long pid) { AllowSetForegroundWindow((DWORD)pid); }

    Menu menuView() const { return Menu(); }

    bool otherWindowAt(int x, int y) { return cccWindowAt({ x, y }) != nullptr; }

    bool giveTab(const TabHandoff& h) {
        HWND target = cccWindowAt({ h.x, h.y });
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        HANDLE proc = target ? OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid) : nullptr;
        if (!proc) return false;
        HANDLE src[3] = { h.in, h.out, h.process.handle };
        HANDLE dst[3] = {};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) ok = DuplicateHandle(GetCurrentProcess(), src[i], proc, &dst[i], 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
        TabHandoff sent = h;
        sent.in = dst[0];
        sent.out = dst[1];
        sent.process.handle = dst[2];
        COPYDATASTRUCT cd = { HANDOFF_DATA, sizeof(sent), &sent };
        DWORD_PTR accepted = 0;
        ok = ok && SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &accepted) && accepted;
        if (!ok)
            for (HANDLE d : dst)
                if (d) DuplicateHandle(proc, d, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
        CloseHandle(proc);
        if (ok) SetForegroundWindow(target);
        return ok;
    }

    HWND cccWindowAt(POINT at) const {
        struct Search { POINT at; HWND self, found; } search{ at, hwnd, nullptr };
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            auto* s = reinterpret_cast<Search*>(lp);
            RECT r;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!IsWindowVisible(w) || IsIconic(w) || cloaked || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TRANSPARENT) || !GetWindowRect(w, &r) || !PtInRect(&r, s->at))
                return TRUE;
            wchar_t cls[8] = {};
            if (w != s->self && GetClassNameW(w, cls, 8) && !wcscmp(cls, L"ccc")) s->found = w;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    void setTitle(const std::string& t) { SetWindowTextW(hwnd, ccc::W(t).c_str()); }

    void requestClose() { PostMessageW(hwnd, WM_CLOSE, 0, 0); }

    void bell() {
        if (GetForegroundWindow() == hwnd) return;
        FLASHWINFO f = { sizeof(f), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
        FlashWindowEx(&f);
    }

    void progress(int state, int value) { PostMessageW(hwnd, WM_PROGRESS, (WPARAM)state, (LPARAM)value); }

    void notify(const std::string& title, const std::string& body) {
        NOTIFYICONDATAW n = { sizeof(n) };
        n.hWnd = hwnd;
        n.uID = 1;
        n.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
        n.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
        n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcsncpy_s(n.szTip, L"ccc", _TRUNCATE);
        wcsncpy_s(n.szInfoTitle, ccc::W(title.empty() ? std::string("ccc") : title).c_str(), _TRUNCATE);
        wcsncpy_s(n.szInfo, ccc::W(body.empty() ? title : body).c_str(), _TRUNCATE);
        if (!trayIcon) trayIcon = Shell_NotifyIconW(NIM_ADD, &n) != FALSE;
        else Shell_NotifyIconW(NIM_MODIFY, &n);
    }

    void setClipboard(const std::string& text) { clipboard::set(hwnd, text); }

    void readClipboard(const std::function<void(const std::string&)>& done) { done(clipboard::get(hwnd)); }

    void openUri(const std::string& uri) { ShellExecuteW(nullptr, L"open", ccc::W(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

    void setPointer(Pointer p) {
        if (pointer.exchange(p) != p) PostMessageW(hwnd, WM_SETCURSOR, (WPARAM)hwnd, HTCLIENT);
    }

    WINDOWPLACEMENT closedPlacement = { sizeof(WINDOWPLACEMENT) };
    bool closedValid = false;

    static unsigned systemDpi() {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return GetDpiForSystem();
    }

    void alert(const std::string& text) { MessageBoxW(hwnd, ccc::W(text).c_str(), L"ccc", MB_ICONERROR); }

    void finish(std::thread& render) {
        pumpUntilSignaled(render.native_handle());
        render.join();
    }

    bool savedPlacement(WindowSettings& s) const {
        if (!closedValid) return false;
        const RECT& r = closedPlacement.rcNormalPosition;
        s.rect = { r.left, r.top, r.right, r.bottom };
        s.hasRect = true;
        s.maximized = closedPlacement.showCmd == SW_SHOWMAXIMIZED || (closedPlacement.flags & WPF_RESTORETOMAXIMIZED);
        return true;
    }

    bool create(int clientW, int clientH, InputQueue& queue, const WindowSettings& saved) {
        RECT restoreRect = { saved.rect.left, saved.rect.top, saved.rect.right, saved.rect.bottom };
        const RECT* restore = saved.hasRect ? &restoreRect : nullptr;
        bool restoreMaximized = saved.maximized;
        inputs = &queue;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_IBEAM);
        wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
        wc.lpszClassName = L"ccc";
        RegisterClassExW(&wc);
        DWORD style = WS_OVERLAPPEDWINDOW;
        RECT r = { 0, 0, clientW, clientH };
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"ccc", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, instance, this);
        if (!hwnd) return false;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        useDarkMenus();
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (restore && MonitorFromRect(restore, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp = { sizeof(wp) };
            wp.rcNormalPosition = *restore;
            wp.showCmd = restoreMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement(hwnd, &wp);
        }
        else ShowWindow(hwnd, SW_SHOWDEFAULT);
        dpi = GetDpiForWindow(hwnd);
        RECT client;
        GetClientRect(hwnd, &client);
        width = client.right;
        height = client.bottom;
        DragAcceptFiles(hwnd, TRUE);
        return true;
    }

    void resizeClient(int clientW, int clientH) {
        if (fullscreen || maximized()) return;
        RECT outer, inner;
        GetWindowRect(hwnd, &outer);
        GetClientRect(hwnd, &inner);
        int w = clientW + (outer.right - outer.left) - inner.right, h = clientH + (outer.bottom - outer.top) - inner.bottom;
        SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void toggleFullscreen() {
        DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        if (!fullscreen) {
            MONITORINFO mi = { sizeof(mi) };
            if (!GetWindowPlacement(hwnd, &placement) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
            fullscreen = true;
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        else {
            fullscreen = false;
            SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(hwnd, &placement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }

    void runMessageLoop() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void pumpUntilSignaled(HANDLE h) {
        while (MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
    }

private:
    InputQueue* inputs = nullptr;
    WINDOWPLACEMENT placement = { sizeof(placement) };
    std::atomic<bool> fullscreen{ false };
    wchar_t highSurrogate = 0;

    void post(InputEvent e) {
        inputs->push(e);
        requestFrame();
    }

    void postTab(InputEvent::Kind kind, int value = 0) {
        InputEvent e{ kind };
        e.value = value;
        post(e);
    }

    int resizeBorder() const { return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi); }

    static int buttonOfHit(WPARAM hit) { return hit == HTMINBUTTON ? 0 : hit == HTMAXBUTTON ? 1 : hit == HTCLOSE ? 2 : -1; }

    void setHover(int tab, bool close, bool plus) {
        bool changed = hoverTab.exchange(tab) != tab;
        changed |= hoverClose.exchange(close) != close;
        changed |= hoverPlus.exchange(plus) != plus;
        if (changed) requestFrame();
    }

    void setButtons(int hover, int pressed) {
        bool changed = hoverButton.exchange(hover) != hover;
        changed |= pressedButton.exchange(pressed) != pressed;
        if (changed) requestFrame();
    }

    void trackLeave(DWORD flags) {
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE | flags, hwnd, 0 };
        TrackMouseEvent(&t);
    }

    LRESULT hitTest(int sx, int sy) {
        POINT p = { sx, sy };
        ScreenToClient(hwnd, &p);
        Strip s = strip();
        int border = resizeBorder();
        if (!maximized() && p.y < border) return p.x < border ? HTTOPLEFT : p.x >= width - border ? HTTOPRIGHT : HTTOP;
        if (p.y >= s.height) return HTCLIENT;
        int b = s.button(p.x);
        if (b >= 0) return b == 0 ? HTMINBUTTON : b == 1 ? HTMAXBUTTON : HTCLOSE;
        if (s.tabAt(p.x) >= 0 || s.onNew(p.x)) return HTCLIENT;
        return HTCAPTION;
    }

    void pressButton(int b) {
        if (b == 0) ShowWindow(hwnd, SW_MINIMIZE);
        else if (b == 1) ShowWindow(hwnd, maximized() ? SW_RESTORE : SW_MAXIMIZE);
        else if (b == 2) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    bool tabKey(WPARAM vk) {
        bool ctrl = keyDown(VK_CONTROL), shift = keyDown(VK_SHIFT), alt = keyDown(VK_MENU);
        if (!ctrl || alt) return false;
        if (vk == VK_TAB) postTab(InputEvent::TabCycle, shift ? -1 : 1);
        else if (vk == 'T') postTab(shift ? InputEvent::TabReopen : InputEvent::TabNew, 1);
        else if (vk == 'W') postTab(InputEvent::TabClose, -1);
        else if (!shift && vk >= '1' && vk <= '9') postTab(InputEvent::TabSelect, vk == '9' ? (std::max)((int)tabCount - 1, 0) : (int)vk - '1');
        else if (shift && vk == 'N') postTab(InputEvent::TabNew);
        else return false;
        takeChar(WM_CHAR, WM_DEADCHAR);
        return true;
    }

    void stripClick(int x, int y, bool middle) {
        Strip s = strip();
        int tab = s.tabAt(x);
        if (tab >= 0 && !middle && !s.onClose(x)) {
            postTab(InputEvent::TabSelect, tab);
            dragTab = tab;
            dragStart = { x, y };
            grab = { x - s.tabLeft(tab), y - s.tabTop };
            dragging = false;
            SetCapture(hwnd);
        }
        else if (tab >= 0) postTab(InputEvent::TabClose, tab);
        else if (s.onNew(x) && !middle) postTab(InputEvent::TabNew);
    }

    void tabMenu(int x, int y) {
        int tab = strip().tabAt(x), count = tabCount;
        if (tab < 0) return;
        enum { NEW = 1, CLOSE, OTHERS, RIGHT, REOPEN };
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, NEW, L"New tab\tCtrl+Shift+N");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CLOSE, L"Close tab\tCtrl+Shift+W");
        AppendMenuW(m, MF_STRING | (count > 1 ? 0 : MF_GRAYED), OTHERS, L"Close other tabs");
        AppendMenuW(m, MF_STRING | (tab < count - 1 ? 0 : MF_GRAYED), RIGHT, L"Close tabs to the right");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (closedCount > 0 ? 0 : MF_GRAYED), REOPEN, L"Reopen closed tab\tCtrl+Shift+T");
        POINT p = { x, y };
        ClientToScreen(hwnd, &p);
        int pick = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (pick == NEW) postTab(InputEvent::TabNew);
        else if (pick == CLOSE) postTab(InputEvent::TabClose, tab);
        else if (pick == OTHERS) postTab(InputEvent::TabCloseOthers, tab);
        else if (pick == RIGHT) postTab(InputEvent::TabCloseRight, tab);
        else if (pick == REOPEN) postTab(InputEvent::TabReopen);
    }

    static void useDarkMenus() {
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return;
        using SetPreferredAppMode = int(WINAPI*)(int);
        using FlushMenuThemes = void(WINAPI*)();
        auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
        auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
        if (setMode) setMode(2);
        if (flush) flush();
    }

    int dragTab = -1;
    POINT dragStart = {}, grab = {};
    bool dragging = false;
    HWND outlineWindow = nullptr;
    bool outlineReady = false;

    void showDragOutline(int x, int y) {
        POINT at = { x - grab.x, y - grab.y };
        ClientToScreen(hwnd, &at);
        if (!outlineWindow) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"cccDragOutline";
            RegisterClassW(&wc);
            outlineWindow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0,
                                            nullptr, nullptr, wc.hInstance, nullptr);
        }
        if (!outlineWindow) return;
        if (!outlineReady) {
            DragOutline o = DragOutline::make(strip(), dpi);
            if (!o.width || !o.height) return;
            BITMAPINFO bi = {};
            bi.bmiHeader = { sizeof(BITMAPINFOHEADER), o.width, -o.height, 1, 32, BI_RGB };
            HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(memory, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap) {
                std::memcpy(bits, o.pixels.data(), o.pixels.size() * 4);
                HGDIOBJ old = SelectObject(memory, bitmap);
                SIZE size = { o.width, o.height };
                POINT origin = {};
                BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                UpdateLayeredWindow(outlineWindow, screen, &at, &size, memory, &origin, 0, &blend, ULW_ALPHA);
                SelectObject(memory, old);
                DeleteObject(bitmap);
                outlineReady = true;
            }
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
        }
        SetWindowPos(outlineWindow, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void hideDragOutline() {
        if (outlineWindow) ShowWindow(outlineWindow, SW_HIDE);
        outlineReady = false;
    }

    bool overStrip(int x, int y) const {
        int h = strip().height;
        return x >= 0 && x < width && y >= -h && y < 2 * h;
    }

    void dragStripTab(int x, int y) {
        if (!dragging && abs(x - dragStart.x) < GetSystemMetrics(SM_CXDRAG) && abs(y - dragStart.y) < GetSystemMetrics(SM_CYDRAG)) return;
        dragging = true;
        bool inside = overStrip(x, y);
        SetCursor(LoadCursorW(nullptr, inside ? (LPCWSTR)IDC_ARROW : (LPCWSTR)IDC_SIZEALL));
        if (!inside) { showDragOutline(x, y); return; }
        hideDragOutline();
        Strip s = strip();
        int to = std::clamp((std::max)(x - s.x0, 0) / ((std::max)(s.tabCols, 1) * s.cellW), 0, (std::max)(s.count - 1, 0));
        if (to == dragTab) return;
        InputEvent e{ InputEvent::TabMove, dragTab, to };
        post(e);
        dragTab = to;
    }

    void dropStripTab(int x, int y) {
        int tab = std::exchange(dragTab, -1);
        hideDragOutline();
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!dragging || overStrip(x, y)) return;
        POINT p = { x, y };
        ClientToScreen(hwnd, &p);
        post({ InputEvent::TabTear, tab, (int)p.x, (int)p.y });
    }

    LRESULT receiveTab(const COPYDATASTRUCT* cd) {
        if (cd->dwData != HANDOFF_DATA || cd->cbData != sizeof(TabHandoff)) return FALSE;
        TabHandoff h;
        std::memcpy(&h, cd->lpData, sizeof h);
        POINT p = { h.x, h.y };
        ScreenToClient(hwnd, &p);
        h.index = p.y >= 0 && p.y < strip().height ? strip().tabAt(p.x) : -1;
        inputs->push(h);
        requestFrame();
        return TRUE;
    }

    static bool keyDown(int vk) { return GetKeyState(vk) < 0; }

    ITaskbarList3* taskbar = nullptr;

    void showProgress(int state, int value) {
        if (!taskbar) {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) || FAILED(taskbar->HrInit())) {
                if (taskbar) taskbar->Release();
                taskbar = nullptr;
                return;
            }
        }
        static const TBPFLAG flags[] = { TBPF_NOPROGRESS, TBPF_NORMAL, TBPF_ERROR, TBPF_INDETERMINATE, TBPF_PAUSED };
        state = state >= 0 && state <= 4 ? state : 0;
        taskbar->SetProgressState(hwnd, flags[state]);
        if (state == 1 || state == 2 || state == 4) taskbar->SetProgressValue(hwnd, (ULONGLONG)value, 100);
    }

    void postButton(InputEvent::Kind kind, int x, int y, WPARAM wp, int value = 0) {
        InputEvent e{ kind, value, x, y, (wp & MK_CONTROL) != 0, (wp & MK_SHIFT) != 0 };
        e.state = keyState(0);
        post(e);
    }

    static unsigned keyState(LPARAM lp) {
        unsigned s = 0;
        if (keyDown(VK_LMENU)) s |= LEFT_ALT_PRESSED;
        if (keyDown(VK_RMENU)) s |= RIGHT_ALT_PRESSED;
        if (keyDown(VK_LCONTROL)) s |= LEFT_CTRL_PRESSED;
        if (keyDown(VK_RCONTROL)) s |= RIGHT_CTRL_PRESSED;
        if (keyDown(VK_SHIFT)) s |= SHIFT_PRESSED;
        if (GetKeyState(VK_NUMLOCK) & 1) s |= NUMLOCK_ON;
        if (GetKeyState(VK_SCROLL) & 1) s |= SCROLLLOCK_ON;
        if (GetKeyState(VK_CAPITAL) & 1) s |= CAPSLOCK_ON;
        if (lp & (1 << 24)) s |= ENHANCED_KEY;
        return s;
    }

    uint32_t takeChar(UINT first, UINT last) {
        MSG m;
        wchar_t high = 0;
        while (PeekMessageW(&m, hwnd, first, last, PM_NOREMOVE)) {
            PeekMessageW(&m, hwnd, m.message, m.message, PM_REMOVE);
            if (m.message == WM_DEADCHAR || m.message == WM_SYSDEADCHAR) return 0;
            wchar_t w = (wchar_t)m.wParam;
            if (w >= 0xD800 && w < 0xDC00) { high = w; continue; }
            if (w >= 0xDC00 && w < 0xE000) return high ? 0x10000 + ((high - 0xD800) << 10) + (w - 0xDC00) : 0xFFFD;
            return w;
        }
        return 0;
    }

    bool pointerHidden = false;
    POINT hiddenAt = {};

    void showPointer(bool visible) {
        if (!visible) { SetCursor(nullptr); return; }
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        SetCursor(LoadCursorW(nullptr, p.y < strip().height || p.x >= width - scrollbarWidth ? (LPCWSTR)IDC_ARROW : cursorFor(pointer)));
    }

    static LPCWSTR cursorFor(Pointer p) {
        static const LPCWSTR ids[] = { (LPCWSTR)IDC_IBEAM, (LPCWSTR)IDC_ARROW, (LPCWSTR)IDC_HAND, (LPCWSTR)IDC_CROSS, (LPCWSTR)IDC_WAIT, (LPCWSTR)IDC_APPSTARTING, (LPCWSTR)IDC_HELP, (LPCWSTR)IDC_NO, (LPCWSTR)IDC_SIZEALL, (LPCWSTR)IDC_SIZEWE, (LPCWSTR)IDC_SIZENS, (LPCWSTR)IDC_SIZENESW, (LPCWSTR)IDC_SIZENWSE };
        return ids[(int)p];
    }

    std::atomic<Pointer> pointer{ Pointer::Text };
    bool trayIcon = false;

    void hidePointerWhileTyping(WPARAM vk) {
        if (pointerHidden || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL) return;
        BOOL vanish = FALSE;
        if (!SystemParametersInfoW(SPI_GETMOUSEVANISH, 0, &vanish, 0) || !vanish || GetCapture() == hwnd) return;
        POINT p;
        GetCursorPos(&p);
        if (WindowFromPoint(p) != hwnd) return;
        hiddenAt = p;
        pointerHidden = true;
        showPointer(false);
    }

    void revealPointer() {
        if (!pointerHidden) return;
        pointerHidden = false;
        showPointer(true);
    }

    void postKey(WPARAM wp, LPARAM lp, bool alt, bool release, uint32_t ch) {
        if (!release) hidePointerWhileTyping(wp);
        InputEvent e{ InputEvent::Key, (int)wp, 0, 0, keyDown(VK_CONTROL), keyDown(VK_SHIFT), std::string(), alt };
        e.release = release;
        e.repeat = !release && (lp & (1 << 30)) != 0;
        e.scan = (int)(lp >> 16 & 0xFF);
        e.state = keyState(lp);
        e.ch = ch;
        e.base = layoutChar((unsigned)wp, (unsigned)e.scan, false);
        e.shifted = layoutChar((unsigned)wp, (unsigned)e.scan, true);
        e.super = keyDown(VK_LWIN) || keyDown(VK_RWIN);
        post(e);
    }

    static std::string droppedPaths(HDROP drop) {
        std::string out;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, path.data(), (UINT)path.size() + 1);
            int n = WideCharToMultiByte(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0, nullptr, nullptr);
            std::string utf8(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.data(), (int)path.size(), utf8.data(), n, nullptr, nullptr);
            bool quote = utf8.find_first_of(" &()[]{}^=;!'+,`~") != std::string::npos;
            if (!out.empty()) out += ' ';
            out += quote ? "\"" + utf8 + "\"" : utf8;
        }
        DragFinish(drop);
        return out;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int inset = strip().height;
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp && !fullscreen) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                LONG top = params->rgrc[0].top;
                LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
                params->rgrc[0].top = top + (maximized() ? resizeBorder() : 0);
                return r;
            }
            break;
        case WM_NCHITTEST: {
            if (fullscreen) break;
            LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            return hit == HTCLIENT ? hitTest(x, y) : hit;
        }
        case WM_NCMOUSEMOVE: {
            int b = buttonOfHit(wp);
            setHover(-1, false, false);
            setButtons(b, b >= 0 && b == pressedButton ? b : -1);
            trackLeave(TME_NONCLIENT);
            if (b >= 0) return 0;
            break;
        }
        case WM_NCMOUSELEAVE:
            setButtons(-1, -1);
            break;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (buttonOfHit(wp) >= 0) { setButtons(buttonOfHit(wp), buttonOfHit(wp)); return 0; }
            break;
        case WM_NCLBUTTONUP:
            if (buttonOfHit(wp) >= 0) {
                int pressed = pressedButton.load();
                setButtons(buttonOfHit(wp), -1);
                if (pressed == buttonOfHit(wp)) pressButton(pressed);
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            setHover(-1, false, false);
            pointerHidden = false;
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if (msg == WM_KILLFOCUS) revealPointer();
            focused = msg == WM_SETFOCUS;
            requestFrame();
            post({ InputEvent::Focus, msg == WM_SETFOCUS ? 1 : 0 });
            break;
        case WM_PROGRESS:
            showProgress((int)wp, (int)lp);
            return 0;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
            if (y < inset) return 0;
            SetCapture(hwnd);
            postButton(InputEvent::MiddleDown, x, y - inset, wp);
            return 0;
        case WM_MBUTTONUP:
            if (GetCapture() == hwnd) {
                if (!(wp & (MK_LBUTTON | MK_RBUTTON))) ReleaseCapture();
                postButton(InputEvent::MiddleUp, x, y - inset, wp);
            }
            else if (y < inset) stripClick(x, y, true);
            return 0;
        case WM_SIZE:
            width = LOWORD(lp);
            height = HIWORD(lp);
            requestFrame();
            return 0;
        case WM_DPICHANGED: {
            dpi = LOWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            requestFrame();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                showPointer(!pointerHidden);
                return TRUE;
            }
            break;
        case WM_CHAR:
            if (wp >= 0xD800 && wp < 0xDC00) { highSurrogate = (wchar_t)wp; return 0; }
            if (wp >= 0xDC00 && wp < 0xE000) {
                if (highSurrogate) post({ InputEvent::Char, 0x10000 + ((highSurrogate - 0xD800) << 10) + ((int)wp - 0xDC00) });
                highSurrogate = 0;
                return 0;
            }
            post({ InputEvent::Char, (int)wp });
            return 0;
        case WM_KEYDOWN:
            if (tabKey(wp)) return 0;
            postKey(wp, lp, false, false, takeChar(WM_CHAR, WM_DEADCHAR));
            return 0;
        case WM_KEYUP:
            postKey(wp, lp, false, true, 0);
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN) { takeChar(WM_SYSCHAR, WM_SYSDEADCHAR); toggleFullscreen(); return 0; }
            if (wp == VK_F4 && keyDown(VK_MENU)) break;
            postKey(wp, lp, keyDown(VK_MENU), false, takeChar(WM_SYSCHAR, WM_SYSDEADCHAR));
            return 0;
        case WM_SYSKEYUP:
            postKey(wp, lp, keyDown(VK_MENU), true, 0);
            return 0;
        case WM_SYSCHAR:
            if (wp == VK_RETURN) return 0;
            break;
        case WM_LBUTTONDOWN:
            if (y < inset) { stripClick(x, y, false); return 0; }
            SetCapture(hwnd);
            if ((DWORD)(GetMessageTime() - doubleTime) <= GetDoubleClickTime() && abs(x - doubleAt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                abs(y - doubleAt.y) <= GetSystemMetrics(SM_CYDOUBLECLK)) {
                doubleTime = 0;
                postButton(InputEvent::LeftDouble, x, y - inset, wp, 3);
                return 0;
            }
            postButton(InputEvent::LeftDown, x, y - inset, wp);
            return 0;
        case WM_LBUTTONUP:
            if (dragTab >= 0) { dropStripTab(x, y); return 0; }
            if (GetCapture() != hwnd) return 0;
            if (!(wp & (MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
            KillTimer(hwnd, DRAG_TIMER);
            postButton(InputEvent::LeftUp, x, y - inset, wp);
            return 0;
        case WM_LBUTTONDBLCLK:
            if (y < inset) { stripClick(x, y, false); return 0; }
            doubleTime = GetMessageTime();
            doubleAt = { x, y };
            SetCapture(hwnd);
            postButton(InputEvent::LeftDouble, x, y - inset, wp);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            if (y < inset) return 0;
            SetCapture(hwnd);
            postButton(InputEvent::RightDown, x, y - inset, wp);
            return 0;
        case WM_RBUTTONUP:
            if (GetCapture() != hwnd) {
                if (y < inset) tabMenu(x, y);
                return 0;
            }
            if (!(wp & (MK_LBUTTON | MK_MBUTTON))) ReleaseCapture();
            postButton(InputEvent::RightUp, x, y - inset, wp);
            return 0;
        case WM_MOUSEMOVE: {
            if (dragTab >= 0 && GetCapture() == hwnd) { dragStripTab(x, y); return 0; }
            if (pointerHidden) {
                POINT p;
                GetCursorPos(&p);
                if (p.x != hiddenAt.x || p.y != hiddenAt.y) revealPointer();
            }
            setButtons(-1, -1);
            Strip s = strip();
            bool inStrip = y < inset && GetCapture() != hwnd;
            int tab = inStrip ? s.tabAt(x) : -1;
            setHover(tab, tab >= 0 && s.onClose(x), inStrip && s.onNew(x));
            if (inStrip) trackLeave(0);
            if ((wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) && GetCapture() == hwnd) {
                postButton(InputEvent::Move, x, y - inset, wp);
                if (y < inset || y >= height) SetTimer(hwnd, DRAG_TIMER, 50, nullptr);
                else KillTimer(hwnd, DRAG_TIMER);
            }
            else {
                bool ctrl = (wp & MK_CONTROL) && !inStrip;
                int at = inStrip ? -1 : (y - inset) * 65536 + x;
                if (ctrl != hoverCtrl || at != hoverAt) post({ InputEvent::Hover, 0, x, y - inset, ctrl });
                hoverCtrl = ctrl;
                hoverAt = at;
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p = { x, y };
            ScreenToClient(hwnd, &p);
            if (p.y < inset) return 0;
            InputEvent e{ InputEvent::Wheel, GET_WHEEL_DELTA_WPARAM(wp), p.x, p.y - inset, (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0 };
            e.state = keyState(0);
            post(e);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd) dragTab = -1;
            break;
        case WM_COPYDATA:
            return receiveTab(reinterpret_cast<const COPYDATASTRUCT*>(lp));
        case WM_DROPFILES:
            post({ InputEvent::Text, 0, 0, 0, false, false, droppedPaths(reinterpret_cast<HDROP>(wp)) });
            return 0;
        case WM_TIMER:
            if (wp == DRAG_TIMER) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (GetCapture() == hwnd) post({ InputEvent::Move, 0, p.x, p.y - inset });
                else KillTimer(hwnd, DRAG_TIMER);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (tabCount > 1) {
                std::wstring text = L"Close all " + std::to_wstring(tabCount.load()) + L" tabs?";
                if (MessageBoxW(hwnd, text.c_str(), L"ccc", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1) != IDOK) return 0;
            }
            if (fullscreen) { closedPlacement = placement; closedValid = true; }
            else closedValid = GetWindowPlacement(hwnd, &closedPlacement) != FALSE;
            ShowWindow(hwnd, SW_HIDE);
            if (trayIcon) { NOTIFYICONDATAW n = { sizeof(n) }; n.hWnd = hwnd; n.uID = 1; Shell_NotifyIconW(NIM_DELETE, &n); }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK procedure(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* w = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            w->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        }
        auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return w ? w->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }
};
