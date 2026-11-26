#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <iconv.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#ifdef __APPLE__
#include "os_macos.h"
#else
#include "os_linux.h"
#endif

extern char** environ;

namespace os {

constexpr unsigned UTF8_PAGE = 65001;
constexpr char SLASH = '/';
constexpr const char* SLASHES = "/";
constexpr const char* PATH_STOPS = "/";
constexpr char PATH_LIST_SEPARATOR = ':';
constexpr const char* DEFAULT_PATHEXT = ".bat;.cmd";
constexpr bool EXTENSIONLESS_PROGRAMS = true;
constexpr bool INDEX_PATH_DIRS = false;
constexpr bool CONSOLE_TOOLS_EXTERNAL = false;
constexpr const char* NEW_CONSOLE_SWITCH = "/c";
constexpr int INTERRUPTED_EXIT_CODE = 130;
constexpr int ERR_ALREADY_EXISTS = EEXIST, ERR_FILE_NOT_FOUND = ENOENT, ERR_ACCESS_DENIED = EACCES;

using Handle = int;
constexpr Handle NO_HANDLE = -1;

template <class It, class F>
void forEachParallel(It first, It last, F f) { std::for_each(first, last, f); }

inline unsigned defaultCodePage() { return UTF8_PAGE; }

inline bool stdRedirected() {
    for (int fd : { 0, 1 }) {
        struct stat s;
        if (fstat(fd, &s) == 0 && (S_ISREG(s.st_mode) || S_ISFIFO(s.st_mode) || S_ISSOCK(s.st_mode))) return true;
    }
    return false;
}

inline void ignoreConsoleInterrupts() { signal(SIGPIPE, SIG_IGN); }

inline uint32_t decodeByte(unsigned page, unsigned char b) {
    iconv_t cd = iconv_open("UTF-32LE", ("CP" + std::to_string(page)).c_str());
    if (cd == (iconv_t)-1) return '?';
    char in = (char)b;
    uint32_t out = '?';
    char* src = &in;
    char* dst = reinterpret_cast<char*>(&out);
    size_t srcLeft = 1, dstLeft = sizeof out;
    iconv(cd, &src, &srcLeft, &dst, &dstLeft);
    iconv_close(cd);
    return out;
}

inline bool codePageValid(unsigned page) {
    if (page == UTF8_PAGE) return true;
    iconv_t cd = iconv_open("UTF-32LE", ("CP" + std::to_string(page)).c_str());
    if (cd == (iconv_t)-1) return false;
    iconv_close(cd);
    return true;
}

inline std::u16string utf16(const char* p, size_t n) {
    std::u16string out;
    out.reserve(n);
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)p[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; ++k) {
            unsigned char t = (unsigned char)p[i + k];
            ok = (t & 0xC0) == 0x80;
            cp = cp << 6 | (t & 0x3F);
        }
        if (!ok) { out += u'\xFFFD'; ++i; continue; }
        i += len;
        if (cp < 0x10000) out += (char16_t)cp;
        else { cp -= 0x10000; out += (char16_t)(0xD800 | cp >> 10); out += (char16_t)(0xDC00 | (cp & 0x3FF)); }
    }
    return out;
}

