#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "../shell/os.h"

struct InputEvent {
    enum Kind { Char, Key, LeftDown, LeftUp, LeftDouble, RightDown, Move, Hover, Wheel, Text, TabNew, TabClose, TabSelect, TabCycle, RightUp, MiddleDown, MiddleUp, Focus, TabMove, TabTear, TabCloseOthers, TabCloseRight, TabReopen, Clipboard } kind;
    int value = 0;
    int x = 0, y = 0;
    bool ctrl = false, shift = false;
    std::string text;
    bool alt = false;
    bool release = false, repeat = false;
    int scan = 0;
    unsigned state = 0;
    uint32_t ch = 0;
    uint32_t base = 0, shifted = 0;
    bool super = false;
};

struct TabHandoff {
    os::Handle in = os::NO_HANDLE, out = os::NO_HANDLE;
    os::Process process;
    int32_t x = 0, y = 0;
    int index = -1;
};

class InputQueue {
public:
    void push(const InputEvent& e) { std::lock_guard<std::mutex> l(m); events.push_back(e); }
    std::vector<InputEvent> take() { std::lock_guard<std::mutex> l(m); return std::exchange(events, {}); }
    void push(const TabHandoff& h) { std::lock_guard<std::mutex> l(m); handoffs.push_back(h); }
    std::vector<TabHandoff> takeHandoffs() { std::lock_guard<std::mutex> l(m); return std::exchange(handoffs, {}); }
private:
    std::mutex m;
    std::vector<InputEvent> events;
    std::vector<TabHandoff> handoffs;
};
