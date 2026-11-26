#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

struct WindowSettings {
    struct Rect { int left = 0, top = 0, right = 0, bottom = 0; };
    std::string font;
    float points = 0;
    Rect rect;
    bool maximized = false, hasRect = false;

    static std::filesystem::path path() {
#ifdef _WIN32
        const char* base = std::getenv("APPDATA");
        return base ? std::filesystem::path(base) / "ccc" / "window.txt" : std::filesystem::path();
#else
        const char* config = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        std::filesystem::path base = config ? std::filesystem::path(config) : home ? std::filesystem::path(home) / ".config" : std::filesystem::path();
        return base.empty() ? base : base / "ccc" / "window.txt";
#endif
    }

    bool load() {
        std::ifstream in(path());
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            std::istringstream v(value);
            if (key == "font") font = value;
            else if (key == "points") v >> points;
            else if (key == "maximized") maximized = value == "1";
            else if (key == "rect") hasRect = static_cast<bool>(v >> rect.left >> rect.top >> rect.right >> rect.bottom);
        }
        return true;
    }

    void save() const {
        std::filesystem::path p = path();
        if (p.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::trunc);
        out << "font=" << font << "\npoints=" << points << "\nmaximized=" << (maximized ? 1 : 0) << "\n";
        if (hasRect) out << "rect=" << rect.left << " " << rect.top << " " << rect.right << " " << rect.bottom << "\n";
    }
};
