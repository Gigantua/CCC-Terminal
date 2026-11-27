#pragma once
#include <vulkan/vulkan.h>
#define Window XWindow
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <vulkan/vulkan_xlib.h>
#undef Window
#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#include "input.h"
#include "menu.h"
#include "folders.h"
#include "keys.h"
#include "pacing.h"
#include "../shell/pointer.h"
#include "settings.h"
#include "strip.h"
#include "tab_socket.h"
#include <thread>

extern char** environ;

class Window {
public:
    std::atomic<int> width{ 0 }, height{ 0 };
    std::atomic<unsigned> dpi{ 96 };
    std::atomic<int> scrollbarWidth{ 12 };
    std::atomic<int> cellW{ 0 }, cellH{ 0 }, tabCount{ 1 }, closedCount{ 0 };
    std::atomic<int> hoverButton{ -1 }, pressedButton{ -1 };
    std::atomic<int> hoverTab{ -1 };
    DropSlot dropSlot;
    XWindow dropTarget = 0;
    std::atomic<bool> hoverClose{ false }, hoverPlus{ false }, focused{ true };

    ~Window() {
        if (!display) return;
        if (outlineWindow) XDestroyWindow(display, outlineWindow);
        if (window) XDestroyWindow(display, window);
        XCloseDisplay(display);
        close(wake);
    }

    static bool loadVulkan() { return true; }

    static std::vector<const char*> surfaceExtensions() { return { VK_KHR_XLIB_SURFACE_EXTENSION_NAME }; }

    static VkSurfaceKHR makeSurface(VkInstance instance, void* self) {
        Window* w = static_cast<Window*>(self);
        VkXlibSurfaceCreateInfoKHR sc = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
        sc.dpy = w->display;
        sc.window = w->window;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        return vkCreateXlibSurfaceKHR(instance, &sc, nullptr, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
    }

    Strip strip() const {
        if (fullscreen) return Strip();
        return Strip::make(width, dpi, cellW, cellH, tabCount);
    }

    StripHover stripHover() const { return { hoverTab.load(), hoverClose.load(), hoverPlus.load(), dropSlot.get() }; }

    bool maximized() const { return maximizedNow; }

    static unsigned systemDpi() { return 96; }

    static void allowForeground(unsigned long) {}

    Menu menuView() { std::lock_guard<std::mutex> l(menuMutex); return menu; }

    bool otherWindowAt(int x, int y) { return onEventThread([&] { return cccWindowAt(x, y); }) != 0; }

    bool giveTab(const TabHandoff& h) {
        XWindow target = onEventThread([&] { return cccWindowAt(h.x, h.y); });
        if (!target) return false;
        socklen_t len = 0;
        sockaddr_un address = tabAddress(target, len);
        return tabsocket::give(address, len, SOCK_SEQPACKET, h);
    }

    void alert(const std::string& text) { fprintf(stderr, "ccc: %s\n", text.c_str()); }

    void finish(std::thread& render) { render.join(); }

    bool savedPlacement(WindowSettings& s) const {
        s.rect = { 0, 0, width.load(), height.load() };
        s.hasRect = !maximizedNow && !fullscreen;
        s.maximized = maximizedNow;
        return true;
    }

    void openUri(const std::string& uri) {
        std::thread([uri] {
            pid_t pid;
            char* argv[] = { const_cast<char*>("xdg-open"), const_cast<char*>(uri.c_str()), nullptr };
            if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0) waitpid(pid, nullptr, 0);
        }).detach();
    }

