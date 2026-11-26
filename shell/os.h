#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace os {

using FileTime = std::chrono::system_clock::time_point;
using EnvList = std::vector<std::string>;

constexpr uint32_t ATTR_READONLY = 0x1, ATTR_HIDDEN = 0x2, ATTR_SYSTEM = 0x4, ATTR_DIRECTORY = 0x10, ATTR_ARCHIVE = 0x20,
                   ATTR_TEMPORARY = 0x100, ATTR_REPARSE = 0x400, ATTR_COMPRESSED = 0x800, ATTR_OFFLINE = 0x1000,
                   ATTR_NOT_INDEXED = 0x2000, ATTR_INVALID = 0xFFFFFFFF;

constexpr uint32_t COPY_RESTARTABLE = 0x2, COPY_SYMLINK = 0x800, COPY_DECRYPTED = 0x8;

constexpr unsigned long WAIT_FOREVER = 0xFFFFFFFF;
constexpr int KEY_TIMEOUT = -1, KEY_CANCELLED = -2;
constexpr uint32_t PIPE_BYTES = 16 << 20;

struct FileInfo {
    std::string name, shortName;
    uint32_t attr = 0;
    uint64_t size = 0;
    FileTime written, created, accessed;
    bool junction = false;
};

struct Clock { int year = 0, month = 0, day = 0, weekday = 0, hour = 0, minute = 0, second = 0, millis = 0; };

struct Volume { std::string drive, label; uint32_t serial = 0; };

struct Mapped { std::shared_ptr<void> keep; const char* data = nullptr; size_t size = 0; };

enum class Show { Normal, Minimized, Maximized };
enum class Priority { Default, Low, BelowNormal, Normal, AboveNormal, High, Realtime };
enum class ReadResult { Data, End, Cancelled };

struct Launch {
    std::string exe, cmdline, title, dir;
    Show show = Show::Normal;
    Priority priority = Priority::Default;
    long long node = -1;
    unsigned long long affinity = 0;
    bool detached = false, newConsole = true, wait = false;
    const EnvList* env = nullptr;
};

}

#ifdef _WIN32
#include "os_windows.h"
#else
#include "os_posix.h"
#endif
