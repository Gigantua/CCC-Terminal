#pragma once
#include <string>
#include "../shell/palette.h"
#include "../shell/util.h"

inline std::string percentDecoded(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < s.size() && (hi = palette::hexDigit(s[i + 1])) >= 0 && (lo = palette::hexDigit(s[i + 2])) >= 0) { out += (char)(hi << 4 | lo); i += 2; }
        else out += s[i];
    }
    return out;
}

inline std::string urlPath(const std::string& reported) {
    if (reported.rfind("file://", 0) != 0) return reported;
    size_t slash = reported.find('/', 7);
    return slash == std::string::npos ? std::string() : percentDecoded(reported.substr(slash));
}

#ifdef _WIN32
inline std::string wslRoot() {
    HKEY lxss;
    std::string root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &lxss)) return root;
    wchar_t id[64] = {}, name[256] = {};
    DWORD idBytes = sizeof(id) - sizeof(wchar_t), nameBytes = sizeof(name) - sizeof(wchar_t);
    if (!RegGetValueW(lxss, nullptr, L"DefaultDistribution", RRF_RT_REG_SZ, nullptr, id, &idBytes) &&
        !RegGetValueW(lxss, id, L"DistributionName", RRF_RT_REG_SZ, nullptr, name, &nameBytes))
        root = "\\\\wsl.localhost\\" + ccc::U(name);
    RegCloseKey(lxss);
    return root;
}

inline std::string programDirectory(const std::string& reported) {
    std::string path = urlPath(reported);
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path = path.substr(1);
    for (char& c : path) if (c == '/') c = '\\';
    if (!path.empty() && path[0] == '\\' && (path.size() < 2 || path[1] != '\\')) path = wslRoot() + path;
    return path;
}
#else
inline std::string programDirectory(const std::string& reported) { return urlPath(reported); }
#endif
