#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <winioctl.h>
#include <aclapi.h>
#include <conio.h>
#include <intrin.h>
#include <algorithm>
#include <execution>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace os {

constexpr unsigned UTF8_PAGE = CP_UTF8;
constexpr char SLASH = '\\';
constexpr const char* SLASHES = "\\/";
constexpr const char* PATH_STOPS = "\\/:";
constexpr char PATH_LIST_SEPARATOR = ';';
constexpr const char* DEFAULT_PATHEXT = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";
constexpr bool EXTENSIONLESS_PROGRAMS = false;
constexpr bool INDEX_PATH_DIRS = true;
constexpr bool CONSOLE_TOOLS_EXTERNAL = true;
constexpr const char* NEW_CONSOLE_SWITCH = "--run";
constexpr int INTERRUPTED_EXIT_CODE = -1073741510;
constexpr int ERR_ALREADY_EXISTS = ERROR_ALREADY_EXISTS, ERR_FILE_NOT_FOUND = ERROR_FILE_NOT_FOUND, ERR_ACCESS_DENIED = ERROR_ACCESS_DENIED;

using Handle = HANDLE;
constexpr Handle NO_HANDLE = nullptr;

inline std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

template <class It, class F>
void forEachParallel(It first, It last, F f) { std::for_each(std::execution::par, first, last, f); }

inline unsigned defaultCodePage() { return GetOEMCP(); }

inline bool stdRedirected() {
    for (DWORD which : { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE }) {
        DWORD t = GetFileType(GetStdHandle(which));
        if (t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE) return true;
    }
    return false;
}

inline void ignoreConsoleInterrupts() { SetConsoleCtrlHandler([](DWORD t) -> BOOL { return t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT; }, TRUE); }

inline uint32_t decodeByte(unsigned page, unsigned char b) {
    char in = (char)b;
    wchar_t w = 0;
    return MultiByteToWideChar(page, 0, &in, 1, &w, 1) ? (uint32_t)w : '?';
}

inline bool codePageValid(unsigned page) {
    CPINFO info;
    return page == CP_UTF8 || GetCPInfo(page, &info);
}

inline std::u16string utf16(const char* p, size_t n) {
    int count = MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
    std::u16string out(count, u'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, (int)n, reinterpret_cast<wchar_t*>(out.data()), count);
    return out;
}

inline std::string fromUtf16(const char16_t* p, size_t n) { return narrow(std::wstring(reinterpret_cast<const wchar_t*>(p), n)); }

inline int lastError() { return (int)GetLastError(); }

inline std::string errorText(int code) {
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, (DWORD)code, 0,
                             reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    if (!n) return "Error " + std::to_string((DWORD)code) + ".";
    std::string s = narrow(std::wstring(text, n));
    LocalFree(text);
    return s.erase(s.find_last_not_of("\r\n") + 1);
}

inline std::string cmdMessage(unsigned id) {
    static HMODULE cmd = [] {
        wchar_t dir[MAX_PATH];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        return LoadLibraryExW((std::wstring(dir, n) + L"\\cmd.exe").c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    }();
    wchar_t* text = nullptr;
    DWORD n = cmd ? FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER, cmd, id, 0,
                                   reinterpret_cast<wchar_t*>(&text), 0, nullptr) : 0;
    if (!n) return {};
    std::string s = narrow(std::wstring(text, n));
    LocalFree(text);
    return s;
}

inline unsigned consoleOutputPage() { return GetConsoleOutputCP(); }

inline std::string encodeText(const std::string& utf8, unsigned page) {
    if (page == CP_UTF8 || std::all_of(utf8.begin(), utf8.end(), [](char c) { return (unsigned char)c < 0x80; })) return utf8;
    std::wstring w = wide(utf8);
    int n = WideCharToMultiByte(page, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(page, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string digitGroupSeparator() {
    wchar_t sep[8];
    int n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8);
    return n > 1 ? narrow(std::wstring(sep, n - 1)) : ",";
}

inline Handle stdHandle(int fd) { return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE); }

inline bool isConsole(Handle h) { return GetFileType(h) == FILE_TYPE_CHAR; }

inline Handle opened(HANDLE h) { return h == INVALID_HANDLE_VALUE ? NO_HANDLE : h; }

inline Handle openRead(const std::string& path) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    return opened(CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
}

inline Handle openWrite(const std::string& path, bool append) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    return opened(CreateFileW(wide(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              append ? OPEN_ALWAYS : CREATE_ALWAYS, 0, nullptr));
}

inline Handle openNull() { return opened(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)); }

inline size_t read(Handle h, char* p, size_t n) {
    DWORD got = 0;
    return ReadFile(h, p, (DWORD)n, &got, nullptr) ? got : 0;
}

inline bool write(Handle h, const char* p, size_t n) {
    DWORD w;
    return WriteFile(h, p, (DWORD)n, &w, nullptr) != 0;
}

inline void close(Handle h) { if (h) CloseHandle(h); }

inline bool createPipe(Handle& read, Handle& write, uint32_t bytes) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (CreatePipe(&read, &write, &sa, bytes)) return true;
    read = write = NO_HANDLE;
    return false;
}

