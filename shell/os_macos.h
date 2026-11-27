#pragma once
#include <crt_externs.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
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

// An app started from Finder or the Dock gets launchd's bare environment, without what
// ~/.zprofile and ~/.zshrc add (Homebrew, ~/.local/bin, ...). Ask the user's login shell
// for its environment once and adopt it, so tabs find the same commands Terminal does.
inline void importLoginEnvironment() {
    const char* path = getenv("PATH");
    if (path && strcmp(path, "/usr/bin:/bin:/usr/sbin:/sbin") != 0) return;
    std::string shell = getenv("SHELL") ? getenv("SHELL") : "";
    if (shell.empty() || access(shell.c_str(), X_OK) != 0) {
        passwd* pw = getpwuid(getuid());
        shell = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/zsh";
    }
    int fds[2];
    if (!makePipe(fds, false)) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
    static const char MARKER[] = "\x1f" "ccc-env" "\x1f";
    std::string command = "printf '\037ccc-env\037'; exec /usr/bin/env -0";
    std::string name = shell.substr(shell.rfind('/') + 1);
    const char* argv[] = { name.c_str(), "-l", "-i", "-c", command.c_str(), nullptr };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, shell.c_str(), &fa, &at, (char* const*)argv, *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return; }
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        pollfd p{ fds[0], POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, left) <= 0) { kill(pid, SIGKILL); out.clear(); break; }
        char buf[16384];
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    size_t at0 = out.rfind(MARKER);
    if (at0 == std::string::npos) return;
    static const char* const SKIP[] = { "PWD", "OLDPWD", "SHLVL", "_", "TERM", "TERM_PROGRAM", "TERM_PROGRAM_VERSION", "COLUMNS", "LINES" };
    for (size_t i = at0 + sizeof MARKER - 1; i < out.size();) {
        size_t end = out.find('\0', i);
        if (end == std::string::npos) end = out.size();
        std::string entry = out.substr(i, end - i);
        i = end + 1;
        size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = entry.substr(0, eq);
        bool skip = false;
        for (const char* s : SKIP) skip = skip || key == s;
        if (!skip) setenv(key.c_str(), entry.c_str() + eq + 1, 1);
    }
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
