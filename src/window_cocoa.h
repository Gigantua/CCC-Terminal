#pragma once
#define Picture QuickDrawPicture
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#undef Picture
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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

extern char** environ;

class Window;

@interface CccView : NSView
@property(nonatomic, assign) Window* owner;
@end

@interface CccDelegate : NSObject <NSWindowDelegate, NSApplicationDelegate>
@property(nonatomic, assign) Window* owner;
@end

class Window {
public:
    static constexpr int TRAFFIC_LIGHTS = 78;
    std::atomic<int> width{ 0 }, height{ 0 };
    std::atomic<unsigned> dpi{ 96 };
    std::atomic<int> scrollbarWidth{ 12 };
    std::atomic<int> cellW{ 0 }, cellH{ 0 }, tabCount{ 1 }, closedCount{ 0 };
    std::atomic<int> hoverButton{ -1 }, pressedButton{ -1 };
    std::atomic<int> hoverTab{ -1 };
    std::atomic<bool> hoverClose{ false }, hoverPlus{ false }, focused{ true };

    ~Window() {
        if (tabSocket >= 0) unlink(tabPath(getpid()).c_str());
        if (tabSource) {
            dispatch_source_cancel(tabSource);
            dispatch_release(tabSource);
        }
        if (outlineWindow) { [outlineWindow close]; [outlineWindow release]; }
        if (window) [window close];
    }

    static bool loadVulkan() { return true; }

    static std::vector<const char*> surfaceExtensions() { return { VK_EXT_METAL_SURFACE_EXTENSION_NAME }; }