inline std::string fromUtf16(const char16_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = p[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (p[++i] - 0xDC00);
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    return out;
}

inline int lastError() { return errno; }

inline std::string errorText(int code) {
    switch (code) {
    case ENOENT: return "The system cannot find the file specified.";
    case ENOTDIR: return "The system cannot find the path specified.";
    case EACCES: case EPERM: case EISDIR: case EROFS: return "Access is denied.";
    case EEXIST: return "Cannot create a file when that file already exists.";
    case ENOTEMPTY: return "The directory is not empty.";
    case EBUSY: case ETXTBSY: return "The process cannot access the file because it is being used by another process.";
    case EINVAL: return "The parameter is incorrect.";
    case ENAMETOOLONG: return "The filename, directory name, or volume label syntax is incorrect.";
    case EXDEV: return "The system cannot move the file to a different disk drive.";
    default: break;
    }
    return std::string(strerror(code)) + ".";
}

inline std::string cmdMessage(unsigned) { return {}; }

inline unsigned consoleOutputPage() { return UTF8_PAGE; }

inline std::string encodeText(const std::string& text, unsigned) { return text; }

inline std::string digitGroupSeparator() { return ","; }

inline int failWith(int code) { errno = code; return 0; }

inline Handle stdHandle(int fd) { return fd; }

inline bool isConsole(Handle h) { return isatty(h) != 0; }

inline Handle openFile(const std::string& path, int flags) {
    int fd;
    while ((fd = ::open(path.c_str(), flags | O_CLOEXEC, 0666)) < 0 && errno == EINTR) {}
    if (fd < 0 || (flags & O_ACCMODE) != O_RDONLY) return fd;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { ::close(fd); errno = EISDIR; return NO_HANDLE; }
    return fd;
}

inline Handle openRead(const std::string& path) { return openFile(path, O_RDONLY); }
inline Handle openWrite(const std::string& path, bool append) { return openFile(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)); }
inline Handle openNull() { return openFile("/dev/null", O_WRONLY); }

inline size_t read(Handle h, char* p, size_t n) {
    ssize_t r;
    while ((r = ::read(h, p, n)) < 0 && errno == EINTR) {}
    return r > 0 ? (size_t)r : 0;
}

inline bool write(Handle h, const char* p, size_t n) {
    while (n) {
        ssize_t w = ::write(h, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

inline void close(Handle h) { if (h >= 0) ::close(h); }

inline bool createPipe(Handle& read, Handle& write, uint32_t) {
    int fds[2];
    if (!kernel::makePipe(fds, false)) { read = write = NO_HANDLE; return false; }
    read = fds[0];
    write = fds[1];
    return true;
}

inline bool createReadPipe(Handle& read, Handle& write) { return createPipe(read, write, PIPE_BYTES); }

struct Process {
    pid_t pid = -1;
    bool waited = false;
    bool operator==(const Process& o) const { return pid == o.pid; }
};

struct Spawn {
    std::string exe, cmdline, dir;
    Handle in = NO_HANDLE, out = NO_HANDLE, err = NO_HANDLE;
    const EnvList* env = nullptr;
    bool hidden = false;
};

class PipeReader {
public:
    PipeReader(Handle pipe, const Process*) : from(pipe) {}
    bool start(char* p, size_t capacity) { next = p; room = capacity; return true; }
    size_t finish() {
        if (!next) return 0;
        char* p = next;
        next = nullptr;
        return os::read(from, p, room);
    }
    size_t read(char* p, size_t capacity) { return os::read(from, p, capacity); }
private:
    Handle from;
    char* next = nullptr;
    size_t room = 0;
};

inline FileTime toFileTime(const struct timespec& t) {
    return FileTime(std::chrono::duration_cast<FileTime::duration>(std::chrono::seconds(t.tv_sec) + std::chrono::nanoseconds(t.tv_nsec)));
}

inline uint32_t attributesOf(const std::string& name, const struct stat& link, const struct stat* target) {
    const struct stat& s = target ? *target : link;
    uint32_t a = 0;
    if (S_ISDIR(s.st_mode)) a |= ATTR_DIRECTORY;
    else a |= ATTR_ARCHIVE;
    if (S_ISLNK(link.st_mode)) a |= ATTR_REPARSE;
    if (!(s.st_mode & S_IWUSR)) a |= ATTR_READONLY;
    if (name.size() > 1 && name[0] == '.' && name != "..") a |= ATTR_HIDDEN;
    return a;
}

inline bool describe(const std::string& path, const std::string& name, FileInfo& f) {
    struct stat link, target;
    if (lstat(path.c_str(), &link) != 0) return false;
    bool isLink = S_ISLNK(link.st_mode);
    bool hasTarget = isLink && stat(path.c_str(), &target) == 0;
    const struct stat& s = hasTarget ? target : link;
    f.name = name;
    f.shortName.clear();
    f.attr = attributesOf(name, link, hasTarget ? &target : nullptr);
    f.size = isLink || S_ISDIR(s.st_mode) ? 0 : (uint64_t)s.st_size;
    f.written = toFileTime(kernel::modified(link));
    f.created = toFileTime(kernel::created(link));
    f.accessed = toFileTime(kernel::accessed(link));
    f.junction = false;
    return true;
}

inline bool wildcardMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') {
        while (p[1] == '*') ++p;
        for (const char* t = s;; ++t) {
            if (wildcardMatch(p + 1, t)) return true;
            if (!*t) return false;
        }
    }
    if (*p == '.' && p[1] == '*' && !p[2] && !*s) return true;
    if (*p == '.' && !p[1] && !*s) return true;
    if (!*s) return *p == '?' && wildcardMatch(p + 1, s);
    if (*p == '?' || *p == *s) return wildcardMatch(p + 1, s + 1);
    return false;
}

inline bool nameBefore(const std::string& a, const std::string& b) {
    auto up = [](char c) { return toupper((unsigned char)c); };
    if (std::ranges::lexicographical_compare(a, b, {}, up, up)) return true;
    return !std::ranges::lexicographical_compare(b, a, {}, up, up) && a < b;
}

inline std::vector<FileInfo> listFiles(const std::string& pattern, bool = false) {
    std::vector<FileInfo> out;
    size_t slash = pattern.rfind('/');
    std::string dir = slash == std::string::npos ? "" : pattern.substr(0, slash + 1), mask = pattern.substr(slash + 1);
    if (mask.empty()) return out;
    if (mask.find_first_of("*?") == std::string::npos) {
        FileInfo f;
        if (describe(pattern, mask, f)) out.push_back(f);
        return out;
    }
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (dir == "/" && (name == "." || name == "..")) continue;
        if (wildcardMatch(mask.c_str(), name.c_str())) names.push_back(name);
    }
    closedir(d);
    std::sort(names.begin(), names.end(), nameBefore);
    for (auto& name : names) {
        FileInfo f;
        if (describe(dir + name, name, f)) out.push_back(std::move(f));
    }
    return out;
}

