#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>
#include "../shell/os.h"

enum class Msg : uint32_t { Input, Size, Metrics, Ready, Close, Frame, Title, Font, Resize, Print, Exit, Cwd, Bell, Progress, Detach, Detached, Attach, Notify, Image, ImageDrop, ClipboardSet, ClipboardGet, Open };

class Packet {
public:
    explicit Packet(Msg m) { put((uint32_t)m); put((uint32_t)0); }

    template <class T> Packet& put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        append(&v, sizeof v);
        return *this;
    }
    Packet& put(const std::string& s) { put((uint32_t)s.size()); append(s.data(), s.size()); return *this; }
    Packet& put(const std::u32string& s) { put((uint32_t)s.size()); append(s.data(), s.size() * sizeof(char32_t)); return *this; }
    template <class T> Packet& put(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        put((uint32_t)v.size());
        append(v.data(), v.size() * sizeof(T));
        return *this;
    }

    const std::vector<char>& bytes() {
        uint32_t body = (uint32_t)(data.size() - 8);
        std::memcpy(data.data() + 4, &body, 4);
        return data;
    }

private:
    std::vector<char> data;
    void append(const void* p, size_t n) { data.insert(data.end(), (const char*)p, (const char*)p + n); }
};

class Unpacker {
public:
    explicit Unpacker(const std::vector<char>& body) : p(body.data()), end(body.data() + body.size()) {}

    template <class T> T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        take(&v, sizeof v);
        return v;
    }
    template <class S> S text() {
        S s(get<uint32_t>(), typename S::value_type());
        take(s.data(), s.size() * sizeof(typename S::value_type));
        return s;
    }
    template <class T> void vec(std::vector<T>& v) {
        v.resize(get<uint32_t>());
        take(v.data(), v.size() * sizeof(T));
    }

private:
    const char* p;
    const char* end;
    void take(void* out, size_t n) {
        if ((size_t)(end - p) < n) { std::memset(out, 0, n); p = end; return; }
        std::memcpy(out, p, n);
        p += n;
    }
};

class Channel {
public:
    os::Handle in = os::NO_HANDLE, out = os::NO_HANDLE;

    bool send(Packet& packet) {
        const std::vector<char>& b = packet.bytes();
        std::lock_guard<std::mutex> l(writeMutex);
        if (held) { heldBytes.insert(heldBytes.end(), b.begin(), b.end()); return true; }
        return writeAll(b.data(), b.size());
    }

    bool sendAndHold(Packet& last) {
        const std::vector<char>& b = last.bytes();
        std::lock_guard<std::mutex> l(writeMutex);
        held = true;
        return writeAll(b.data(), b.size());
    }

    bool resume() {
        std::lock_guard<std::mutex> l(writeMutex);
        held = false;
        bool ok = writeAll(heldBytes.data(), heldBytes.size());
        heldBytes.clear();
        return ok;
    }

    bool receive(Msg& kind, std::vector<char>& body) {
        uint32_t header[2];
        if (!readAll(header, sizeof header)) return false;
        kind = (Msg)header[0];
        body.resize(header[1]);
        return readAll(body.data(), body.size());
    }

    void close() {
        os::close(in);
        os::close(out);
        in = out = os::NO_HANDLE;
    }

private:
    std::mutex writeMutex;
    bool held = false;
    std::vector<char> heldBytes;

    bool writeAll(const char* p, size_t n) {
        while (n) {
            size_t chunk = (std::min)(n, (size_t)1 << 20);
            if (!os::write(out, p, chunk)) return false;
            p += chunk;
            n -= chunk;
        }
        return true;
    }

    bool readAll(void* dst, size_t n) {
        char* p = static_cast<char*>(dst);
        while (n) {
            size_t done = os::read(in, p, (std::min)(n, (size_t)1 << 20));
            if (!done) return false;
            p += done;
            n -= done;
        }
        return true;
    }
};