    static VkSurfaceKHR makeSurface(VkInstance instance, void* self) {
        Window* w = static_cast<Window*>(self);
        VkMetalSurfaceCreateInfoEXT sc = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
        sc.pLayer = w->layer;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        return vkCreateMetalSurfaceEXT(instance, &sc, nullptr, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
    }

    Strip strip() const {
        Strip s = Strip::make(width, dpi, cellW, cellH, tabCount, fullscreen ? 8 : TRAFFIC_LIGHTS, false);
        if (s.height && placedStrip.exchange(s.height) != s.height) {
            Window* self = const_cast<Window*>(this);
            self->post([self] { self->placeTrafficLights(); });
        }
        return s;
    }

    StripHover stripHover() const { return { hoverTab.load(), hoverClose.load(), hoverPlus.load() }; }

    bool maximized() const { return zoomed; }

    static unsigned systemDpi() { return 96; }

    static void allowForeground(unsigned long) {}

    Menu menuView() { std::lock_guard<std::mutex> l(menuMutex); return menu; }

    bool otherWindowAt(int x, int y) { return cccWindowAt(x, y) != 0; }

    bool giveTab(const TabHandoff& h) {
        pid_t target = cccWindowAt(h.x, h.y);
        if (!target) return false;
        socklen_t len = 0;
        sockaddr_un address = tabAddress(target, len);
        return tabsocket::give(address, len, SOCK_STREAM, h);
    }

    void alert(const std::string& text) {
        NSAlert* a = [[NSAlert alloc] init];
        a.messageText = @"ccc";
        a.informativeText = ns(text);
        [a runModal];
    }

    void finish(std::thread& render) { render.join(); }

    bool savedPlacement(WindowSettings& s) const {
        s.rect = placement;
        s.hasRect = !zoomed && !fullscreen;
        s.maximized = zoomed;
        return true;
    }

    void openUri(const std::string& uri) {
        post([uri] {
            NSURL* url = [NSURL URLWithString:ns(uri)];
            if (url) [[NSWorkspace sharedWorkspace] openURL:url];
        });
    }

    bool create(int clientW, int clientH, InputQueue& queue, const WindowSettings& saved) {
        inputs = &queue;
        adoptLoginEnvironment();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        delegate = [[CccDelegate alloc] init];
        delegate.owner = this;
        [NSApp setDelegate:delegate];
        buildMainMenu();
        NSRect content = NSMakeRect(0, 0, clientW, clientH);
        if (saved.hasRect) content = NSMakeRect(saved.rect.left, topToBottom(saved.rect.top, saved.rect.bottom - saved.rect.top), saved.rect.right - saved.rect.left, saved.rect.bottom - saved.rect.top);
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
        window = [[NSWindow alloc] initWithContentRect:content styleMask:style backing:NSBackingStoreBuffered defer:NO];
        if (!window) return false;
        window.releasedWhenClosed = NO;
        window.titlebarAppearsTransparent = YES;
        window.titleVisibility = NSWindowTitleHidden;
        window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        window.delegate = delegate;
        window.acceptsMouseMovedEvents = YES;
        window.backgroundColor = NSColor.blackColor;
        view = [[CccView alloc] initWithFrame:content];
        view.owner = this;
        view.wantsLayer = YES;
        layer = [[CAMetalLayer layer] retain];
        layer.contentsGravity = kCAGravityTopLeft;
        layer.opaque = YES;
        view.layer = layer;
        [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [view addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                           options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                             owner:view
                                                          userInfo:nil]];
        window.contentView = view;
        [window makeFirstResponder:view];
        if (!saved.hasRect) [window center];
        applyTitle("ccc");
        resized();
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        if (saved.maximized) [window zoom:nil];
        listenForTabs();
        return true;
    }

    void runMessageLoop() { [NSApp run]; }

    void requestClose() {
        post([] {
            [NSApp stop:nil];
            NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
            [NSApp postEvent:wake atStart:YES];
        });
    }

    void setTitle(const std::string& t) { post([this, t] { applyTitle(t); }); }

    void bell() { post([] { if (![NSApp isActive]) [NSApp requestUserAttention:NSInformationalRequest]; }); }

    void progress(int, int) {}

    void notify(const std::string& title, const std::string& body) {
        std::string t = title.empty() ? std::string("ccc") : title, b = body.empty() ? title : body;
        std::string script = "display notification " + appleScriptString(b) + " with title " + appleScriptString(t);
        std::thread([script] {
            pid_t pid;
            char* argv[] = { const_cast<char*>("osascript"), const_cast<char*>("-e"), const_cast<char*>(script.c_str()), nullptr };
            if (posix_spawn(&pid, "/usr/bin/osascript", nullptr, nullptr, argv, environ) == 0) waitpid(pid, nullptr, 0);
        }).detach();
    }

    void setPointer(Pointer p) {
        if (pointer.exchange(p) != p) post([this] { applyPointer(lastX, lastY); });
    }

    void resizeClient(int clientW, int clientH) {
        post([this, clientW, clientH] {
            if (zoomed || fullscreen) return;
            double k = window.backingScaleFactor;
            [window setContentSize:NSMakeSize(clientW / k, clientH / k)];
        });
    }

    void setClipboard(const std::string& text) {
        post([text] {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            [pb clearContents];
            [pb setString:ns(text) forType:NSPasteboardTypeString];
        });
    }

    void readClipboard(std::function<void(const std::string&)> done) {
        post([done] {
            NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
            done(s ? std::string(s.UTF8String) : std::string());
        });
    }

    void keyEvent(NSEvent* e, bool release) {
        NSEventModifierFlags f = e.modifierFlags;
        bool cmd = f & NSEventModifierFlagCommand, ctrl = f & NSEventModifierFlagControl, shift = f & NSEventModifierFlagShift, option = f & NSEventModifierFlagOption;
        bool enhanced = false;
        std::u32string base = codepoints([e charactersByApplyingModifiers:0]);
        std::u32string shiftedText = codepoints([e charactersByApplyingModifiers:NSEventModifierFlagShift]);
        int vk = vkOfKeyCode(e.keyCode, enhanced);
        if (!vk && !base.empty()) {
            char32_t c = base[0];
            bool unused = false;
            vk = c >= 'a' && c <= 'z' ? (int)(c - 32) : c < 128 ? vkForChar((uint32_t)c, unused) : 0;
        }
        if (!release && vk == VK_ESCAPE && menuView().open) { setMenu(Menu()); return; }
        if (!release && cmd && !ctrl && commandKey(vk, shift, option)) return;
        if (!release && cmd && ctrl && vk == 'F') { [window toggleFullScreen:nil]; return; }
        if (cmd) return;
        if (!release && ctrl && !option && tabKey(vk, shift)) return;
        if (!release && !isModifierVk(vk)) [NSCursor setHiddenUntilMouseMoves:YES];
        std::u32string typed = release ? std::u32string() : codepoints(e.characters);
        NSEventModifierFlags withoutOption = f & ~(NSEventModifierFlagOption | NSEventModifierFlagCommand);
        bool optionTypes = option && !ctrl && !typed.empty() && typed != codepoints([e charactersByApplyingModifiers:withoutOption]);
        if (option && !optionTypes && !release) typed = codepoints([e charactersByApplyingModifiers:withoutOption]);
        InputEvent k{ InputEvent::Key, vk, 0, 0, ctrl, shift, std::string(), option && !optionTypes };
        k.release = release;
        k.repeat = !release && e.isARepeat;
        k.scan = scanForVk(vk);
        k.state = modifierState(f, optionTypes, enhanced);
        k.ch = vk && !typed.empty() ? (uint32_t)typed[0] : 0;
        k.base = base.empty() ? 0 : (uint32_t)base[0];
        k.shifted = shiftedText.empty() ? 0 : (uint32_t)shiftedText[0];
        deliver(k);
        if (!vk) for (char32_t c : typed) deliver({ InputEvent::Char, (int)c });
    }

    void flagsEvent(NSEvent* e) {
        bool enhanced = false;
        int vk = vkOfKeyCode(e.keyCode, enhanced);
        NSEventModifierFlags bit = vk == VK_SHIFT ? NSEventModifierFlagShift : vk == VK_CONTROL ? NSEventModifierFlagControl : vk == VK_MENU ? NSEventModifierFlagOption : 0;
        if (!bit) return;
        InputEvent k{ InputEvent::Key, vk, 0, 0, (e.modifierFlags & NSEventModifierFlagControl) != 0, (e.modifierFlags & NSEventModifierFlagShift) != 0 };
        k.release = !(e.modifierFlags & bit);
        k.scan = scanForVk(vk);
        k.state = modifierState(e.modifierFlags, false, enhanced);
        deliver(k);
    }

    void mouseEvent(NSEvent* e, int button, bool release) {
        int x = 0, y = 0;
        pixelsOf(e, x, y);
        if (menuPress(x, y, release)) return;
        int inset = strip().height;
        bool ctrl = e.modifierFlags & NSEventModifierFlagControl, shift = e.modifierFlags & NSEventModifierFlagShift;
        InputEvent m{ InputEvent::Move, 0, x, y - inset, ctrl, shift };
        m.state = modifierState(e.modifierFlags, false, false);
        unsigned bit = 1u << button;
        if (!release) {
            if (y < inset && !buttonsHeld) { stripPress(e, x, y, button); return; }
            buttonsHeld |= bit;
            if (button == 1) {
                long clicks = e.clickCount;
                m.kind = clicks <= 1 ? InputEvent::LeftDown : InputEvent::LeftDouble;
                m.value = clicks >= 3 ? 3 : 0;
            }
            else m.kind = button == 2 ? InputEvent::MiddleDown : InputEvent::RightDown;
            deliver(m);
            return;
        }
        if (dragTab >= 0 && button == 1) {
            int tab = std::exchange(dragTab, -1);
            hideDragOutline();
            NSPoint screen = [NSEvent mouseLocation];
            if (std::exchange(dragging, false) && strip().tornOff(x, y, width)) deliver({ InputEvent::TabTear, tab, (int)screen.x, (int)topToBottom(screen.y, 0) });
            return;
        }
        if (button == 3 && y < inset && !buttonsHeld && strip().tabAt(x) >= 0) {
            setMenu(Menu::forTab(strip().tabAt(x), tabCount, closedCount, cellOfX(x)));
            return;
        }
        if (!(buttonsHeld & bit)) return;
        buttonsHeld &= ~bit;
        m.kind = button == 1 ? InputEvent::LeftUp : button == 2 ? InputEvent::MiddleUp : InputEvent::RightUp;
        deliver(m);
    }

    void motionEvent(NSEvent* e) {
        int x = 0, y = 0;
        pixelsOf(e, x, y);
        lastX = x;
        lastY = y;
        int inset = strip().height;
        if (Menu m = menuView(); m.open) {
            int hover = m.itemAt(cellOfX(x), cellOfY(y));
            if (hover != m.hover) { m.hover = hover; setMenu(m); }
            return;
        }
        if (dragTab >= 0) {
            if (!dragging && std::abs(x - dragX) < 4 && std::abs(y - dragY) < 4) return;
            dragging = true;
            if (strip().tornOff(x, y, width)) showDragOutline();
            else { hideDragOutline(); dragStripTab(x); }
            return;
        }
        applyPointer(x, y);
        Strip s = strip();
        bool inStrip = y < inset && !buttonsHeld;
        int tab = inStrip ? s.tabAt(x) : -1;
        setHover(tab, tab >= 0 && s.onClose(x), inStrip && s.onNew(x));
        bool ctrl = e.modifierFlags & NSEventModifierFlagControl;
        if (buttonsHeld) {
            InputEvent m{ InputEvent::Move, 0, x, y - inset, ctrl, (e.modifierFlags & NSEventModifierFlagShift) != 0 };
            m.state = modifierState(e.modifierFlags, false, false);
            deliver(m);
            return;
        }
        bool hoverCtrlNow = ctrl && !inStrip;
        int at = inStrip ? -1 : (y - inset) * 65536 + x;
        if (hoverCtrlNow != hoverCtrl || at != hoverAt) {
            InputEvent m{ InputEvent::Hover, 0, x, y - inset, hoverCtrlNow };
            m.state = modifierState(e.modifierFlags, false, false);
            deliver(m);
        }
        hoverCtrl = hoverCtrlNow;
        hoverAt = at;
    }

    void exitEvent() {
        setHover(-1, false, false);
        lastX = lastY = -1;
    }

    void wheelEvent(NSEvent* e) {
        int x = 0, y = 0;
        pixelsOf(e, x, y);
        int inset = strip().height;
        if (y < inset) return;
        bool precise = e.hasPreciseScrollingDeltas;
        double step = precise ? (std::max)((int)cellH, 1) : 1.0;
        wheelRest += precise ? e.scrollingDeltaY * window.backingScaleFactor : e.scrollingDeltaY;
        int units = (int)(wheelRest / step);
        if (!units) return;
        wheelRest -= units * step;
        InputEvent w{ InputEvent::Wheel, units * (precise ? WHEEL_DELTA / 3 : WHEEL_DELTA), x, y - inset, (e.modifierFlags & NSEventModifierFlagControl) != 0,
                      (e.modifierFlags & NSEventModifierFlagShift) != 0 };
        w.state = modifierState(e.modifierFlags, false, false);
        deliver(w);
    }

    void focusEvent(bool on) {
        focused = on;
        deliver({ InputEvent::Focus, on ? 1 : 0 });
    }

    void resized() {
        double k = window.backingScaleFactor;
        NSSize s = view.bounds.size;
        layer.contentsScale = k;
        dpi = (unsigned)std::lround(96 * k);
        width = (int)std::lround(s.width * k);
        height = (int)std::lround(s.height * k);
        zoomed = window.isZoomed;
        NSRect r = [window contentRectForFrameRect:window.frame];
        int top = (int)topToBottom(NSMaxY(r), 0);
        placement = { (int)r.origin.x, top, (int)(r.origin.x + r.size.width), top + (int)r.size.height };
        placeTrafficLights();
        requestFrame();
    }

    void fullscreenChanged(bool on) {
        fullscreen = on;
        resized();
    }

    void closeRequested() { requestClose(); }

    void dropped(NSArray<NSURL*>* urls) {
        std::string out;
        for (NSURL* u in urls) {
            if (!u.fileURL) continue;
            std::string path = u.path.UTF8String;
            bool quote = path.find_first_of(" &()[]{}^=;!'+,`~") != std::string::npos;
            out += (out.empty() ? "" : " ") + (quote ? "\"" + path + "\"" : path);
        }
        if (!out.empty()) deliver({ InputEvent::Text, 0, 0, 0, false, false, out });
    }

private:
    NSWindow* window = nil;
    CccView* view = nil;
    CAMetalLayer* layer = nil;
    CccDelegate* delegate = nil;
    InputQueue* inputs = nullptr;
    bool zoomed = false, fullscreen = false, hoverCtrl = false, dragging = false, swallowRelease = false;
    mutable std::atomic<int> placedStrip{ 0 };
    std::atomic<Pointer> pointer{ Pointer::Text };
    WindowSettings::Rect placement;
    int hoverAt = -1, dragTab = -1, dragX = 0, dragY = 0, lastX = -1, lastY = -1;
    unsigned buttonsHeld = 0;
    double wheelRest = 0;
    int tabSocket = -1;
    dispatch_source_t tabSource = nullptr;
    std::mutex menuMutex;
    Menu menu;

    static NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

    static std::u32string codepoints(NSString* s) {
        std::u32string out;
        if (!s) return out;
        std::string u = s.UTF8String ?: "";
        for (size_t i = 0; i < u.size();) out += (char32_t)unicode::decodeUtf8(u, i);
        return out;
    }

    static std::string appleScriptString(const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    }

    static double topToBottom(double y, double h) { return NSScreen.screens.firstObject.frame.size.height - y - h; }

    static std::string tabPath(pid_t pid) {
        char dir[1024] = {};
        if (!confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir)) std::strcpy(dir, "/tmp/");
        return std::string(dir) + "ccc-tab-" + std::to_string(pid);
    }