inline bool findFile(const std::string& pattern, FileInfo& found) {
    auto all = listFiles(pattern);
    if (all.empty()) return failWith(ENOENT);
    found = all.front();
    return true;
}

inline uint32_t attributes(const std::string& path) {
    FileInfo f;
    std::string name = path.substr(path.find_last_of('/') + 1);
    return describe(path, name, f) ? f.attr : ATTR_INVALID;
}

inline bool fileInfo(const std::string& path, FileInfo& info) { return describe(path, path.substr(path.find_last_of('/') + 1), info); }

inline bool mapFile(const std::string& path, Mapped& m) {
    Handle h = openRead(path);
    if (h < 0) return false;
    struct stat st;
    m = Mapped();
    if (fstat(h, &st) != 0) { ::close(h); return false; }
    size_t size = (size_t)st.st_size;
    if (!size) { ::close(h); return true; }
    void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, h, 0);
    ::close(h);
    if (view == MAP_FAILED) return false;
    m.keep = std::shared_ptr<void>(view, [size](void* v) { munmap(v, size); });
    m.data = (const char*)view;
    m.size = size;
    return true;
}

inline bool makeWritable(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && chmod(path.c_str(), st.st_mode | S_IWUSR) == 0;
}
inline bool removeDirectory(const std::string& path) { return rmdir(path.c_str()) == 0; }
inline bool removeFile(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return failWith(EACCES);
    return unlink(path.c_str()) == 0;
}
inline bool renameFile(const std::string& from, const std::string& to) {
    struct stat st;
    if (lstat(to.c_str(), &st) == 0) return failWith(EEXIST);
    return ::rename(from.c_str(), to.c_str()) == 0;
}
inline bool moveFile(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    if (errno != EXDEV) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
    if (!ec) fs::remove_all(from, ec);
    return ec ? failWith(ec.value()) : true;
}
inline bool copyFile(const std::string& from, const std::string& to, uint32_t flags) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if ((flags & COPY_SYMLINK) && fs::is_symlink(fs::symlink_status(from, ec))) {
        fs::path target = fs::read_symlink(from, ec);
        if (!ec) { fs::remove(to, ec); fs::create_symlink(target, to, ec); }
        return ec ? failWith(ec.value()) : true;
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::last_write_time(to, fs::last_write_time(from, ec), ec);
    return ec ? failWith(ec.value()) : true;
}