inline bool createReadPipe(Handle& read, Handle& write) {
    static std::atomic<unsigned> serial{ 0 };
    wchar_t name[64];
    swprintf(name, 64, L"\\\\.\\pipe\\ccc.%lu.%u", GetCurrentProcessId(), serial++);
    read = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, PIPE_BYTES, PIPE_BYTES, 0, nullptr);
    if (read == INVALID_HANDLE_VALUE) { read = write = NO_HANDLE; return false; }
    write = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (write == INVALID_HANDLE_VALUE) { CloseHandle(read); read = write = NO_HANDLE; return false; }
    return true;
}

struct Process {
    HANDLE handle = nullptr;
    bool operator==(const Process& o) const { return handle == o.handle; }
};

struct Spawn {
    std::string exe, cmdline, dir;
    Handle in = NO_HANDLE, out = NO_HANDLE, err = NO_HANDLE;
    const EnvList* env = nullptr;
    bool hidden = false;
};

class PipeReader {
public:
    PipeReader(Handle pipe, const Process* writer) : from(pipe), writerExit(writer ? writer->handle : nullptr) { ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~PipeReader() { if (pending) { CancelIoEx(from, &ov); DWORD n; GetOverlappedResult(from, &ov, &n, TRUE); } CloseHandle(ov.hEvent); }
    PipeReader(const PipeReader&) = delete;
    PipeReader& operator=(const PipeReader&) = delete;
    bool start(char* p, size_t capacity) {
        pending = ReadFile(from, p, (DWORD)capacity, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
        return pending;
    }
    size_t finish() {
        if (!pending) return 0;
        pending = false;
        if (WaitForSingleObject(ov.hEvent, 0) != WAIT_OBJECT_0) {
            if (writerGone) CancelIoEx(from, &ov);
            else if (writerExit) {
                HANDLE hs[2] = { ov.hEvent, writerExit };
                if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) { writerGone = true; CancelIoEx(from, &ov); }
            }
        }
        DWORD n = 0;
        GetOverlappedResult(from, &ov, &n, TRUE);
        return n;
    }
    size_t read(char* p, size_t capacity) { return start(p, capacity) ? finish() : 0; }
private:
    HANDLE from, writerExit;
    OVERLAPPED ov = {};
    bool pending = false, writerGone = false;
};

inline FileTime toFileTime(FILETIME ft) {
    long long ticks = (long long)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL;
    return FileTime(std::chrono::duration_cast<FileTime::duration>(std::chrono::duration<long long, std::ratio<1, 10000000>>(ticks)));
}

inline FILETIME toFiletime(FileTime t) {
    long long ticks = std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10000000>>>(t.time_since_epoch()).count() + 116444736000000000LL;
    return FILETIME{ (DWORD)(uint64_t)ticks, (DWORD)((uint64_t)ticks >> 32) };
}

inline FileInfo toFileInfo(const WIN32_FIND_DATAW& fd) {
    FileInfo f;
    f.name = narrow(fd.cFileName);
    f.shortName = narrow(fd.cAlternateFileName);
    f.attr = fd.dwFileAttributes;
    f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    f.written = toFileTime(fd.ftLastWriteTime);
    f.created = toFileTime(fd.ftCreationTime);
    f.accessed = toFileTime(fd.ftLastAccessTime);
    f.junction = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT;
    return f;
}

inline std::vector<FileInfo> listFiles(const std::string& pattern, bool shortNames = false) {
    std::vector<FileInfo> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(wide(pattern).c_str(), shortNames ? FindExInfoStandard : FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(toFileInfo(fd));
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

inline bool findFile(const std::string& pattern, FileInfo& found) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(wide(pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    found = toFileInfo(fd);
    return true;
}

inline uint32_t attributes(const std::string& path) { return GetFileAttributesW(wide(path).c_str()); }

inline bool fileInfo(const std::string& path, FileInfo& info) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &fa)) return false;
    info.name = path.substr(path.find_last_of("\\/") + 1);
    info.attr = fa.dwFileAttributes;
    info.size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    info.written = toFileTime(fa.ftLastWriteTime);
    info.created = toFileTime(fa.ftCreationTime);
    info.accessed = toFileTime(fa.ftLastAccessTime);
    return true;
}

inline bool mapFile(const std::string& path, Mapped& m) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    m = Mapped();
    if (sz.QuadPart == 0) { CloseHandle(h); return true; }
    HANDLE map = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(h);
    if (!map) return false;
    const char* view = (const char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(map);
    if (!view) return false;
    m.keep = std::shared_ptr<void>((void*)view, [](void* v) { UnmapViewOfFile(v); });
    m.data = view;
    m.size = (size_t)sz.QuadPart;
    return true;
}

inline bool makeWritable(const std::string& path) {
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(wide(path).c_str(), a & ~FILE_ATTRIBUTE_READONLY);
}
inline bool removeDirectory(const std::string& path) { return RemoveDirectoryW(wide(path).c_str()) != 0; }
inline bool removeFile(const std::string& path) { return DeleteFileW(wide(path).c_str()) != 0; }
inline bool renameFile(const std::string& from, const std::string& to) { return MoveFileW(wide(from).c_str(), wide(to).c_str()) != 0; }
inline bool moveFile(const std::string& from, const std::string& to) {
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != 0;
}
inline bool copyFile(const std::string& from, const std::string& to, uint32_t flags) {
    return CopyFileExW(wide(from).c_str(), wide(to).c_str(), nullptr, nullptr, nullptr, flags) != 0;
}

inline std::string fullPath(const std::string& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(wide(p.empty() ? "." : p).c_str(), (DWORD)std::size(buf), buf, nullptr);
    return n ? narrow(std::wstring(buf, n)) : p;
}

inline bool junction(const std::string& link, const std::string& target) {
    std::wstring t = wide(fullPath(target)), sub = L"\\??\\" + t;
    if (!CreateDirectoryW(wide(link).c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(wide(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(8 + 8 + (sub.size() + t.size() + 2) * 2);
    auto* p = reinterpret_cast<uint8_t*>(buf.data());
    USHORT subLen = (USHORT)(sub.size() * 2), prnLen = (USHORT)(t.size() * 2);
    *(DWORD*)p = IO_REPARSE_TAG_MOUNT_POINT;
    *(USHORT*)(p + 4) = (USHORT)(buf.size() - 8);
    *(USHORT*)(p + 8) = 0;
    *(USHORT*)(p + 10) = subLen;
    *(USHORT*)(p + 12) = (USHORT)(subLen + 2);
    *(USHORT*)(p + 14) = prnLen;
    memcpy(p + 16, sub.c_str(), subLen + 2);
    memcpy(p + 16 + subLen + 2, t.c_str(), prnLen + 2);
    DWORD n;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &n, nullptr);
    CloseHandle(h);
    return ok != 0;
}

inline std::string currentDirectory() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW((DWORD)std::size(buf), buf);
    return narrow(std::wstring(buf, n));
}
inline bool setCurrentDirectory(const std::string& path) { return SetCurrentDirectoryW(wide(path).c_str()) != 0; }

inline std::string shortPath(const std::string& path) {
    wchar_t buf[MAX_PATH * 4];
    DWORD k = GetShortPathNameW(wide(path).c_str(), buf, (DWORD)std::size(buf));
    return k && k < std::size(buf) ? narrow(std::wstring(buf, k)) : std::string();
}

inline std::string driveOf(const std::string& path) { return path.substr(0, 2); }

inline std::string selfPath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return narrow(std::wstring(buf, n));
}

inline std::string defaultComspec() { return "C:\\Windows\\System32\\cmd.exe"; }

inline std::string foldName(const std::string& name) {
    std::wstring s = wide(name), u(s.size(), L'\0');
    if (!s.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), (int)s.size(), &u[0], (int)u.size(), nullptr, nullptr, 0);
    return narrow(u);
}

inline bool isExecutable(const std::string&) { return true; }

inline bool isGuiProgram(const std::string& exe) { return HIWORD(SHGetFileInfoW(wide(exe).c_str(), 0, nullptr, 0, SHGFI_EXETYPE)) != 0; }

inline bool opensAsDocument(const std::string& upperExt) { return upperExt != ".EXE" && upperExt != ".COM"; }

class DirWatch {
public:
    explicit DirWatch(const std::string& dir) {
        HANDLE w = FindFirstChangeNotificationW(wide(dir).c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        h = w == INVALID_HANDLE_VALUE ? nullptr : w;
    }
    ~DirWatch() { if (h) FindCloseChangeNotification(h); }
    DirWatch(const DirWatch&) = delete;
    DirWatch& operator=(const DirWatch&) = delete;
    bool active() const { return h != nullptr; }
    bool changed() const { return WaitForSingleObject(h, 0) == WAIT_OBJECT_0; }
private:
    HANDLE h = nullptr;
};

inline bool volumeInfo(const std::string& path, Volume& v) {
    wchar_t label[MAX_PATH] = {};
    DWORD serial = 0;
    BOOL ok = GetVolumeInformationW(wide(path.substr(0, 3)).c_str(), label, MAX_PATH, &serial, nullptr, nullptr, nullptr, 0);
    v.drive = path.substr(0, 1);
    v.label = narrow(label);
    v.serial = serial;
    return ok != 0;
}

inline std::string fileOwner(const std::string& path) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(wide(path).c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) return "...";
    wchar_t name[256], domain[256];
    DWORD n = 256, d = 256;
    SID_NAME_USE use;
    std::string r = LookupAccountSidW(nullptr, sid, name, &n, domain, &d, &use) ? narrow(domain) + "\\" + narrow(name) : "...";
    LocalFree(sd);
    return r;
}

inline std::vector<std::pair<std::string, uint64_t>> alternateStreams(const std::string& path) {
    std::vector<std::pair<std::string, uint64_t>> out;
    WIN32_FIND_STREAM_DATA sd;
    HANDLE h = FindFirstStreamW(wide(path).c_str(), FindStreamInfoStandard, &sd, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do if (wcscmp(sd.cStreamName, L"::$DATA") != 0) out.push_back({ narrow(sd.cStreamName), (uint64_t)sd.StreamSize.QuadPart });
    while (FindNextStreamW(h, &sd));
    FindClose(h);
    return out;
}

inline bool connectNetworkDrive(const std::string& remote, char& letter) {
    for (char d = 'Z'; d > 'C'; --d) {
        std::wstring local = wide(std::string(1, d) + ":"), target = wide(remote);
        NETRESOURCEW nr = {};
        nr.dwType = RESOURCETYPE_DISK;
        nr.lpLocalName = &local[0];
        nr.lpRemoteName = &target[0];
        if (WNetAddConnection2W(&nr, nullptr, nullptr, 0) == NO_ERROR) { letter = d; return true; }
    }
    return false;
}

inline void disconnectNetworkDrive(char letter) { WNetCancelConnection2W(wide(std::string(1, letter) + ":").c_str(), 0, TRUE); }

inline EnvList environment() {
    EnvList out;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) out.push_back(narrow(p));
    FreeEnvironmentStringsW(block);
    return out;
}

inline void setEnvironment(const std::string& name, const std::string* value) {
    SetEnvironmentVariableW(wide(name).c_str(), value ? wide(*value).c_str() : nullptr);
}

inline HKEY settingRoot(int scope) { return scope == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

inline bool shellSetting(int scope, const char* name, uint32_t& value) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(settingRoot(scope), L"Software\\Microsoft\\Command Processor", wide(name).c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    value = v;
    return true;
}

inline std::string shellSettingText(int scope, const char* name) {
    wchar_t buf[4096];
    DWORD size = sizeof(buf);
    if (RegGetValueW(settingRoot(scope), L"Software\\Microsoft\\Command Processor", wide(name).c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return "";
    return narrow(buf);
}

inline std::string classValue(const std::string& key) {
    wchar_t buf[2048];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CLASSES_ROOT, wide(key).c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &sz) != ERROR_SUCCESS) return "";
    return narrow(buf);
}

inline bool setClassValue(const std::string& key, const std::string* value) {
    std::wstring k = wide("Software\\Classes\\" + key);
    if (!value) return RegDeleteTreeW(HKEY_LOCAL_MACHINE, k.c_str()) == ERROR_SUCCESS;
    HKEY h;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, k.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    std::wstring v = wide(*value);
    bool ok = RegSetValueExW(h, nullptr, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

inline std::vector<std::string> classKeys() {
    std::vector<std::string> out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD n = 512;
        if (RegEnumKeyExW(HKEY_CLASSES_ROOT, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(narrow(name));
    }
    return out;
}

inline Clock toClock(const SYSTEMTIME& t) { return { t.wYear, t.wMonth, t.wDay, t.wDayOfWeek, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds }; }

inline SYSTEMTIME toSystemTime(const Clock& c) {
    SYSTEMTIME t = {};
    t.wYear = (WORD)c.year; t.wMonth = (WORD)c.month; t.wDay = (WORD)c.day; t.wDayOfWeek = (WORD)c.weekday;
    t.wHour = (WORD)c.hour; t.wMinute = (WORD)c.minute; t.wSecond = (WORD)c.second; t.wMilliseconds = (WORD)c.millis;
    return t;
}

inline Clock localNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return toClock(t);
}

inline Clock localTime(FileTime when) {
    FILETIME ft = toFiletime(when), lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    return toClock(st);
}

inline bool setLocalNow(const Clock& c) {
    SYSTEMTIME t = toSystemTime(c);
    return SetLocalTime(&t) != 0;
}

inline std::string shortDate(const Clock& c) {
    SYSTEMTIME t = toSystemTime(c);
    wchar_t buf[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &t, nullptr, buf, 64, nullptr);
    return narrow(buf);
}

inline std::string shortTime(const Clock& c) {
    SYSTEMTIME t = toSystemTime(c);
    wchar_t buf[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, buf, 64);
    return narrow(buf);
}

inline std::string versionLine() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = { sizeof(v) };
    auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (fn) fn(&v);
    DWORD ubr = 0, sz = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &sz);
    return "Microsoft Windows [Version " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
           std::to_string(v.dwBuildNumber) + "." + std::to_string(ubr) + "]";
}

inline std::string banner() { return versionLine() + "\r\n(c) Microsoft Corporation. All rights reserved.\r\n"; }

inline unsigned highestNumaNode() { ULONG h = 0; GetNumaHighestNodeNumber(&h); return h; }

inline std::string commandLine() { return narrow(GetCommandLineW()); }

inline HANDLE inheritable(HANDLE h) {
    HANDLE d = nullptr;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return d;
}

inline std::vector<wchar_t> environmentBlock(const EnvList& env) {
    std::vector<wchar_t> block;
    for (auto& e : env) {
        std::wstring entry = wide(e);
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

inline bool spawn(const Spawn& s, Process& p, int& error) {
    HANDLE in = inheritable(s.in), out = inheritable(s.out), err = s.err == s.out ? out : inheritable(s.err);
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    HANDLE list[3];
    int count = 0;
    for (HANDLE h : { in, out, err })
        if (h && std::find(list, list + count, h) == list + count) list[count++] = h;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, count * sizeof(HANDLE), nullptr, nullptr);
    PROCESS_INFORMATION pi = {};
    std::wstring wcmd = wide(s.cmdline), wexe = wide(s.exe), wdir = wide(s.dir);
    std::vector<wchar_t> block;
    if (s.env) block = environmentBlock(*s.env);
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | (s.hidden ? CREATE_NO_WINDOW : 0);
    BOOL ok = CreateProcessW(wexe.c_str(), &wcmd[0], nullptr, nullptr, TRUE, flags, block.empty() ? nullptr : block.data(),
                             s.dir.empty() ? nullptr : wdir.c_str(), &si.StartupInfo, &pi);
    error = (int)GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    for (int k = 0; k < count; ++k) CloseHandle(list[k]);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    p.handle = pi.hProcess;
    return true;
}

inline int waitProcess(Process& p) {
    WaitForSingleObject(p.handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p.handle, &code);
    return (int)code;
}

inline void interruptProcess(const Process& p) { TerminateProcess(p.handle, 0xC000013A); }

inline bool waitProcessFor(Process& p, unsigned ms) { return WaitForSingleObject(p.handle, ms) == WAIT_OBJECT_0; }

inline void killProcess(const Process& p) { TerminateProcess(p.handle, 1); }

inline unsigned long processId(const Process& p) { return GetProcessId(p.handle); }

inline bool readSharedMemory(const std::string& name, size_t offset, size_t size, std::string& out) {
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, wide(name).c_str());
    if (!map) return false;
    const char* view = static_cast<const char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    MEMORY_BASIC_INFORMATION info = {};
    bool ok = view && VirtualQuery(view, &info, sizeof info) && offset <= info.RegionSize;
    if (ok) out.assign(view + offset, size ? (std::min)(size, info.RegionSize - offset) : info.RegionSize - offset);
    if (view) UnmapViewOfFile(view);
    CloseHandle(map);
    return ok;
}

inline Process openProcess(unsigned long pid) { return { OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid) }; }

inline void closeProcess(Process& p) {
    if (p.handle) CloseHandle(p.handle);
    p.handle = nullptr;
}

inline int showCommand(Show s) { return s == Show::Minimized ? SW_SHOWMINNOACTIVE : s == Show::Maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL; }

inline DWORD priorityClass(Priority p) {
    switch (p) {
    case Priority::Low: return IDLE_PRIORITY_CLASS;
    case Priority::BelowNormal: return BELOW_NORMAL_PRIORITY_CLASS;
    case Priority::Normal: return NORMAL_PRIORITY_CLASS;
    case Priority::AboveNormal: return ABOVE_NORMAL_PRIORITY_CLASS;
    case Priority::High: return HIGH_PRIORITY_CLASS;
    case Priority::Realtime: return REALTIME_PRIORITY_CLASS;
    default: return 0;
    }
}

inline bool launch(const Launch& l, int& exitCode) {
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = (WORD)showCommand(l.show);
    std::wstring wt = wide(l.title);
    if (!l.title.empty()) si.StartupInfo.lpTitle = &wt[0];
    std::vector<char> attrBuf;
    USHORT nodeValue = (USHORT)(l.node < 0 ? 0 : l.node);
    DWORD flags = priorityClass(l.priority) | (l.detached ? DETACHED_PROCESS : l.newConsole ? CREATE_NEW_CONSOLE : 0) | (l.affinity ? CREATE_SUSPENDED : 0) |
                  CREATE_UNICODE_ENVIRONMENT;
    if (l.node >= 0) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrBuf.resize(size);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PREFERRED_NODE, &nodeValue, sizeof(nodeValue), nullptr, nullptr);
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    PROCESS_INFORMATION pi = {};
    std::wstring cmdline = wide(l.cmdline), wd = wide(l.dir);
    std::vector<wchar_t> block;
    if (l.env) block = environmentBlock(*l.env);
    BOOL ok = CreateProcessW(wide(l.exe).c_str(), &cmdline[0], nullptr, nullptr, FALSE, flags, block.empty() ? nullptr : block.data(),
                             l.dir.empty() ? nullptr : wd.c_str(), &si.StartupInfo, &pi);
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) return false;
    if (l.affinity) { SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)l.affinity); ResumeThread(pi.hThread); }
    CloseHandle(pi.hThread);
    DWORD code = 0;
    if (l.wait) { WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &code); }
    CloseHandle(pi.hProcess);
    exitCode = (int)code;
    return true;
}