    static sockaddr_un tabAddress(pid_t pid, socklen_t& len) {
        sockaddr_un a = {};
        a.sun_family = AF_UNIX;
        std::string path = tabPath(pid);
        std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        len = (socklen_t)sizeof a;
        return a;
    }

    void listenForTabs() {
        tabSocket = socket(AF_UNIX, SOCK_STREAM, 0);
        if (tabSocket < 0) return;
        fcntl(tabSocket, F_SETFD, FD_CLOEXEC);
        socklen_t len = 0;
        sockaddr_un address = tabAddress(getpid(), len);
        unlink(address.sun_path);
        if (bind(tabSocket, reinterpret_cast<sockaddr*>(&address), len) != 0 || listen(tabSocket, 4) != 0) { close(tabSocket); tabSocket = -1; return; }
        tabSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)tabSocket, 0, dispatch_get_main_queue());
        int listening = tabSocket;
        dispatch_source_set_event_handler(tabSource, ^{ receiveTab(); });
        dispatch_source_set_cancel_handler(tabSource, ^{ close(listening); });
        dispatch_resume(tabSource);
    }

    void receiveTab() {
        tabsocket::take(tabSocket, *inputs, [this](int x, int y) {
            double k = window.backingScaleFactor;
            int lx = (int)std::lround((x - placement.left) * k), ly = (int)std::lround((y - placement.top) * k);
            return ly >= 0 && ly < strip().height ? strip().tabAt(lx) : -1;
        });
    }

    static pid_t cccWindowAt(int x, int y) {
        CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (!list) return 0;
        pid_t found = 0;
        for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
            NSDictionary* info = (NSDictionary*)CFArrayGetValueAtIndex(list, i);
            if ([info[(id)kCGWindowLayer] intValue] != 0) continue;
            CGRect bounds;
            if (!CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)info[(id)kCGWindowBounds], &bounds) || !CGRectContainsPoint(bounds, CGPointMake(x, y))) continue;
            pid_t owner = [info[(id)kCGWindowOwnerPID] intValue];
            if (owner != getpid() && access(tabPath(owner).c_str(), F_OK) == 0) found = owner;
            break;
        }
        CFRelease(list);
        return found;
    }

    static void adoptLoginEnvironment() {
        os::kernel::importLoginEnvironment();
        char cwd[4096];
        const char* home = getenv("HOME");
        if (home && getcwd(cwd, sizeof cwd) && std::strcmp(cwd, "/") == 0 && chdir(home) != 0) {}
        if (!getenv("LANG")) setenv("LANG", "en_US.UTF-8", 1);
        std::string path = getenv("PATH") ? getenv("PATH") : "";
        auto add = [&](const std::string& dir) {
            if (dir.empty() || (":" + path + ":").find(":" + dir + ":") != std::string::npos) return;
            path = path.empty() ? dir : dir + ":" + path;
        };
        std::vector<std::string> dirs;
        auto readList = [&](const std::string& file) {
            std::ifstream in(file);
            for (std::string line; std::getline(in, line);) if (!line.empty()) dirs.push_back(line);
        };
        readList("/etc/paths");
        NSArray<NSString*>* extra = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/etc/paths.d" error:nil];
        for (NSString* name in [extra sortedArrayUsingSelector:@selector(compare:)]) readList(std::string("/etc/paths.d/") + name.UTF8String);
        dirs.push_back("/opt/homebrew/bin");
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) add(*it);
        setenv("PATH", path.c_str(), 1);
    }

    void post(std::function<void()> f) {
        dispatch_async(dispatch_get_main_queue(), ^{ f(); });
    }

    void buildMainMenu() {
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [bar addItem:appItem];
        NSMenu* app = [[NSMenu alloc] init];
        [app addItemWithTitle:@"Hide ccc" action:@selector(hide:) keyEquivalent:@"h"];
        [app addItem:[NSMenuItem separatorItem]];
        [app addItemWithTitle:@"Quit ccc" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = app;
        NSMenuItem* windowItem = [[NSMenuItem alloc] init];
        [bar addItem:windowItem];
        NSMenu* windows = [[NSMenu alloc] initWithTitle:@"Window"];
        [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        windowItem.submenu = windows;
        NSApp.mainMenu = bar;
        NSApp.windowsMenu = windows;
    }

    void applyTitle(const std::string& t) { window.title = ns(t); }

    void placeTrafficLights() {
        int stripH = placedStrip.load();
        if (!stripH || fullscreen) return;
        double k = window.backingScaleFactor, centre = stripH / k / 2;
        for (NSWindowButton b : { NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton }) {
            NSButton* button = [window standardWindowButton:b];
            NSView* bar = button.superview;
            if (!bar) continue;
            NSRect f = button.frame;
            double y = bar.bounds.size.height - centre - f.size.height / 2;
            f.origin.y = (std::max)(0.0, y);
            if (b == NSWindowCloseButton) f.origin.x = 14;
            else if (b == NSWindowMiniaturizeButton) f.origin.x = 34;
            else f.origin.x = 54;
            [button setFrameOrigin:f.origin];
        }
    }

    void pixelsOf(NSEvent* e, int& x, int& y) const {
        NSPoint p = [view convertPoint:e.locationInWindow fromView:nil];
        double k = window.backingScaleFactor;
        x = (int)std::floor(p.x * k);
        y = (int)std::floor(p.y * k);
    }

    NSCursor* cursorFor(Pointer p) const {
        switch (p) {
        case Pointer::Text: return NSCursor.IBeamCursor;
        case Pointer::Hand: return NSCursor.pointingHandCursor;
        case Pointer::Cross: return NSCursor.crosshairCursor;
        case Pointer::Forbidden: return NSCursor.operationNotAllowedCursor;
        case Pointer::Move: return NSCursor.openHandCursor;
        case Pointer::ResizeWE: return NSCursor.resizeLeftRightCursor;
        case Pointer::ResizeNS: return NSCursor.resizeUpDownCursor;
        default: return NSCursor.arrowCursor;
        }
    }

    void applyPointer(int x, int y) {
        if (x < 0 || y < 0) return;
        bool chrome = y < strip().height || x >= width - scrollbarWidth;
        [chrome ? NSCursor.arrowCursor : cursorFor(pointer.load()) set];
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

    void pressKey(int vk, bool ctrl, bool shift) {
        InputEvent k{ InputEvent::Key, vk, 0, 0, ctrl, shift };
        k.scan = scanForVk(vk);
        k.state = (ctrl ? LEFT_CTRL_PRESSED : 0) | (shift ? SHIFT_PRESSED : 0);
        deliver(k);
    }

    bool commandKey(int vk, bool shift, bool option) {
        if (vk >= '1' && vk <= '9' && !shift) { postTab(InputEvent::TabSelect, vk == '9' ? (std::max)((int)tabCount - 1, 0) : vk - '1'); return true; }
        if ((shift && (vk == VK_OEM_4 || vk == VK_OEM_6)) || (option && (vk == VK_LEFT || vk == VK_RIGHT))) {
            postTab(InputEvent::TabCycle, vk == VK_OEM_4 || vk == VK_LEFT ? -1 : 1);
            return true;
        }
        switch (vk) {
        case 'T': postTab(shift ? InputEvent::TabReopen : InputEvent::TabNew, 1); return true;
        case 'W': postTab(InputEvent::TabClose, -1); return true;
        case 'N': openWindow(); return true;
        case 'C': pressKey(VK_INSERT, true, false); return true;
        case 'V': pressKey(VK_INSERT, false, true); return true;
        case 'F': pressKey('F', true, true); return true;
        case VK_OEM_PLUS: case VK_ADD: pressKey(VK_OEM_PLUS, true, false); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT: pressKey(VK_OEM_MINUS, true, false); return true;
        case '0': pressKey('0', true, false); return true;
        case VK_UP: case VK_DOWN: pressKey(vk, true, true); return true;
        default: return false;
        }
    }

    bool tabKey(int vk, bool shift) {
        if (vk == VK_TAB) postTab(InputEvent::TabCycle, shift ? -1 : 1);
        else if (shift && vk == 'T') postTab(InputEvent::TabReopen, 1);
        else if (shift && vk == 'N') postTab(InputEvent::TabNew);
        else if (shift && vk == 'W') postTab(InputEvent::TabClose, -1);
        else return false;
        return true;
    }

    static void openWindow() {
        std::string self = os::selfPath();
        pid_t pid;
        char* argv[] = { const_cast<char*>(self.c_str()), nullptr };
        if (posix_spawn(&pid, self.c_str(), nullptr, nullptr, argv, environ) == 0) std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    }

    static bool isModifierVk(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL || vk == VK_LWIN || vk == VK_RWIN; }

    static unsigned modifierState(NSEventModifierFlags f, bool optionTypes, bool enhanced) {
        unsigned s = 0;
        if (f & NSEventModifierFlagShift) s |= SHIFT_PRESSED;
        if (f & NSEventModifierFlagControl) s |= LEFT_CTRL_PRESSED;
        if (f & NSEventModifierFlagOption) s |= optionTypes ? RIGHT_ALT_PRESSED : LEFT_ALT_PRESSED;
        if (f & NSEventModifierFlagCapsLock) s |= CAPSLOCK_ON;
        if (enhanced) s |= ENHANCED_KEY;
        return s;
    }

    static int vkOfKeyCode(unsigned short code, bool& enhanced) {
        enhanced = false;
        static const struct { unsigned short code; int vk; bool enhanced; } table[] = {
            { 0x24, VK_RETURN, false }, { 0x4C, VK_RETURN, true }, { 0x30, VK_TAB, false }, { 0x31, VK_SPACE, false }, { 0x33, VK_BACK, false },
            { 0x35, VK_ESCAPE, false }, { 0x75, VK_DELETE, true }, { 0x72, VK_INSERT, true }, { 0x73, VK_HOME, true }, { 0x77, VK_END, true },
            { 0x74, VK_PRIOR, true }, { 0x79, VK_NEXT, true }, { 0x7B, VK_LEFT, true }, { 0x7C, VK_RIGHT, true }, { 0x7D, VK_DOWN, true },
            { 0x7E, VK_UP, true }, { 0x7A, VK_F1, false }, { 0x78, VK_F2, false }, { 0x63, VK_F3, false }, { 0x76, VK_F4, false },
            { 0x60, VK_F5, false }, { 0x61, VK_F6, false }, { 0x62, VK_F7, false }, { 0x64, VK_F8, false }, { 0x65, VK_F9, false },
            { 0x6D, VK_F10, false }, { 0x67, VK_F11, false }, { 0x6F, VK_F12, false }, { 0x69, VK_F13, false }, { 0x6B, VK_F13 + 1, false },
            { 0x71, VK_F13 + 2, false }, { 0x6A, VK_F13 + 3, false }, { 0x40, VK_F13 + 4, false }, { 0x4F, VK_F13 + 5, false },
            { 0x50, VK_F13 + 6, false }, { 0x5A, VK_F13 + 7, false }, { 0x52, VK_NUMPAD0, false }, { 0x53, VK_NUMPAD0 + 1, false },
            { 0x54, VK_NUMPAD0 + 2, false }, { 0x55, VK_NUMPAD0 + 3, false }, { 0x56, VK_NUMPAD0 + 4, false }, { 0x57, VK_NUMPAD0 + 5, false },
            { 0x58, VK_NUMPAD0 + 6, false }, { 0x59, VK_NUMPAD0 + 7, false }, { 0x5B, VK_NUMPAD0 + 8, false }, { 0x5C, VK_NUMPAD9, false },
            { 0x41, VK_DECIMAL, false }, { 0x43, VK_MULTIPLY, false }, { 0x45, VK_ADD, false }, { 0x4B, VK_DIVIDE, true },
            { 0x4E, VK_SUBTRACT, false }, { 0x47, VK_NUMLOCK, true }, { 0x38, VK_SHIFT, false }, { 0x3C, VK_SHIFT, false },
            { 0x3B, VK_CONTROL, false }, { 0x3E, VK_CONTROL, true }, { 0x3A, VK_MENU, false }, { 0x3D, VK_MENU, true },
            { 0x37, VK_LWIN, true }, { 0x36, VK_RWIN, true }, { 0x39, VK_CAPITAL, false },
        };
        for (auto& t : table) if (t.code == code) { enhanced = t.enhanced; return t.vk; }
        return 0;
    }

    int cellOfX(int x) const { return x / (std::max)((int)cellW, 1); }
    int cellOfY(int y) const { return (y - strip().height) / (std::max)((int)cellH, 1); }

    void setMenu(const Menu& m) {
        { std::lock_guard<std::mutex> l(menuMutex); menu = m; }
        requestFrame();
    }

    bool menuPress(int x, int y, bool release) {
        if (release && std::exchange(swallowRelease, false)) return true;
        Menu m = menuView();
        if (!m.open || release) return false;
        int item = m.itemAt(cellOfX(x), cellOfY(y));
        setMenu(Menu());
        if (item >= 0) deliver({ m.items[item].action, m.items[item].value });
        swallowRelease = true;
        return true;
    }

    void setHover(int tab, bool close, bool plus) {
        bool changed = hoverTab.exchange(tab) != tab;
        changed |= hoverClose.exchange(close) != close;
        changed |= hoverPlus.exchange(plus) != plus;
        if (changed) requestFrame();
    }

    void stripPress(NSEvent* e, int x, int y, int button) {
        Strip s = strip();
        int tab = s.tabAt(x);
        if (button == 2) { if (tab >= 0) postTab(InputEvent::TabClose, tab); return; }
        if (button != 1) return;
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
        if (e.clickCount == 2) [window zoom:nil];
        else [window performWindowDragWithEvent:e];
    }

    NSWindow* outlineWindow = nil;
    bool outlineReady = false;
    int grabX = 0, grabY = 0;

    void showDragOutline() {
        double k = window.backingScaleFactor;
        if (!outlineReady) {
            DragOutline o = DragOutline::make(strip(), dpi);
            if (!o.width || !o.height) return;
            if (!outlineWindow) {
                outlineWindow = [[NSWindow alloc] initWithContentRect:NSZeroRect styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                outlineWindow.releasedWhenClosed = NO;
                outlineWindow.opaque = NO;
                outlineWindow.backgroundColor = NSColor.clearColor;
                outlineWindow.hasShadow = NO;
                outlineWindow.ignoresMouseEvents = YES;
                outlineWindow.level = NSPopUpMenuWindowLevel;
                outlineWindow.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
                outlineWindow.contentView.wantsLayer = YES;
            }
            CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
            CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(o.pixels.data()), (CFIndex)(o.pixels.size() * 4));
            CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
            CGImageRef image = CGImageCreate((size_t)o.width, (size_t)o.height, 8, 32, (size_t)o.width * 4, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
                                             provider, nullptr, false, kCGRenderingIntentDefault);
            outlineWindow.contentView.layer.contents = (id)image;
            outlineWindow.contentView.layer.contentsScale = k;
            [outlineWindow setContentSize:NSMakeSize(o.width / k, o.height / k)];
            CGImageRelease(image);
            CGDataProviderRelease(provider);
            CFRelease(data);
            CGColorSpaceRelease(space);
            outlineReady = true;
        }
        NSPoint mouse = [NSEvent mouseLocation];
        [outlineWindow setFrameTopLeftPoint:NSMakePoint(mouse.x - grabX / k, mouse.y + grabY / k)];
        [outlineWindow orderFrontRegardless];
    }

    void hideDragOutline() {
        if (outlineWindow) [outlineWindow orderOut:nil];
        outlineReady = false;
    }

    void dragStripTab(int x) {
        Strip s = strip();
        int to = std::clamp((std::max)(x - s.x0, 0) / ((std::max)(s.tabCols, 1) * s.cellW), 0, (std::max)(s.count - 1, 0));
        if (to == dragTab) return;
        deliver({ InputEvent::TabMove, dragTab, to });
        dragTab = to;
    }
};

@implementation CccView
- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)keyDown:(NSEvent*)e { self.owner->keyEvent(e, false); }
- (void)keyUp:(NSEvent*)e { self.owner->keyEvent(e, true); }
- (void)flagsChanged:(NSEvent*)e { self.owner->flagsEvent(e); }
- (BOOL)performKeyEquivalent:(NSEvent*)e {
    if (!(e.modifierFlags & NSEventModifierFlagCommand)) return NO;
    if (![[NSApp mainMenu] performKeyEquivalent:e]) self.owner->keyEvent(e, false);
    return YES;
}
- (void)mouseDown:(NSEvent*)e { self.owner->mouseEvent(e, 1, false); }
- (void)mouseUp:(NSEvent*)e { self.owner->mouseEvent(e, 1, true); }
- (void)rightMouseDown:(NSEvent*)e { self.owner->mouseEvent(e, 3, false); }
- (void)rightMouseUp:(NSEvent*)e { self.owner->mouseEvent(e, 3, true); }
- (void)otherMouseDown:(NSEvent*)e { if (e.buttonNumber == 2) self.owner->mouseEvent(e, 2, false); }
- (void)otherMouseUp:(NSEvent*)e { if (e.buttonNumber == 2) self.owner->mouseEvent(e, 2, true); }
- (void)mouseMoved:(NSEvent*)e { self.owner->motionEvent(e); }
- (void)mouseDragged:(NSEvent*)e { self.owner->motionEvent(e); }
- (void)rightMouseDragged:(NSEvent*)e { self.owner->motionEvent(e); }
- (void)otherMouseDragged:(NSEvent*)e { self.owner->motionEvent(e); }
- (void)mouseExited:(NSEvent*)e { self.owner->exitEvent(); }
- (void)scrollWheel:(NSEvent*)e { self.owner->wheelEvent(e); }
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (self.owner) self.owner->resized();
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.owner) self.owner->resized();
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    self.owner->dropped(urls);
    return YES;
}
@end

@implementation CccDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    self.owner->closeRequested();
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    self.owner->closeRequested();
    return NSTerminateCancel;
}
- (void)windowDidBecomeKey:(NSNotification*)n { self.owner->focusEvent(true); }
- (void)windowDidResignKey:(NSNotification*)n { self.owner->focusEvent(false); }
- (void)windowDidResize:(NSNotification*)n { self.owner->resized(); }
- (void)windowDidMove:(NSNotification*)n { self.owner->resized(); }
- (void)windowDidChangeBackingProperties:(NSNotification*)n { self.owner->resized(); }
- (void)windowDidEnterFullScreen:(NSNotification*)n { self.owner->fullscreenChanged(true); }
- (void)windowDidExitFullScreen:(NSNotification*)n { self.owner->fullscreenChanged(false); }
@end