inline std::string currentDirectory() {
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}
inline bool setCurrentDirectory(const std::string& path) { return chdir(path.c_str()) == 0; }

inline std::string fullPath(const std::string& p) {
    std::string in = p.empty() ? "." : p;
    std::string abs = in[0] == '/' ? in : currentDirectory() + "/" + in;
    bool trailing = abs.size() > 1 && abs.back() == '/';
    std::vector<std::string> parts;
    for (size_t a = 0; a <= abs.size();) {
        size_t b = abs.find('/', a);
        if (b == std::string::npos) b = abs.size();
        std::string part = abs.substr(a, b - a);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        a = b + 1;
    }
    std::string out;
    for (auto& part : parts) out += "/" + part;
    if (out.empty()) return "/";
    if (trailing) out += '/';
    return out;
}

inline bool junction(const std::string& link, const std::string& target) { return symlink(fullPath(target).c_str(), link.c_str()) == 0; }

inline std::string shortPath(const std::string& path) { return path; }
inline std::string driveOf(const std::string&) { return ""; }

inline std::string selfPath() { return kernel::executablePath(); }

inline std::string defaultComspec() { return selfPath(); }

inline std::string foldName(const std::string& name) { return name; }

inline bool isExecutable(const std::string& path) { return access(path.c_str(), X_OK) == 0; }

inline bool isGuiProgram(const std::string&) { return false; }

inline bool opensAsDocument(const std::string&) { return false; }

struct DirWatch {
    explicit DirWatch(const std::string&) {}
    bool active() const { return false; }
    bool changed() const { return true; }
};

inline bool volumeInfo(const std::string& path, Volume& v) {
    struct statvfs fs;
    struct stat st, up;
    std::string mount = fullPath(path);
    if (mount.size() > 1 && mount.back() == '/') mount.pop_back();
    auto parentOf = [](const std::string& p) { size_t slash = p.find_last_of('/', p.size() - 2); return slash == 0 ? std::string("/") : p.substr(0, slash); };
    while (stat(mount.c_str(), &st) != 0) {
        if (mount == "/") return false;
        mount = parentOf(mount);
    }
    if (statvfs(mount.c_str(), &fs) != 0) return false;
    while (mount != "/") {
        std::string parent = parentOf(mount);
        if (stat(parent.c_str(), &up) != 0 || up.st_dev != st.st_dev) break;
        mount = parent;
    }
    v.drive = mount;
    v.label.clear();
    v.serial = (uint32_t)fs.f_fsid;
    return true;
}

inline std::string fileOwner(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "...";
    struct passwd pw, *found = nullptr;
    char buf[4096];
    return getpwuid_r(st.st_uid, &pw, buf, sizeof buf, &found) == 0 && found ? std::string(found->pw_name) : std::to_string(st.st_uid);
}

inline std::vector<std::pair<std::string, uint64_t>> alternateStreams(const std::string&) { return {}; }

inline bool connectNetworkDrive(const std::string&, char&) { return failWith(ENOTSUP); }
inline void disconnectNetworkDrive(char) {}

inline EnvList environment() {
    EnvList out;
    for (char** e = environ; e && *e; ++e) out.push_back(*e);
    auto name = [](const std::string& s) { return s.substr(0, s.find('=')); };
    std::stable_sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) { return nameBefore(name(a), name(b)); });
    return out;
}

