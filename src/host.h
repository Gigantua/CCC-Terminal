#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include "../shell/ccc.h"
#include "pacing.h"

inline std::string utf8FromWide(const std::wstring& w) {
    std::u16string units(w.begin(), w.end());
    return os::fromUtf16(units.data(), units.size());
}

struct WindowHost : ccc::Host {
    Console con;
    ccc::ConsoleServer srv{ con };
    std::function<void(const std::string&)> onTitle;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> lines;
    std::atomic<bool> waiting{ false }, wantKey{ false }, closed{ false }, exited{ true }, cancel{ false };
    std::atomic<os::Handle> child{ os::NO_HANDLE };
    std::atomic<uint64_t> linesTaken{ 0 };
    std::atomic<uint32_t> fg{ TERM_FG }, bg{ TERM_BG };
    std::atomic<int> rows{ 25 }, cols{ 80 };
    const std::atomic<bool>* atPrompt = nullptr;
    static constexpr float MIN_POINTS = 5, MAX_POINTS = 72;

    bool readLine(std::string& line) override {
        if (atPrompt && *atPrompt) con.markPrompt();
        return take(line, os::WAIT_FOREVER, false);
    }

    int readKey(unsigned long timeoutMs) override {
        std::string key;
        if (!take(key, timeoutMs, true)) return -1;
        return key.empty() ? 0 : (unsigned char)key[0];
    }

    bool take(std::string& out, unsigned long timeoutMs, bool key) {
        std::unique_lock<std::mutex> lock(m);
        waiting = true;
        wantKey = key;
        if (!key) wakeFrame();
        auto ready = [&] { return !lines.empty() || closed || cancel; };
        bool got = true;
        if (timeoutMs == os::WAIT_FOREVER) cv.wait(lock, ready);
        else got = cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
        waiting = false;
        wantKey = false;
        if (cancel) { cancel = false; throw ccc::Interrupt(); }
        if (!got || lines.empty()) return false;
        out = std::move(lines.front());
        lines.pop_front();
        if (!key) ++linesTaken;
        return true;
    }

    void waitKey() override { readKey(os::WAIT_FOREVER); }

    std::string windowTitle() override { std::lock_guard<std::mutex> l(titleMutex); return title; }

    void showTitle(const std::string& t) {
        { std::lock_guard<std::mutex> l(titleMutex); title = t; }
        if (onTitle) onTitle(t);
    }

    std::vector<std::string> history() override { std::lock_guard<std::mutex> l(historyMutex); return historyLines; }
    void clearHistory() override { std::lock_guard<std::mutex> l(historyMutex); historyLines.clear(); }
    void addHistory(const std::string& line) {
        std::lock_guard<std::mutex> l(historyMutex);
        if (!line.empty() && (historyLines.empty() || historyLines.back() != line)) historyLines.push_back(line);
    }
    size_t historyCount() { std::lock_guard<std::mutex> l(historyMutex); return historyLines.size(); }
    std::string historyAt(size_t i) { std::lock_guard<std::mutex> l(historyMutex); return i < historyLines.size() ? historyLines[i] : std::string(); }

    std::atomic<bool> insertMode{ true };
    void setInsertMode(bool insert) override { insertMode = insert; requestFrame(); }

    bool resizeScreen(int c, int r) override {
        if (c > 1000 || r > 1000) return false;
        requestedCols = c;
        requestedRows = r;
        requestFrame();
        return true;
    }
    bool takeResize(int& c, int& r) {
        c = requestedCols.exchange(0);
        r = requestedRows.exchange(0);
        return c > 0 && r > 0;
    }

    void cancelInput() override {
        { std::lock_guard<std::mutex> lock(m); if (!waiting) return; cancel = true; }
        cv.notify_all();
    }

    void submit(const std::string& line) {
        { std::lock_guard<std::mutex> lock(m); lines.push_back(line); }
        cv.notify_one();
    }

    void close() {
        { std::lock_guard<std::mutex> lock(m); closed = true; }
        cv.notify_all();
    }

    void setTitle(const std::string& t) override { showTitle(t); }

    void setColor(int a) override {
        static const uint32_t rgb[16] = { 0x0C0C0C, 0x0037DA, 0x13A10E, 0x3A96DD, 0xC50F1F, 0x881798, 0xC19C00, 0xCCCCCC,
                                          0x767676, 0x3B78FF, 0x16C60C, 0x61D6D6, 0xE74856, 0xB4009E, 0xF9F1A5, 0xF2F2F2 };
        auto abgr = [](uint32_t c) { return 0xFF000000u | (c & 255) << 16 | (c & 0xFF00) | (c >> 16 & 255); };
        bg = abgr(rgb[(a >> 4) & 15]);
        fg = abgr(rgb[a & 15]);
        requestFrame();
    }

    void clear() override { con.clear(); }
    Console* console() override { return &con; }
    ccc::ConsoleServer* server() override { return srv.started() ? &srv : nullptr; }
    void childInput(os::Handle h) override { child = h; }
    int screenRows() override { return rows; }
    int screenColumns() override { return cols; }

    bool setFont(const std::string& family, float points) override {
        {
            std::lock_guard<std::mutex> l(fontMutex);
            if (!family.empty()) requestedFamily = family;
            if (points > 0) requestedPoints = std::clamp(points, MIN_POINTS, MAX_POINTS);
            fontDirty = true;
        }
        requestFrame();
        return true;
    }

    void resizeFont(float delta) {
        std::lock_guard<std::mutex> l(fontMutex);
        requestedPoints = std::clamp(requestedPoints + delta, MIN_POINTS, MAX_POINTS);
        fontDirty = true;
    }


    bool takeFontRequest(std::string& family, float& points) {
        std::lock_guard<std::mutex> l(fontMutex);
        if (!fontDirty) return false;
        fontDirty = false;
        family = requestedFamily;
        points = requestedPoints;
        return true;
    }

    void fontApplied(const std::string& family, float points) {
        std::lock_guard<std::mutex> l(fontMutex);
        activeFamily = family;
        if (points > 0) requestedPoints = points;
    }

    std::string fontInfo() override {
        std::lock_guard<std::mutex> l(fontMutex);
        char size[32];
        snprintf(size, sizeof(size), "%g", requestedPoints);
        return (requestedFamily.empty() ? activeFamily : requestedFamily) + ", " + size + " pt";
    }

    bool setLineWrap(bool on) override { con.lineWrap = on; requestFrame(); return true; }
    int lineWrapState() override { return con.lineWrap ? 1 : 0; }

private:
    std::mutex historyMutex;
    std::vector<std::string> historyLines;
    std::atomic<int> requestedCols{ 0 }, requestedRows{ 0 };
    std::mutex titleMutex;
    std::string title = "ccc";
    std::mutex fontMutex;
    std::string requestedFamily, activeFamily;
    float requestedPoints = 12;
    bool fontDirty = false;
};
