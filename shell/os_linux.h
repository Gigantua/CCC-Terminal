#pragma once
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace os::kernel {

constexpr bool SPAWN_TAKES_TERMINAL = true;
constexpr const char* OPENER = "/usr/bin/xdg-open";

inline void addChdir(posix_spawn_file_actions_t* fa, const std::string& dir) { posix_spawn_file_actions_addchdir_np(fa, dir.c_str()); }

inline bool makePipe(int (&fds)[2], bool nonblocking) { return pipe2(fds, O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) == 0; }

inline const timespec& modified(const struct stat& s) { return s.st_mtim; }
inline const timespec& created(const struct stat& s) { return s.st_ctim; }
inline const timespec& accessed(const struct stat& s) { return s.st_atim; }

inline std::string executablePath() {
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).string();
}

inline std::vector<std::string> arguments() {
    std::ifstream file("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> out;
    if (!file) return out;
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    for (size_t a = 0; a < raw.size();) {
        size_t b = raw.find('\0', a);
        if (b == std::string::npos) b = raw.size();
        out.push_back(raw.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

inline void setAffinity(pid_t pid, uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int k = 0; k < 64; ++k) if (mask >> k & 1) CPU_SET(k, &set);
    sched_setaffinity(pid, sizeof set, &set);
}

inline bool waitInterrupt(const sigset_t& set, bool& fromTerminal) {
    siginfo_t si;
    if (sigwaitinfo(&set, &si) < 0) return false;
    fromTerminal = si.si_code == SI_KERNEL;
    return true;
}

}
