#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "util.h"

namespace ccc {

class Environment {
public:
    Environment() { reload(); }

    void reload() {
        vars.clear();
        dirty.clear();
        for (const std::string& entry : os::environment()) {
            size_t eq = entry.find('=', 1);
            if (eq == std::string::npos) continue;
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
    }

    const std::string* find(std::string_view name) const {
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : &it->second;
    }

    void set(const std::string& name, const std::string* value) {
        auto it = vars.find(std::string_view(name));
        if (value) {
            if (it == vars.end()) vars.emplace(name, *value);
            else it->second = *value;
        }
        else if (it != vars.end()) vars.erase(it);
        if (!name.empty() && name[0] == '=') { os::setEnvironment(name, value); return; }
        dirty.push_back(name);
        if (dirty.size() > 4096) sync();
    }

    os::EnvList listWith(const std::vector<std::pair<std::string, std::string>>& defaults) const {
        std::vector<std::pair<std::string, std::string>> all(vars.begin(), vars.end());
        for (auto& d : defaults) if (!find(d.first)) all.push_back(d);
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return compareNoCase(a.first, b.first) < 0; });
        os::EnvList list;
        for (auto& [name, value] : all) list.push_back(name + "=" + value);
        return list;
    }

    void sync() {
        if (dirty.empty()) return;
        std::vector<std::string> names;
        names.swap(dirty);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return compareNoCase(a, b) < 0; });
        names.erase(std::unique(names.begin(), names.end(), ieq), names.end());
        for (const std::string& n : names) os::setEnvironment(n, find(n));
    }

private:
    struct NoCaseHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const {
            size_t h = 1469598103934665603ull;
            for (char c : s) h = (h ^ (unsigned char)(c >= 'a' && c <= 'z' ? c - 32 : c)) * 1099511628211ull;
            return h;
        }
    };
    struct NoCaseEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const { return ieq(a, b); }
    };

    std::unordered_map<std::string, std::string, NoCaseHash, NoCaseEqual> vars;
    std::vector<std::string> dirty;
};

}
