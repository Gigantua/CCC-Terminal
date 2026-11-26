#pragma once
#include "tabs.h"
#include "window.h"

inline void closeHandoff(TabHandoff& h) {
    os::close(h.in);
    os::close(h.out);
    os::closeProcess(h.process);
}

inline bool giveToNewWindow(const TabHandoff& h) {
    os::Spawn s;
    s.exe = os::selfPath();
    s.cmdline = quoteArg(s.exe) + " --adopt " + std::to_string(os::processId(h.process)) + " " + std::to_string(h.x) + " " + std::to_string(h.y);
    s.in = h.in;
    s.out = h.out;
    os::Process window;
    int error = 0;
    if (!os::spawn(s, window, error)) return false;
    Window::allowForeground(os::processId(window));
    os::closeProcess(window);
    return true;
}

inline TabHandoff adoptedHandoff(unsigned long pid, int x, int y) {
    TabHandoff h;
    h.in = os::stdHandle(0);
    h.out = os::stdHandle(1);
    h.process = os::openProcess(pid);
    h.x = x;
    h.y = y;
    return h;
}

inline void tearTab(Tabs& tabs, Window& win, int index, int x, int y) {
    bool joins = win.otherWindowAt(x, y);
    if (!joins && tabs.list.size() < 2) return;
    TabHandoff h;
    h.x = x;
    h.y = y;
    if (!tabs.take(index, h)) return;
    if (joins ? win.giveTab(h) : giveToNewWindow(h)) closeHandoff(h);
    else tabs.adopt(h, index);
}