inline bool openDocument(const std::string& file, const std::string& params, const std::string& dir, Show show, bool wait) {
    SHELLEXECUTEINFOW si = { sizeof(si) };
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    std::wstring wf = wide(file), wp = wide(params), wd = wide(dir);
    si.lpFile = wf.c_str();
    si.lpParameters = wp.c_str();
    si.lpDirectory = dir.empty() ? nullptr : wd.c_str();
    si.nShow = showCommand(show);
    if (!ShellExecuteExW(&si)) return false;
    if (si.hProcess) { if (wait) WaitForSingleObject(si.hProcess, INFINITE); CloseHandle(si.hProcess); }
    return true;
}

inline void (*interruptHandler)() = nullptr;

inline void handleSignals(void (*onInterrupt)()) {
    interruptHandler = onInterrupt;
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
        if (interruptHandler) interruptHandler();
        return TRUE;
    }, TRUE);
}

inline ReadResult readStdin(char* buf, size_t capacity, size_t& got) {
    DWORD n = 0;
    if (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, (DWORD)capacity, &n, nullptr) && n) { got = n; return ReadResult::Data; }
    got = 0;
    if (GetLastError() != ERROR_OPERATION_ABORTED) return ReadResult::End;
    SetLastError(0);
    return ReadResult::Cancelled;
}

