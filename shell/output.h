#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "console.h"

namespace ccc {

class OutputBuffer {
public:
    static constexpr size_t CAPACITY = 64 << 10;

    OutputBuffer(Console* c, os::Handle h) : con(c), handle(h), data(new char[CAPACITY]) { registry().add(this); }
    ~OutputBuffer() {
        registry().remove(this);
        flush();
    }
    OutputBuffer(const OutputBuffer&) = delete;
    OutputBuffer& operator=(const OutputBuffer&) = delete;

    void write(const char* p, size_t n) {
        if (lastWriter.load(std::memory_order_relaxed) != this) registry().flushOthers(this);
        lock.lock();
        if (used + n > CAPACITY) flushLocked();
        if (n >= CAPACITY) emit(p, n);
        else {
            std::memcpy(data.get() + used, p, n);
            used += n;
        }
        lock.unlock();
    }

    void writeLine(const char* p, size_t n) {
        if (n + 2 >= CAPACITY) { write(p, n); write("\r\n", 2); return; }
        if (lastWriter.load(std::memory_order_relaxed) != this) registry().flushOthers(this);
        lock.lock();
        if (used + n + 2 > CAPACITY) flushLocked();
        char* d = data.get() + used;
        std::memcpy(d, p, n);
        d[n] = '\r';
        d[n + 1] = '\n';
        used += n + 2;
        lock.unlock();
    }

    void flush() {
        lock.lock();
        flushLocked();
        lock.unlock();
    }

    static void flushAll() { registry().flushOthers(nullptr); }
    void setUtf16(bool on) { utf16 = on && !con; }

private:
    struct WriterLock {
        std::atomic<bool> held{ false };
        void lock() {
            for (int spins = 0; held.exchange(true, std::memory_order_acquire); ++spins)
                if (spins > 64) std::this_thread::yield(); else simd::pause();
        }
        void unlock() { held.store(false, std::memory_order_release); }
    };

    class Registry {
    public:
        Registry() { std::thread([this] { for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(8)); flushOthers(nullptr, false); } }).detach(); }

        void add(OutputBuffer* b) { std::lock_guard<std::mutex> l(m); buffers.push_back(b); }

        void remove(OutputBuffer* b) {
            {
                std::lock_guard<std::mutex> l(m);
                std::erase(buffers, b);
                OutputBuffer* self = b;
                lastWriter.compare_exchange_strong(self, nullptr);
            }
            while (b->users.load() != 0) std::this_thread::yield();
        }

        void flushOthers(OutputBuffer* writer, bool claim = true) {
            OutputBuffer* snapshot[64];
            size_t n = 0;
            {
                std::lock_guard<std::mutex> l(m);
                for (OutputBuffer* b : buffers)
                    if (b != writer && b->used != 0 && n < std::size(snapshot)) { b->users.fetch_add(1); snapshot[n++] = b; }
                if (claim) lastWriter = writer;
            }
            for (size_t i = 0; i < n; ++i) {
                snapshot[i]->flush();
                snapshot[i]->users.fetch_sub(1);
            }
        }

    private:
        std::mutex m;
        std::vector<OutputBuffer*> buffers;
    };

    static Registry& registry() { static Registry* r = new Registry(); return *r; }
    static inline std::atomic<OutputBuffer*> lastWriter{ nullptr };

    Console* con;
    os::Handle handle;
    std::unique_ptr<char[]> data;
    size_t used = 0;
    WriterLock lock;
    std::atomic<int> users{ 0 };
    bool utf16 = false;

    void flushLocked() {
        if (!used) return;
        size_t complete = utf16 ? completeUtf8(data.get(), used) : used;
        emit(data.get(), complete);
        std::memmove(data.get(), data.get() + complete, used - complete);
        used -= complete;
    }

    static size_t completeUtf8(const char* p, size_t n) {
        size_t i = n;
        int back = 0;
        while (i > 0 && back < 3 && ((unsigned char)p[i - 1] & 0xC0) == 0x80) { --i; ++back; }
        if (i == 0) return n;
        unsigned char lead = (unsigned char)p[i - 1];
        int need = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        return need > back ? i - 1 : n;
    }

    void emit(const char* p, size_t n) {
        if (con) con->write(p, n);
        else if (utf16) {
            std::u16string wide = os::utf16(p, n);
            os::write(handle, reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(char16_t));
        }
        else os::write(handle, p, n);
    }
};

}
