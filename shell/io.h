#pragma once
#include <memory>
#include <vector>
#include "console.h"
#include "output.h"
#include "util.h"

namespace ccc {

struct Sink {
    Console* con = nullptr;
    os::Handle h = os::NO_HANDLE;
    std::string* buf = nullptr;
    bool discard = false;
    OutputBuffer* out = nullptr;
    bool hasHandle() const { return h != os::NO_HANDLE; }
    void write(const char* p, size_t n) const {
        if (!n || discard) return;
        if (buf) buf->append(p, n);
        else if (out) out->write(p, n);
        else if (con) con->write(p, n);
        else if (hasHandle()) os::write(h, p, n);
    }
    void write(const std::string& s) const { write(s.data(), s.size()); }
    void line(const char* p, size_t n) const {
        if (out && !discard && !buf) out->writeLine(p, n);
        else { write(p, n); write("\r\n", 2); }
    }
    void line(const std::string& s) const { line(s.data(), s.size()); }
    void flush() const { if (out) out->flush(); }
    void redirect(const char* p, size_t n, std::shared_ptr<void> keep) const {
        if (con && !buf) { flush(); con->redirect(p, n, std::move(keep)); }
        else write(p, n);
    }
    void toHandle(os::Handle handle) { con = nullptr; buf = nullptr; out = nullptr; discard = false; h = handle; }
};

inline void pumpToSink(os::Handle from, const Sink& sink) {
    std::vector<char> buf(1 << 20);
    os::PipeReader reader(from, nullptr);
    while (size_t n = reader.read(buf.data(), buf.size())) sink.write(buf.data(), n);
}

struct Source {
    os::Handle h = os::NO_HANDLE;
    bool hasHandle() const { return h != os::NO_HANDLE; }
};

class ConsoleServer;

struct Host {
    virtual ~Host() {}
    virtual bool readLine(std::string& line) = 0;
    virtual void waitKey() = 0;
    virtual void setTitle(const std::string& t) = 0;
    virtual void setColor(int attr) = 0;
    virtual void clear() = 0;
    virtual Console* console() = 0;
    virtual void childInput(os::Handle stdinWrite) = 0;
    virtual int screenRows() { return 25; }
    virtual void cancelInput() {}
    virtual int screenColumns() { return 80; }
    virtual bool setFont(const std::string&, float) { return false; }
    virtual std::string fontInfo() { return ""; }
    virtual bool setLineWrap(bool) { return false; }
    virtual int lineWrapState() { return -1; }
    virtual int readKey(unsigned long timeoutMs) { (void)timeoutMs; return -1; }
    virtual std::string windowTitle() { return ""; }
    virtual std::vector<std::string> history() { return {}; }
    virtual void clearHistory() {}
    virtual bool resizeScreen(int cols, int rows) { (void)cols; (void)rows; return false; }
    virtual void setInsertMode(bool insert) { (void)insert; }
    virtual ConsoleServer* server() { return nullptr; }
};

inline void pumpToConsole(os::Handle from, Console& con, const os::Process* writer) {
    Console::Feed feed(con);
    os::PipeReader reader(from, writer);
    size_t capacity;
    char* p = feed.space(capacity);
    if (!reader.start(p, capacity)) return;
    for (;;) {
        size_t n = reader.finish();
        if (!n) break;
        auto keep = feed.take(n);
        char* next = feed.space(capacity);
        bool more = reader.start(next, capacity);
        con.adopt(p, n, keep);
        if (!more) break;
        p = next;
    }
}

struct ExitShell { int code; };
struct Interrupt {};

}