    bool create(int clientW, int clientH, InputQueue& queue, const WindowSettings& saved) {
        if (saved.hasRect) { clientW = saved.rect.right - saved.rect.left; clientH = saved.rect.bottom - saved.rect.top; }
        inputs = &queue;
        XInitThreads();
        display = XOpenDisplay(nullptr);
        if (!display) return false;
        dpi = readDpi();
        int screen = DefaultScreen(display);
        XSetWindowAttributes a = {};
        a.background_pixel = BlackPixel(display, screen);
        a.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                       FocusChangeMask | StructureNotifyMask | ExposureMask | PropertyChangeMask;
        window = XCreateWindow(display, RootWindow(display, screen), saved.rect.left, saved.rect.top, (unsigned)clientW, (unsigned)clientH, 0, CopyFromParent, InputOutput,
                               CopyFromParent, CWBackPixel | CWEventMask, &a);
        if (!window) return false;
        for (auto& [atom, name] : atomNames()) atom = XInternAtom(display, name, False);
        XSetWMProtocols(display, window, &atoms.deleteWindow, 1);
        long dndVersion = 5;
        XChangeProperty(display, window, atoms.dndAware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&dndVersion), 1);
        XClassHint cls = { const_cast<char*>("ccc"), const_cast<char*>("ccc") };
        XSetClassHint(display, window, &cls);
        long motif[5] = { 2, 0, 0, 0, 0 };
        XChangeProperty(display, window, atoms.motifHints, atoms.motifHints, 32, PropModeReplace, reinterpret_cast<unsigned char*>(motif), 5);
        im = XOpenIM(display, nullptr, nullptr, nullptr);
        if (im) ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window, XNFocusWindow, window, nullptr);
        applyTitle("ccc");
        width = clientW;
        height = clientH;
        XMapWindow(display, window);
        if (saved.maximized) sendState(1, atoms.maxVert, atoms.maxHorz);
        XFlush(display);
        wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        listenForTabs();
        return true;
    }

    void runMessageLoop() {
        pollfd fds[3] = { { ConnectionNumber(display), POLLIN, 0 }, { wake, POLLIN, 0 }, { tabSocket, POLLIN, 0 } };
        while (!quitting) {
            runQueued();
            while (!quitting && XPending(display)) {
                XEvent e;
                XNextEvent(display, &e);
                if (XFilterEvent(&e, None)) continue;
                handle(e);
            }
            if (quitting) break;
            XFlush(display);
            poll(fds, 3, -1);
            if (fds[1].revents) { uint64_t v; ssize_t n = read(wake, &v, sizeof v); (void)n; }
            if (fds[2].revents) receiveTab();
        }
    }

    void requestClose() { post([this] { quitting = true; }); }

    void setTitle(const std::string& t) { post([this, t] { applyTitle(t); }); }

    void bell() { post([this] { if (!focused) urgent(true); }); }

    void progress(int, int) {}

    void notify(const std::string& title, const std::string& body) {
        std::string t = title.empty() ? std::string("ccc") : title, b = body.empty() ? title : body;
        std::thread([t, b] {
            pid_t pid;
            char* argv[] = { const_cast<char*>("notify-send"), const_cast<char*>("--app-name=ccc"), const_cast<char*>(t.c_str()), const_cast<char*>(b.c_str()), nullptr };
            if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, argv, environ) == 0) waitpid(pid, nullptr, 0);
        }).detach();
    }

    void setPointer(Pointer p) {
        if (pointer.exchange(p) != p) post([this] { applyPointer(); });
    }

    void resizeClient(int clientW, int clientH) {
        post([this, clientW, clientH] { if (!maximizedNow && !fullscreen) XResizeWindow(display, window, (unsigned)clientW, (unsigned)clientH); });
    }

    void setClipboard(const std::string& text) { post([this, text] { clipText = text; XSetSelectionOwner(display, atoms.clipboard, window, CurrentTime); }); }

    void readClipboard(std::function<void(const std::string&)> done) {
        post([this, done] {
            if (XGetSelectionOwner(display, atoms.clipboard) == window) { done(clipText); return; }
            pasteWaiters.push_back(done);
            if (pasteWaiters.size() == 1) XConvertSelection(display, atoms.clipboard, atoms.utf8, atoms.transfer, window, CurrentTime);
        });
    }