inline void setEnvironment(const std::string& name, const std::string* value) {
    if (name.empty() || name.find('=') != std::string::npos) return;
    if (value) setenv(name.c_str(), value->c_str(), 1);
    else unsetenv(name.c_str());
}

inline bool shellSetting(int, const char*, uint32_t&) { return false; }
inline std::string shellSettingText(int, const char*) { return ""; }

inline std::string classValue(const std::string&) { return ""; }
inline bool setClassValue(const std::string&, const std::string*) { return failWith(ENOTSUP); }
inline std::vector<std::string> classKeys() { return {}; }

inline Clock toClock(const struct tm& t, int millis) { return { t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_wday, t.tm_hour, t.tm_min, t.tm_sec, millis }; }

inline Clock localNow() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);
    return toClock(t, (int)(ts.tv_nsec / 1000000));
}

inline Clock localTime(FileTime when) {
    time_t s = std::chrono::system_clock::to_time_t(when);
    struct tm t;
    localtime_r(&s, &t);
    return toClock(t, 0);
}

inline struct tm toTm(const Clock& c) {
    struct tm t = {};
    t.tm_year = c.year - 1900; t.tm_mon = c.month - 1; t.tm_mday = c.day; t.tm_wday = c.weekday;
    t.tm_hour = c.hour; t.tm_min = c.minute; t.tm_sec = c.second; t.tm_isdst = -1;
    return t;
}

inline bool setLocalNow(const Clock& c) {
    struct tm t = toTm(c);
    struct timespec ts = { mktime(&t), (long)c.millis * 1000000 };
    return clock_settime(CLOCK_REALTIME, &ts) == 0;
}

inline std::string formatClock(const Clock& c, const char* format) {
    static locale_t loc = [] {
        locale_t l = newlocale(LC_TIME_MASK, "", (locale_t)0);
        return l ? l : newlocale(LC_TIME_MASK, "C", (locale_t)0);
    }();
    struct tm t = toTm(c);
    char buf[64];
    size_t n = strftime_l(buf, sizeof buf, format, &t, loc);
    return std::string(buf, n);
}

inline std::string shortDate(const Clock& c) { return formatClock(c, "%x"); }
inline std::string shortTime(const Clock& c) { return formatClock(c, "%H:%M"); }

inline std::string versionLine() {
    struct utsname u;
    if (uname(&u) != 0) return "Unix";
    return std::string(u.sysname) + " [Version " + u.release + "]";
}

inline std::string banner() { return versionLine() + "\r\n"; }

inline unsigned highestNumaNode() { return 0; }

inline std::vector<std::string> splitCommandLine(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    std::string a;
    bool q = false;
    while (i < s.size() && (q || (s[i] != ' ' && s[i] != '\t'))) {
        if (s[i] == '"') q = !q;
        else a += s[i];
        ++i;
    }
    out.push_back(a);
    for (;;) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        a.clear();
        q = false;
        for (;;) {
            size_t slashes = 0;
            while (i < s.size() && s[i] == '\\') { ++i; ++slashes; }
            bool copy = true;
            if (i < s.size() && s[i] == '"') {
                if (slashes % 2 == 0) {
                    if (q && i + 1 < s.size() && s[i + 1] == '"') ++i;
                    else { copy = false; q = !q; }
                }
                slashes /= 2;
            }
            a.append(slashes, '\\');
            if (i >= s.size() || (!q && (s[i] == ' ' || s[i] == '\t'))) break;
            if (copy) a += s[i];
            ++i;
        }
        out.push_back(a);
    }
    return out;
}

inline std::string commandLine() {
    std::vector<std::string> args = kernel::arguments();
    if (args.empty()) return "ccc";
    std::string out;
    for (const std::string& arg : args) {
        if (!out.empty()) out += ' ';
        if (arg.empty()) out += "\"\"";
        else if (arg.find_first_of(" \t") != std::string::npos && arg.find('"') == std::string::npos) out += "\"" + arg + "\"";
        else out += arg;
    }
    return out;
}