inline void cancelStdinRead() {}

inline int readKey(unsigned long timeoutMs) {
    if (timeoutMs == WAIT_FOREVER) return _getch();
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        if (_kbhit()) return _getch();
        ULONGLONG now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(in, (DWORD)(deadline - now)) != WAIT_OBJECT_0) return KEY_TIMEOUT;
        INPUT_RECORD r;
        DWORD n;
        if (!_kbhit() && PeekConsoleInputW(in, &r, 1, &n) && n && !(r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown)) ReadConsoleInputW(in, &r, 1, &n);
    }
}

inline void beep() { MessageBeep(MB_OK); }

inline bool consoleSize(int& cols, int& rows) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return false;
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return true;
}

inline bool resizeConsole(int, int) { return false; }

inline void setConsoleTitle(const std::string& title) { SetConsoleTitleW(wide(title).c_str()); }

inline std::string consoleTitle() {
    wchar_t buf[1024];
    DWORD n = GetConsoleTitleW(buf, (DWORD)std::size(buf));
    return narrow(std::wstring(buf, n));
}

inline void setConsoleColor(int attr) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(out, &info)) return;
    DWORD n;
    FillConsoleOutputAttribute(out, (WORD)attr, info.dwSize.X * info.dwSize.Y, COORD{ 0, 0 }, &n);
    SetConsoleTextAttribute(out, (WORD)attr);
}

inline void clearConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(out, &info)) return;
    DWORD n, cells = info.dwSize.X * info.dwSize.Y;
    FillConsoleOutputCharacterW(out, L' ', cells, COORD{ 0, 0 }, &n);
    FillConsoleOutputAttribute(out, info.wAttributes, cells, COORD{ 0, 0 }, &n);
    SetConsoleCursorPosition(out, COORD{ 0, 0 });
}

}

namespace ccc {
inline std::wstring W(const std::string& s) { return os::wide(s); }
inline std::string U(const std::wstring& w) { return os::narrow(w); }
}