private:
    struct Atoms {
        Atom deleteWindow, motifHints, netName, utf8, netState, maxVert, maxHorz, fullscreenState, moveResize, clipboard, targets, transfer, hidden;
        Atom dndAware, dndEnter, dndPosition, dndStatus, dndDrop, dndFinished, dndSelection, dndCopy, uriList, drop, tabSocketAtom;
    };
    Display* display = nullptr;
    XWindow window = 0;
    XIM im = nullptr;
    XIC ic = nullptr;
    Atoms atoms = {};
    InputQueue* inputs = nullptr;
    int wake = -1, tabSocket = -1;
    std::mutex queueMutex;
    std::vector<std::function<void()>> queued;
    bool quitting = false, maximizedNow = false, fullscreen = false, pointerHidden = false, hoverCtrl = false;
    std::atomic<Pointer> pointer{ Pointer::Text };
    std::string clipText;
    std::vector<std::function<void(const std::string&)>> pasteWaiters;
    int hoverAt = -1, dragTab = -1, dragX = 0, dragY = 0, clicks = 0, clickX = 0, clickY = 0;
    bool dragging = false;
    Time clickTime = 0;
    unsigned buttonsHeld = 0;

    std::vector<std::pair<Atom&, const char*>> atomNames() {
        return { { atoms.deleteWindow, "WM_DELETE_WINDOW" }, { atoms.motifHints, "_MOTIF_WM_HINTS" }, { atoms.netName, "_NET_WM_NAME" },
                 { atoms.utf8, "UTF8_STRING" }, { atoms.netState, "_NET_WM_STATE" }, { atoms.maxVert, "_NET_WM_STATE_MAXIMIZED_VERT" },
                 { atoms.maxHorz, "_NET_WM_STATE_MAXIMIZED_HORZ" }, { atoms.fullscreenState, "_NET_WM_STATE_FULLSCREEN" },
                 { atoms.moveResize, "_NET_WM_MOVERESIZE" }, { atoms.clipboard, "CLIPBOARD" }, { atoms.targets, "TARGETS" },
                 { atoms.transfer, "CCC_CLIPBOARD" }, { atoms.hidden, "_NET_WM_STATE_HIDDEN" }, { atoms.dndAware, "XdndAware" },
                 { atoms.dndEnter, "XdndEnter" }, { atoms.dndPosition, "XdndPosition" }, { atoms.dndStatus, "XdndStatus" }, { atoms.dndDrop, "XdndDrop" },
                 { atoms.dndFinished, "XdndFinished" }, { atoms.dndSelection, "XdndSelection" }, { atoms.dndCopy, "XdndActionCopy" },
                 { atoms.uriList, "text/uri-list" }, { atoms.drop, "CCC_DROP" }, { atoms.tabSocketAtom, "CCC_TAB_SOCKET" } };
    }

    void post(std::function<void()> f) {
        { std::lock_guard<std::mutex> l(queueMutex); queued.push_back(std::move(f)); }
        uint64_t one = 1;
        ssize_t n = write(wake, &one, sizeof one);
        (void)n;
    }

    template <class F>
    auto onEventThread(F f) -> decltype(f()) {
        std::packaged_task<decltype(f())()> task(std::move(f));
        auto result = task.get_future();
        post([&task] { task(); });
        return result.get();
    }

    static sockaddr_un tabAddress(XWindow w, socklen_t& len) {
        sockaddr_un a = {};
        a.sun_family = AF_UNIX;
        std::string name = "ccc-tab-" + std::to_string(w);
        std::memcpy(a.sun_path + 1, name.data(), name.size());
        len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        return a;
    }

    void listenForTabs() {
        tabSocket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        socklen_t len = 0;
        sockaddr_un address = tabAddress(window, len);
        if (tabSocket < 0 || bind(tabSocket, reinterpret_cast<sockaddr*>(&address), len) != 0 || listen(tabSocket, 4) != 0) return;
        long one = 1;
        XChangeProperty(display, window, atoms.tabSocketAtom, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&one), 1);
    }

    void receiveTab() {
        auto local = [this](int x, int y) {
            int lx = 0, ly = 0;
            XWindow child = 0;
            XTranslateCoordinates(display, DefaultRootWindow(display), window, x, y, &lx, &ly, &child);
            return std::pair<int, int>(lx, ly);
        };
        tabsocket::take(tabSocket, *inputs, [&](int x, int y) {
            auto [lx, ly] = local(x, y);
            dropSlot.set(-1);
            return ly >= 0 && ly < strip().height ? strip().tabAt(lx) : -1;
        }, [&](int x, int y) {
            if (x == INT32_MIN) { if (dropSlot.set(-1)) requestFrame(); return; }
            auto [lx, ly] = local(x, y);
            if (dropSlot.set(DropSlot::at(strip(), lx, ly))) requestFrame();
        });
    }

    // While a torn-off tab is over another ccc window, that window shows where it will land.
    void hoverDrop(int x, int y) {
        XWindow target = cccWindowAt(x, y);
        if (target != dropTarget) endDrop();
        dropTarget = target;
        if (!target) return;
        socklen_t len = 0;
        sockaddr_un address = tabAddress(target, len);
        tabsocket::hover(address, len, SOCK_SEQPACKET, x, y);
    }

    void endDrop() {
        if (!dropTarget) return;
        socklen_t len = 0;
        sockaddr_un address = tabAddress(std::exchange(dropTarget, (XWindow)0), len);
        tabsocket::hover(address, len, SOCK_SEQPACKET, INT32_MIN, INT32_MIN);
    }

    XWindow cccWindowAt(int x, int y) {
        XWindow root = DefaultRootWindow(display), parent = 0, *children = nullptr, found = 0;
        unsigned n = 0;
        if (!XQueryTree(display, root, &root, &parent, &children, &n)) return 0;
        for (unsigned i = n; i-- > 0;) {
            XWindowAttributes a;
            if (children[i] == outlineWindow || !XGetWindowAttributes(display, children[i], &a) || a.map_state != IsViewable || x < a.x || y < a.y || x >= a.x + a.width ||
                y >= a.y + a.height)
                continue;
            found = tabWindowIn(children[i], 2);
            break;
        }
        if (children) XFree(children);
        return found == window ? 0 : found;
    }

    XWindow outlineWindow = 0;
    bool outlineReady = false;
    int grabX = 0, grabY = 0;

    void showDragOutline(int rootX, int rootY) {
        if (!outlineReady) {
            DragOutline o = DragOutline::make(strip(), dpi);
            XVisualInfo vi;
            if (!o.width || !o.height || !XMatchVisualInfo(display, DefaultScreen(display), 32, TrueColor, &vi)) return;
            XWindow root = DefaultRootWindow(display);
            if (outlineWindow) XDestroyWindow(display, outlineWindow);
            XSetWindowAttributes a = {};
            a.colormap = XCreateColormap(display, root, vi.visual, AllocNone);
            a.override_redirect = True;
            outlineWindow = XCreateWindow(display, root, rootX, rootY, (unsigned)o.width, (unsigned)o.height, 0, 32, InputOutput, vi.visual,
                                          CWColormap | CWBorderPixel | CWBackPixel | CWOverrideRedirect, &a);
            Pixmap picture = XCreatePixmap(display, outlineWindow, (unsigned)o.width, (unsigned)o.height, 32);
            XImage* image = XCreateImage(display, vi.visual, 32, ZPixmap, 0, reinterpret_cast<char*>(o.pixels.data()), (unsigned)o.width, (unsigned)o.height, 32, 0);
            GC gc = XCreateGC(display, picture, 0, nullptr);
            XPutImage(display, picture, gc, image, 0, 0, 0, 0, (unsigned)o.width, (unsigned)o.height);
            image->data = nullptr;
            XDestroyImage(image);
            XFreeGC(display, gc);
            XSetWindowBackgroundPixmap(display, outlineWindow, picture);
            XFreePixmap(display, picture);
            outlineReady = true;
        }
        XMoveWindow(display, outlineWindow, rootX, rootY);
        XMapRaised(display, outlineWindow);
    }

    void hideDragOutline() {
        if (outlineWindow) XUnmapWindow(display, outlineWindow);
        outlineReady = false;
    }

    XWindow tabWindowIn(XWindow w, int depth) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        XGetWindowProperty(display, w, atoms.tabSocketAtom, 0, 1, False, AnyPropertyType, &type, &format, &count, &after, &data);
        if (data) XFree(data);
        if (type) return w;
        XWindow root = 0, parent = 0, *children = nullptr, found = 0;
        unsigned n = 0;
        if (depth > 0 && XQueryTree(display, w, &root, &parent, &children, &n))
            for (unsigned i = n; i-- > 0 && !found;) found = tabWindowIn(children[i], depth - 1);
        if (children) XFree(children);
        return found;
    }

    void runQueued() {
        std::vector<std::function<void()>> now;
        { std::lock_guard<std::mutex> l(queueMutex); now.swap(queued); }
        for (auto& f : now) f();
    }

    unsigned readDpi() {
        XrmInitialize();
        const char* resources = XResourceManagerString(display);
        if (!resources) return 96;
        XrmDatabase db = XrmGetStringDatabase(resources);
        XrmValue v;
        char* type = nullptr;
        unsigned result = 96;
        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &v) && v.addr) result = (unsigned)atof(v.addr);
        XrmDestroyDatabase(db);
        return result ? result : 96;
    }

    void applyTitle(const std::string& t) {
        XChangeProperty(display, window, atoms.netName, atoms.utf8, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(t.data()), (int)t.size());
        XStoreName(display, window, t.c_str());
    }

    void urgent(bool on) {
        XWMHints* h = XGetWMHints(display, window);
        XWMHints fresh = {};
        XWMHints* use = h ? h : &fresh;
        use->flags = on ? use->flags | XUrgencyHint : use->flags & ~XUrgencyHint;
        XSetWMHints(display, window, use);
        if (h) XFree(h);
    }

    Cursor cursorFor(Pointer p) {
        static const unsigned shapes[] = { XC_xterm, XC_left_ptr, XC_hand2, XC_crosshair, XC_watch, XC_watch, XC_question_arrow, XC_X_cursor,
                                           XC_fleur, XC_sb_h_double_arrow, XC_sb_v_double_arrow, XC_bottom_left_corner, XC_bottom_right_corner };
        static Cursor made[std::size(shapes)] = {};
        Cursor& c = made[(int)p];
        if (!c) c = XCreateFontCursor(display, shapes[(int)p]);
        return c;
    }

    Cursor blankCursor() {
        static Cursor blank = 0;
        if (blank) return blank;
        char bits = 0;
        Pixmap pm = XCreateBitmapFromData(display, window, &bits, 1, 1);
        XColor black = {};
        blank = XCreatePixmapCursor(display, pm, pm, &black, &black, 0, 0);
        XFreePixmap(display, pm);
        return blank;
    }

    void applyPointer(int y = -1, int x = -1) {
        if (pointerHidden) { XDefineCursor(display, window, blankCursor()); return; }
        bool chrome = (y >= 0 && y < strip().height) || (x >= 0 && x >= width - scrollbarWidth);
        XDefineCursor(display, window, cursorFor(chrome ? Pointer::Arrow : pointer.load()));
    }

    void sendState(long action, Atom first, Atom second) {
        XEvent e = {};
        e.xclient.type = ClientMessage;
        e.xclient.window = window;
        e.xclient.message_type = atoms.netState;
        e.xclient.format = 32;
        e.xclient.data.l[0] = action;
        e.xclient.data.l[1] = (long)first;
        e.xclient.data.l[2] = (long)second;
        e.xclient.data.l[3] = 1;
        XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    }

    void moveResize(int rootX, int rootY, int direction) {
        XUngrabPointer(display, CurrentTime);
        XEvent e = {};
        e.xclient.type = ClientMessage;
        e.xclient.window = window;
        e.xclient.message_type = atoms.moveResize;
        e.xclient.format = 32;
        e.xclient.data.l[0] = rootX;
        e.xclient.data.l[1] = rootY;
        e.xclient.data.l[2] = direction;
        e.xclient.data.l[3] = Button1;
        e.xclient.data.l[4] = 1;
        XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    }

    int edgeAt(int x, int y) const {
        if (maximizedNow || fullscreen) return -1;
        int border = (int)(6 * dpi / 96);
        bool left = x < border, right = x >= width - border, top = y < border, bottom = y >= height - border;
        if (top && left) return 0;
        if (top && right) return 2;
        if (bottom && right) return 4;
        if (bottom && left) return 6;
        if (top) return 1;
        if (right) return 3;
        if (bottom) return 5;
        if (left) return 7;
        return -1;
    }

    void deliver(InputEvent e) {
        inputs->push(e);
        requestFrame();
    }

    void postTab(InputEvent::Kind kind, int value = 0) {
        InputEvent e{ kind };
        e.value = value;
        deliver(e);
    }

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

    static unsigned keyState(unsigned mods, int vk, bool enhanced) {
        unsigned s = 0;
        if (mods & ShiftMask) s |= SHIFT_PRESSED;
        if (mods & ControlMask) s |= LEFT_CTRL_PRESSED;
        if (mods & Mod1Mask) s |= LEFT_ALT_PRESSED;
        if (mods & Mod2Mask) s |= NUMLOCK_ON;
        if (mods & LockMask) s |= CAPSLOCK_ON;
        if (enhanced) s |= ENHANCED_KEY;
        (void)vk;
        return s;
    }

    static int vkOf(KeySym k, bool& enhanced) {
        enhanced = false;
        if (k >= XK_a && k <= XK_z) return 'A' + (int)(k - XK_a);
        if (k >= XK_A && k <= XK_Z) return 'A' + (int)(k - XK_A);
        if (k >= XK_0 && k <= XK_9) return '0' + (int)(k - XK_0);
        if (k >= XK_F1 && k <= XK_F24) return VK_F1 + (int)(k - XK_F1);
        if (k >= XK_KP_0 && k <= XK_KP_9) return VK_NUMPAD0 + (int)(k - XK_KP_0);
        static const struct { KeySym sym; int vk; bool enhanced; } table[] = {
            { XK_BackSpace, VK_BACK, false }, { XK_Tab, VK_TAB, false }, { XK_ISO_Left_Tab, VK_TAB, false }, { XK_Return, VK_RETURN, false },
            { XK_KP_Enter, VK_RETURN, true }, { XK_Escape, VK_ESCAPE, false }, { XK_space, VK_SPACE, false }, { XK_Delete, VK_DELETE, true },
            { XK_Insert, VK_INSERT, true }, { XK_Home, VK_HOME, true }, { XK_End, VK_END, true }, { XK_Prior, VK_PRIOR, true }, { XK_Next, VK_NEXT, true },
            { XK_Left, VK_LEFT, true }, { XK_Up, VK_UP, true }, { XK_Right, VK_RIGHT, true }, { XK_Down, VK_DOWN, true }, { XK_Shift_L, VK_SHIFT, false },
            { XK_Shift_R, VK_SHIFT, false }, { XK_Control_L, VK_CONTROL, false }, { XK_Control_R, VK_CONTROL, true }, { XK_Alt_L, VK_MENU, false },
            { XK_Alt_R, VK_MENU, true }, { XK_Meta_L, VK_MENU, false }, { XK_Super_L, VK_LWIN, true }, { XK_Super_R, VK_RWIN, true },
            { XK_Caps_Lock, VK_CAPITAL, false }, { XK_Num_Lock, VK_NUMLOCK, true }, { XK_Scroll_Lock, VK_SCROLL, false }, { XK_Pause, VK_PAUSE, false },
            { XK_Print, VK_SNAPSHOT, true }, { XK_Menu, VK_APPS, true }, { XK_KP_Add, VK_ADD, false }, { XK_KP_Subtract, VK_SUBTRACT, false },
            { XK_KP_Multiply, VK_MULTIPLY, false }, { XK_KP_Divide, VK_DIVIDE, true }, { XK_KP_Decimal, VK_DECIMAL, false },
            { XK_KP_Separator, VK_SEPARATOR, false }, { XK_KP_Home, VK_HOME, false }, { XK_KP_End, VK_END, false }, { XK_KP_Up, VK_UP, false },
            { XK_KP_Down, VK_DOWN, false }, { XK_KP_Left, VK_LEFT, false }, { XK_KP_Right, VK_RIGHT, false }, { XK_KP_Prior, VK_PRIOR, false },
            { XK_KP_Next, VK_NEXT, false }, { XK_KP_Insert, VK_INSERT, false }, { XK_KP_Delete, VK_DELETE, false }, { XK_semicolon, VK_OEM_1, false },
            { XK_equal, VK_OEM_PLUS, false }, { XK_plus, VK_OEM_PLUS, false }, { XK_comma, VK_OEM_COMMA, false }, { XK_minus, VK_OEM_MINUS, false },
            { XK_period, VK_OEM_PERIOD, false }, { XK_slash, VK_OEM_2, false }, { XK_grave, VK_OEM_3, false }, { XK_bracketleft, VK_OEM_4, false },
            { XK_backslash, VK_OEM_5, false }, { XK_bracketright, VK_OEM_6, false }, { XK_apostrophe, VK_OEM_7, false },
        };
        for (auto& t : table) if (t.sym == k) { enhanced = t.enhanced; return t.vk; }
        return 0;
    }

    bool tabKey(int vk, unsigned mods) {
        bool ctrl = mods & ControlMask, shift = mods & ShiftMask, alt = mods & Mod1Mask;
        if (!ctrl || alt) return false;
        if (vk == VK_TAB) postTab(InputEvent::TabCycle, shift ? -1 : 1);
        else if (vk == 'T') postTab(shift ? InputEvent::TabReopen : InputEvent::TabNew, 1);
        else if (vk == 'W') postTab(InputEvent::TabClose, -1);
        else if (!shift && vk >= '1' && vk <= '9') postTab(InputEvent::TabSelect, vk == '9' ? (std::max)((int)tabCount - 1, 0) : vk - '1');
        else if (shift && vk == 'N') postTab(InputEvent::TabNew);
        else return false;
        return true;
    }

    void key(XKeyEvent& k, bool release) {
        KeySym base = XkbKeycodeToKeysym(display, (KeyCode)k.keycode, 0, 0);
        char text[64] = {};
        KeySym sym = 0;
        int n = 0;
        if (!release) {
            Status status;
            n = ic ? Xutf8LookupString(ic, &k, text, sizeof text - 1, &sym, &status) : XLookupString(&k, text, sizeof text - 1, &sym, nullptr);
            if (ic && status != XLookupChars && status != XLookupBoth) n = 0;
        }
        bool enhanced = false;
        int vk = vkOf(base, enhanced);
        if (!vk && sym) vk = vkOf(sym, enhanced);
        if (!release && vk == VK_ESCAPE && menuView().open) { setMenu(Menu()); return; }
        if (!release && vk == VK_RETURN && (k.state & Mod1Mask)) { toggleFullscreen(); return; }
        if (!release && tabKey(vk, k.state)) return;
        if (!release && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && !pointerHidden) { pointerHidden = true; applyPointer(); }
        std::string s(text, (size_t)(std::max)(n, 0));
        size_t at = 0;
        uint32_t ch = s.empty() ? 0 : unicode::decodeUtf8(s, at);
        InputEvent e{ InputEvent::Key, vk, 0, 0, (k.state & ControlMask) != 0, (k.state & ShiftMask) != 0, std::string(), (k.state & Mod1Mask) != 0 };
        e.release = release;
        e.repeat = !release && lastPressed == k.keycode;
        lastPressed = release ? 0 : k.keycode;
        e.scan = (int)k.keycode - 8;
        e.state = keyState(k.state, vk, enhanced);
        e.ch = vk ? ch : 0;
        deliver(e);
        if (!vk && ch) {
            for (at = 0; at < s.size();) deliver({ InputEvent::Char, (int)unicode::decodeUtf8(s, at) });
        }
    }

    unsigned lastPressed = 0;
    std::mutex menuMutex;
    Menu menu;
    bool swallowRelease = false;

    int cellOfX(int x) const { return x / (std::max)((int)cellW, 1); }
    int cellOfY(int y) const { return (y - strip().height) / (std::max)((int)cellH, 1); }

    void setMenu(const Menu& m) {
        { std::lock_guard<std::mutex> l(menuMutex); menu = m; }
        requestFrame();
    }

    bool menuPress(XButtonEvent& b, bool release) {
        if (release && std::exchange(swallowRelease, false)) return true;
        Menu m = menuView();
        if (!m.open || release) return false;
        int item = m.itemAt(cellOfX(b.x), cellOfY(b.y));
        setMenu(Menu());
        if (item >= 0) deliver({ m.items[item].action, m.items[item].value });
        swallowRelease = true;
        return true;
    }

    void stripPress(int x, int y, int rootX, int rootY, unsigned button) {
        Strip s = strip();
        int b = s.button(x);
        if (b >= 0 && button == Button1) { setButtons(b, b); return; }
        int tab = s.tabAt(x);
        if (button == Button2) { if (tab >= 0) postTab(InputEvent::TabClose, tab); return; }
        if (button != Button1) return;
        if (tab >= 0 && !s.onClose(x)) {
            postTab(InputEvent::TabSelect, tab);
            dragTab = tab;
            dragX = x;
            dragY = y;
            grabX = x - s.tabLeft(tab);
            grabY = y - s.tabTop;
            dragging = false;
            return;
        }
        if (tab >= 0) { postTab(InputEvent::TabClose, tab); return; }
        if (s.onNew(x)) { postTab(InputEvent::TabNew); return; }
        moveResize(rootX, rootY, 8);
    }

    void pressButton(int b) {
        if (b == 0) XIconifyWindow(display, window, DefaultScreen(display));
        else if (b == 1) sendState(2, atoms.maxVert, atoms.maxHorz);
        else if (b == 2) quitting = true;
    }

    void toggleFullscreen() { sendState(2, atoms.fullscreenState, 0); }

    void dragStripTab(int x) {
        Strip s = strip();
        int to = std::clamp((std::max)(x - s.x0, 0) / ((std::max)(s.tabCols, 1) * s.cellW), 0, (std::max)(s.count - 1, 0));
        if (to == dragTab) return;
        deliver({ InputEvent::TabMove, dragTab, to });
        dragTab = to;
    }

    void button(XButtonEvent& b, bool release) {
        if (menuPress(b, release)) return;
        int inset = strip().height, x = b.x, y = b.y;
        InputEvent e{ InputEvent::Move, 0, x, y - inset, (b.state & ControlMask) != 0, (b.state & ShiftMask) != 0 };
        e.state = keyState(b.state, 0, false);
        if (b.button == Button4 || b.button == Button5) {
            if (release || y < inset) return;
            e.kind = InputEvent::Wheel;
            e.value = b.button == Button4 ? WHEEL_DELTA : -WHEEL_DELTA;
            deliver(e);
            return;
        }
        if (b.button > Button3) return;
        unsigned bit = 1u << b.button;
        if (!release) {
            int edge = b.button == Button1 ? edgeAt(x, y) : -1;
            if (edge >= 0) { moveResize(b.x_root, b.y_root, edge); return; }
            if (y < inset && !buttonsHeld) { stripPress(x, y, b.x_root, b.y_root, b.button); return; }
            buttonsHeld |= bit;
            if (b.button == Button1) {
                bool near = b.time - clickTime < 400 && std::abs(x - clickX) < 5 && std::abs(y - clickY) < 5;
                clicks = near ? clicks + 1 : 1;
                clickTime = b.time;
                clickX = x;
                clickY = y;
                e.kind = clicks == 1 ? InputEvent::LeftDown : InputEvent::LeftDouble;
                e.value = clicks >= 3 ? 3 : 0;
            }
            else e.kind = b.button == Button2 ? InputEvent::MiddleDown : InputEvent::RightDown;
            deliver(e);
            return;
        }
        if (dragTab >= 0 && b.button == Button1) {
            int tab = std::exchange(dragTab, -1);
            hideDragOutline();
            endDrop();
            if (std::exchange(dragging, false) && strip().tornOff(x, y, width)) deliver({ InputEvent::TabTear, tab, b.x_root, b.y_root });
            return;
        }
        if (pressedButton >= 0 && b.button == Button1) {
            int pressed = pressedButton.load();
            setButtons(strip().button(x), -1);
            if (y < inset && strip().button(x) == pressed) pressButton(pressed);
            return;
        }
        if (b.button == Button3 && y < inset && !buttonsHeld && strip().tabAt(x) >= 0) {
            setMenu(Menu::forTab(strip().tabAt(x), tabCount, closedCount, cellOfX(x)));
            return;
        }
        if (!(buttonsHeld & bit)) return;
        buttonsHeld &= ~bit;
        e.kind = b.button == Button1 ? InputEvent::LeftUp : b.button == Button2 ? InputEvent::MiddleUp : InputEvent::RightUp;
        deliver(e);
    }

    void motion(XMotionEvent& m) {
        int inset = strip().height, x = m.x, y = m.y;
        if (pointerHidden) { pointerHidden = false; }
        if (Menu m = menuView(); m.open) {
            int hover = m.itemAt(cellOfX(x), cellOfY(y));
            if (hover != m.hover) { m.hover = hover; setMenu(m); }
            return;
        }
        if (dragTab >= 0) {
            if (!dragging && std::abs(x - dragX) < 4 && std::abs(y - dragY) < 4) return;
            dragging = true;
            if (strip().tornOff(x, y, width)) { showDragOutline(m.x_root - grabX, m.y_root - grabY); hoverDrop(m.x_root, m.y_root); }
            else { hideDragOutline(); endDrop(); dragStripTab(x); }
            return;
        }
        applyPointer(y, x);
        Strip s = strip();
        bool inStrip = y < inset && !buttonsHeld;
        int tab = inStrip ? s.tabAt(x) : -1;
        setHover(tab, tab >= 0 && s.onClose(x), inStrip && s.onNew(x));
        if (pressedButton < 0) setButtons(inStrip ? s.button(x) : -1, -1);
        if (buttonsHeld) {
            InputEvent e{ InputEvent::Move, 0, x, y - inset, (m.state & ControlMask) != 0, (m.state & ShiftMask) != 0 };
            e.state = keyState(m.state, 0, false);
            deliver(e);
            return;
        }
        bool ctrl = (m.state & ControlMask) && !inStrip;
        int at = inStrip ? -1 : (y - inset) * 65536 + x;
        if (ctrl != hoverCtrl || at != hoverAt) {
            InputEvent e{ InputEvent::Hover, 0, x, y - inset, ctrl };
            e.state = keyState(m.state, 0, false);
            deliver(e);
        }
        hoverCtrl = ctrl;
        hoverAt = at;
    }

    void readState() {
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char* data = nullptr;
        maximizedNow = fullscreen = false;
        if (XGetWindowProperty(display, window, atoms.netState, 0, 64, False, XA_ATOM, &type, &format, &count, &after, &data) != Success || !data) return;
        Atom* list = reinterpret_cast<Atom*>(data);
        bool vert = false, horz = false;
        for (unsigned long i = 0; i < count; ++i) {
            vert |= list[i] == atoms.maxVert;
            horz |= list[i] == atoms.maxHorz;
            fullscreen |= list[i] == atoms.fullscreenState;
        }
        maximizedNow = vert && horz;
        XFree(data);
        requestFrame();
    }

    void answerSelection(XSelectionRequestEvent& r) {
        XEvent reply = {};
        reply.xselection.type = SelectionNotify;
        reply.xselection.requestor = r.requestor;
        reply.xselection.selection = r.selection;
        reply.xselection.target = r.target;
        reply.xselection.time = r.time;
        reply.xselection.property = r.property ? r.property : r.target;
        if (r.target == atoms.targets) {
            Atom offered[] = { atoms.targets, atoms.utf8, XA_STRING };
            XChangeProperty(display, r.requestor, reply.xselection.property, XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(offered), 3);
        }
        else if (r.target == atoms.utf8 || r.target == XA_STRING)
            XChangeProperty(display, r.requestor, reply.xselection.property, r.target, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(clipText.data()), (int)clipText.size());
        else reply.xselection.property = None;
        XSendEvent(display, r.requestor, False, 0, &reply);
    }

    void dndMessage(XClientMessageEvent& m) {
        XWindow source = (XWindow)m.data.l[0];
        if (m.message_type == atoms.dndEnter) dndSource = source;
        else if (m.message_type == atoms.dndPosition) sendDnd(source, atoms.dndStatus, 1, (long)atoms.dndCopy);
        else if (m.message_type == atoms.dndDrop) XConvertSelection(display, atoms.dndSelection, atoms.uriList, atoms.drop, window, (Time)m.data.l[2]);
    }

    void sendDnd(XWindow to, Atom type, long accept, long action) {
        XEvent e = {};
        e.xclient.type = ClientMessage;
        e.xclient.window = to;
        e.xclient.message_type = type;
        e.xclient.format = 32;
        e.xclient.data.l[0] = (long)window;
        e.xclient.data.l[1] = accept;
        e.xclient.data.l[type == atoms.dndStatus ? 4 : 2] = action;
        XSendEvent(display, to, False, NoEventMask, &e);
    }

    static std::string droppedPaths(const std::string& uris) {
        std::string out;
        size_t at = 0;
        while (at < uris.size()) {
            size_t end = uris.find_first_of("\r\n", at);
            std::string line = uris.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? uris.size() : end + 1;
            if (line.empty() || line[0] == '#') continue;
            std::string path = urlPath(line);
            bool quote = path.find_first_of(" &()[]{}^=;!'+,`~") != std::string::npos;
            out += (out.empty() ? "" : " ") + (quote ? "\"" + path + "\"" : path);
        }
        return out;
    }

    XWindow dndSource = 0;

    void receiveSelection(XSelectionEvent& s) {
        std::string text;
        if (s.property) {
            Atom type;
            int format;
            unsigned long count, after;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, window, s.property, 0, 1 << 24, True, AnyPropertyType, &type, &format, &count, &after, &data) == Success && data) {
                text.assign(reinterpret_cast<const char*>(data), count);
                XFree(data);
            }
        }
        if (s.property == atoms.drop) {
            if (!text.empty()) deliver({ InputEvent::Text, 0, 0, 0, false, false, droppedPaths(text) });
            sendDnd(dndSource, atoms.dndFinished, 1, (long)atoms.dndCopy);
            return;
        }
        auto waiting = std::exchange(pasteWaiters, {});
        for (auto& done : waiting) done(text);
    }

    void handle(XEvent& e) {
        switch (e.type) {
        case KeyPress: key(e.xkey, false); break;
        case KeyRelease: {
            if (XEventsQueued(display, QueuedAfterReading)) {
                XEvent next;
                XPeekEvent(display, &next);
                if (next.type == KeyPress && next.xkey.time == e.xkey.time && next.xkey.keycode == e.xkey.keycode) break;
            }
            key(e.xkey, true);
            break;
        }
        case ButtonPress: button(e.xbutton, false); break;
        case ButtonRelease: button(e.xbutton, true); break;
        case MotionNotify: motion(e.xmotion); break;
        case LeaveNotify: setHover(-1, false, false); setButtons(-1, -1); break;
        case FocusIn:
        case FocusOut:
            focused = e.type == FocusIn;
            if (focused) urgent(false);
            if (ic) { if (focused) XSetICFocus(ic); else XUnsetICFocus(ic); }
            deliver({ InputEvent::Focus, focused ? 1 : 0 });
            break;
        case ConfigureNotify:
            width = e.xconfigure.width;
            height = e.xconfigure.height;
            requestFrame();
            break;
        case Expose: requestFrame(); break;
        case PropertyNotify: if (e.xproperty.atom == atoms.netState) readState(); break;
        case SelectionRequest: answerSelection(e.xselectionrequest); break;
        case SelectionNotify: receiveSelection(e.xselection); break;
        case ClientMessage:
            if ((Atom)e.xclient.data.l[0] == atoms.deleteWindow) quitting = true;
            else dndMessage(e.xclient);
            break;
        }
    }
};