inline std::atomic<bool> interruptFromTerminal{ false };

inline posix_spawnattr_t* childSignals(posix_spawnattr_t* at, bool ownGroup) {
    posix_spawnattr_init(at);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(at, &none);
    posix_spawnattr_setsigdefault(at, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (ownGroup) { flags |= POSIX_SPAWN_SETPGROUP; posix_spawnattr_setpgroup(at, 0); }
    posix_spawnattr_setflags(at, flags);
    return at;
}

inline int forkOnTerminal(const std::string& exe, char* const* argv, char* const* envp, const std::string& tty, const winsize& size, pid_t& pid) {
    int report[2];
    if (!kernel::makePipe(report, false)) return errno;
    pid = fork();
    if (pid < 0) { int e = errno; ::close(report[0]); ::close(report[1]); return e; }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        setsid();
        int fd = ::open(tty.c_str(), O_RDWR);
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 0);
            ioctl(fd, TIOCSWINSZ, &size);
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) ::close(fd);
            execve(exe.c_str(), argv, envp);
        }
        int e = errno;
        ssize_t w = ::write(report[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    ::close(report[1]);
    int e = 0;
    ssize_t got;
    while ((got = ::read(report[0], &e, sizeof e)) < 0 && errno == EINTR) {}
    ::close(report[0]);
    if (got == (ssize_t)sizeof e) { waitpid(pid, nullptr, 0); return e; }
    return 0;
}

inline int spawnOnTerminal(const std::string& exe, char* const* argv, char* const* envp, const std::string& tty, const winsize& size, pid_t& pid) {
    if (!kernel::SPAWN_TAKES_TERMINAL) return forkOnTerminal(exe, argv, envp, tty, size, pid);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, tty.c_str(), O_RDWR, 0);
    posix_spawn_file_actions_adddup2(&fa, 0, 1);
    posix_spawn_file_actions_adddup2(&fa, 0, 2);
    posix_spawnattr_t at;
    childSignals(&at, false);
    short flags = 0;
    posix_spawnattr_getflags(&at, &flags);
    posix_spawnattr_setflags(&at, flags | POSIX_SPAWN_SETSID);
    int rc = posix_spawn(&pid, exe.c_str(), &fa, &at, argv, envp);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc;
}

inline std::vector<char*> pointers(std::vector<std::string>& v) {
    std::vector<char*> p;
    for (auto& s : v) p.push_back(s.data());
    p.push_back(nullptr);
    return p;
}

inline int startProcess(const std::string& exe, const std::string& cmdline, posix_spawn_file_actions_t* fa, bool ownGroup, const EnvList* env, pid_t& pid) {
    std::vector<std::string> args = splitCommandLine(cmdline), envCopy;
    if (args[0].empty()) args[0] = exe;
    auto argv = pointers(args);
    std::vector<char*> envp;
    if (env) { envCopy = *env; envp = pointers(envCopy); }
    posix_spawnattr_t at;
    int rc = posix_spawn(&pid, exe.c_str(), fa, childSignals(&at, ownGroup), argv.data(), env ? envp.data() : environ);
    posix_spawnattr_destroy(&at);
    return rc;
}

inline bool spawn(const Spawn& s, Process& p, int& error) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int src[3] = { s.in, s.out, s.err };
    std::vector<int> temps;
    for (int k = 0; k < 3; ++k)
        if (src[k] >= 0 && src[k] < 3 && src[k] != k) { src[k] = fcntl(src[k], F_DUPFD_CLOEXEC, 3); temps.push_back(src[k]); }
    for (int k = 0; k < 3; ++k)
        if (src[k] >= 0) posix_spawn_file_actions_adddup2(&fa, src[k], k);
    if (!s.dir.empty()) kernel::addChdir(&fa, s.dir);
    pid_t pid = -1;
    int rc = startProcess(s.exe, s.cmdline, &fa, false, s.env, pid);
    posix_spawn_file_actions_destroy(&fa);
    for (int t : temps) ::close(t);
    if (rc) { error = rc; errno = rc; return false; }
    p.pid = pid;
    p.waited = false;
    return true;
}

