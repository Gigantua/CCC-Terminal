#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <mutex>

inline std::mutex frameMutex;
inline std::condition_variable frameSignal;
inline std::atomic<bool> framePending{ false };
inline std::atomic<bool> contentChanged{ true };

constexpr unsigned NO_LIMIT = UINT_MAX;

inline void wakeFrame() {
    if (framePending.exchange(true)) return;
    std::lock_guard<std::mutex> l(frameMutex);
    frameSignal.notify_all();
}

inline void requestFrame() {
    contentChanged = true;
    wakeFrame();
}

inline double secondsNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr double BLINK_PERIOD = 1.0, BLINK_ON = 0.6;

inline bool blinkPhaseOn(double t) { return std::fmod(t, BLINK_PERIOD) < BLINK_ON; }

inline unsigned millisToBlinkEdge(double t) {
    double phase = std::fmod(t, BLINK_PERIOD);
    double edge = phase < BLINK_ON ? BLINK_ON - phase : BLINK_PERIOD - phase;
    return (unsigned)std::ceil(edge * 1000.0);
}

inline void waitForChange(bool blinking, unsigned limitMs = NO_LIMIT) {
    if (framePending.exchange(false)) return;
    unsigned wait = blinking ? (std::min)(millisToBlinkEdge(secondsNow()), limitMs) : limitMs;
    std::unique_lock<std::mutex> l(frameMutex);
    auto ready = [] { return framePending.load(); };
    if (wait == NO_LIMIT) frameSignal.wait(l, ready);
    else frameSignal.wait_for(l, std::chrono::milliseconds(wait), ready);
    framePending = false;
}
