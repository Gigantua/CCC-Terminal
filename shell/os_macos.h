#pragma once
#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

namespace os::kernel {

constexpr bool SPAWN_TAKES_TERMINAL = false;
constexpr const char* OPENER = "/usr/bin/open";

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
inline void addChdir(posix_spawn_file_actions_t* fa, const std::string& dir) { posix_spawn_file_actions_addchdir_np(fa, dir.c_str()); }
#pragma clang diagnostic pop

inline bool makePipe(int (&fds)[2], bool nonblocking) {
    if (pipe(fds) != 0) return false;
    for (int fd : fds) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (nonblocking) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    return true;
}

inline const timespec& modified(const struct stat& s) { return s.st_mtimespec; }
inline const timespec& created(const struct stat& s) { return s.st_birthtimespec; }
inline const timespec& accessed(const struct stat& s) { return s.st_atimespec; }

inline std::string executablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return std::string();
    path.resize(path.find('\0'));
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

inline std::vector<std::string> arguments() {
    char** argv = *_NSGetArgv();
    return std::vector<std::string>(argv, argv + *_NSGetArgc());
}

inline void setAffinity(pid_t, uint64_t) {}

inline bool waitInterrupt(const sigset_t& set, bool& fromTerminal) {
    int signal = 0;
    if (sigwait(&set, &signal) != 0) return false;
    int tty = isatty(0) ? 0 : -1;
    fromTerminal = tty >= 0 && tcgetpgrp(tty) == getpgrp();
    return true;
}

}