inline int exitCodeOf(const siginfo_t& si) { return si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status; }

inline int waitProcess(Process& p) {
    siginfo_t si{};
    while (waitid(P_PID, (id_t)p.pid, &si, WEXITED | WNOWAIT) < 0 && errno == EINTR) {}
    p.waited = true;
    return exitCodeOf(si);
}

inline void interruptProcess(const Process& p) { if (!interruptFromTerminal) kill(p.pid, SIGINT); }

inline bool waitProcessFor(Process& p, unsigned ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        siginfo_t si{};
        int rc = waitid(P_PID, (id_t)p.pid, &si, WEXITED | WNOHANG | WNOWAIT);
        if (rc == 0 && si.si_pid == p.pid) { p.waited = true; return true; }
        if (rc != 0 && errno == ECHILD && kill(p.pid, 0) != 0) return true;
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

inline void killProcess(const Process& p) { kill(p.pid, SIGKILL); }

inline unsigned long processId(const Process& p) { return (unsigned long)p.pid; }

inline bool readSharedMemory(const std::string& name, size_t offset, size_t size, std::string& out) {
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && offset <= (size_t)st.st_size;
    void* view = ok && st.st_size ? mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (view != MAP_FAILED) {
        size_t avail = (size_t)st.st_size - offset;
        out.assign(static_cast<const char*>(view) + offset, size ? (std::min)(size, avail) : avail);
        munmap(view, (size_t)st.st_size);
    }
    ::close(fd);
    shm_unlink(name.c_str());
    return ok && view != MAP_FAILED;
}

inline Process openProcess(unsigned long pid) { return { (pid_t)pid, true }; }

inline void reapLater(pid_t pid) {
    std::thread([pid] { while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }).detach();
}

inline void closeProcess(Process& p) {
    if (p.pid <= 0) return;
    if (p.waited) waitpid(p.pid, nullptr, 0);
    else reapLater(p.pid);
    p.pid = -1;
}

inline int niceValue(Priority p) {
    switch (p) {
    case Priority::Low: return 19;
    case Priority::BelowNormal: return 10;
    case Priority::AboveNormal: return -5;
    case Priority::High: return -10;
    case Priority::Realtime: return -20;
    default: return 0;
    }
}

inline bool launch(const Launch& l, int& exitCode) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!l.dir.empty()) kernel::addChdir(&fa, l.dir);
    pid_t pid = -1;
    int rc = startProcess(l.exe, l.cmdline, &fa, !l.wait, l.env, pid);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) { errno = rc; return false; }
    if (l.priority != Priority::Default) setpriority(PRIO_PROCESS, (id_t)pid, niceValue(l.priority));
    if (l.affinity) kernel::setAffinity(pid, l.affinity);
    exitCode = 0;
    if (!l.wait) { reapLater(pid); return true; }
    Process p{ pid, false };
    exitCode = waitProcess(p);
    closeProcess(p);
    return true;
}

inline bool openDocument(const std::string& file, const std::string&, const std::string& dir, Show, bool wait) {
    Launch l;
    l.exe = kernel::OPENER;
    l.cmdline = std::string(kernel::OPENER) + " \"" + file + "\"";
    l.dir = dir;
    l.wait = wait;
    int code = 0;
    return launch(l, code);
}

inline int wakePipe(int end) {
    static int fds[2] = { -1, -1 };
    static bool made = kernel::makePipe(fds, true);
    (void)made;
    return fds[end];
}

inline std::atomic<bool> waitingForInput{ false };

inline int waitReadable(int fd, unsigned long timeoutMs) {
    char drain[64];
    while (::read(wakePipe(0), drain, sizeof drain) > 0) {}
    waitingForInput = true;
    pollfd fds[2] = { { fd, POLLIN, 0 }, { wakePipe(0), POLLIN, 0 } };
    int r;
    while ((r = poll(fds, 2, timeoutMs == WAIT_FOREVER ? -1 : (int)timeoutMs)) < 0 && errno == EINTR) {}
    waitingForInput = false;
    if (fds[1].revents) return KEY_CANCELLED;
    return r > 0 ? 1 : KEY_TIMEOUT;
}

inline void (*interruptHandler)() = nullptr;

inline void handleSignals(void (*onInterrupt)()) {
    interruptHandler = onInterrupt;
    signal(SIGPIPE, SIG_IGN);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    std::thread([set] {
        for (;;) {
            bool fromTerminal = false;
            if (!kernel::waitInterrupt(set, fromTerminal)) continue;
            interruptFromTerminal = fromTerminal;
            if (interruptHandler) interruptHandler();
        }
    }).detach();
}

inline ReadResult readStdin(char* buf, size_t capacity, size_t& got) {
    got = 0;
    if (waitReadable(0, WAIT_FOREVER) == KEY_CANCELLED) return ReadResult::Cancelled;
    got = read(0, buf, capacity);
    return got ? ReadResult::Data : ReadResult::End;
}

inline void cancelStdinRead() { if (waitingForInput) { char c = 1; ssize_t w = ::write(wakePipe(1), &c, 1); (void)w; } }

inline int terminal() {
    static int fd = [] { int t = ::open("/dev/tty", O_RDWR | O_CLOEXEC); return t >= 0 ? t : 0; }();
    return fd;
}

inline int readKey(unsigned long timeoutMs) {
    int fd = terminal();
    termios old;
    bool raw = tcgetattr(fd, &old) == 0;
    if (raw) {
        termios t = old;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_iflag &= ~ICRNL;
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    int key = waitReadable(fd, timeoutMs);
    unsigned char c;
    if (key == 1) key = ::read(fd, &c, 1) == 1 ? c : KEY_TIMEOUT;
    if (raw) tcsetattr(fd, TCSANOW, &old);
    return key;
}

inline void beep() { write(terminal(), "\a", 1); }

inline bool consoleSize(int& cols, int& rows) {
    winsize w{};
    if (ioctl(1, TIOCGWINSZ, &w) != 0 && ioctl(terminal(), TIOCGWINSZ, &w) != 0) return false;
    if (!w.ws_col || !w.ws_row) return false;
    cols = w.ws_col;
    rows = w.ws_row;
    return true;
}

inline bool writeTerminal(const std::string& s) { return isatty(1) && write(1, s.data(), s.size()); }

inline bool resizeConsole(int cols, int rows) { return writeTerminal("\x1b[8;" + std::to_string(rows) + ";" + std::to_string(cols) + "t"); }

inline void setConsoleTitle(const std::string& title) { writeTerminal("\x1b]0;" + title + "\x07"); }

inline std::string consoleTitle() { return "ccc"; }

inline void setConsoleColor(int attr) {
    static const uint32_t rgb[16] = { 0x0C0C0C, 0x0037DA, 0x13A10E, 0x3A96DD, 0xC50F1F, 0x881798, 0xC19C00, 0xCCCCCC,
                                      0x767676, 0x3B78FF, 0x16C60C, 0x61D6D6, 0xE74856, 0xB4009E, 0xF9F1A5, 0xF2F2F2 };
    if (attr == 0x07) { writeTerminal("\x1b]110\x07\x1b]111\x07"); return; }
    char buf[64];
    std::snprintf(buf, sizeof buf, "\x1b]10;#%06X\x07\x1b]11;#%06X\x07", rgb[attr & 15], rgb[(attr >> 4) & 15]);
    writeTerminal(buf);
}

inline void clearConsole() { writeTerminal("\x1b[H\x1b[2J\x1b[3J"); }

}
