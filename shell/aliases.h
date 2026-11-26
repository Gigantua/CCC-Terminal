#pragma once
#include <algorithm>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

struct FoldedLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](wchar_t x, wchar_t y) { return std::towlower(x) < std::towlower(y); });
    }
};

class ConsoleAliases {
public:
    void add(const std::wstring& exe, const std::wstring& source, const std::wstring& target) {
        if (target.empty()) { table[exe].erase(source); if (table[exe].empty()) table.erase(exe); }
        else table[exe][source] = target;
    }

    bool find(const std::wstring& exe, const std::wstring& source, std::wstring& target) const {
        auto e = table.find(exe);
        if (e == table.end()) return false;
        auto a = e->second.find(source);
        if (a == e->second.end()) return false;
        target = a->second;
        return true;
    }

    std::wstring list(const std::wstring& exe) const {
        std::wstring out;
        auto e = table.find(exe);
        if (e != table.end())
            for (auto& [source, target] : e->second) out += source + L"=" + target + L'\0';
        return out;
    }

    std::wstring exes() const {
        std::wstring out;
        for (auto& entry : table) out += entry.first + L'\0';
        return out;
    }

    bool expand(const std::wstring& exe, std::wstring_view text, std::wstring& out) const {
        auto e = table.find(exe);
        if (e == table.end() || e->second.empty()) return false;
        std::wstring_view args[10];
        size_t argc = 0;
        for (size_t at = 0; at < text.size() && argc < 10;) {
            size_t end = text.find(L' ', at);
            std::wstring_view arg = text.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos : end - at);
            if (arg.empty()) break;
            args[argc++] = arg;
            if (end == std::wstring_view::npos) break;
            at = text.find_first_not_of(L' ', end);
            if (at == std::wstring_view::npos) break;
        }
        if (!argc) return false;
        auto a = e->second.find(std::wstring(args[0]));
        if (a == e->second.end() || a->second.empty()) return false;
        const std::wstring& target = a->second;
        out.clear();
        for (size_t i = 0; i < target.size();) {
            wchar_t ch = target[i++];
            if (ch != L'$' || i == target.size()) { out += ch; continue; }
            wchar_t next = target[i++], lower = (wchar_t)std::towlower(next);
            if (lower >= L'1' && lower <= L'9') { if ((size_t)(lower - L'0') < argc) out += args[lower - L'0']; }
            else if (lower == L'*') { if (argc > 1) out.append(args[1].data(), text.data() + text.size()); }
            else if (lower == L'l') out += L'<';
            else if (lower == L'g') out += L'>';
            else if (lower == L'b') out += L'|';
            else if (lower == L't') out += L"\r\n";
            else { out += L'$'; out += next; }
        }
        out += L"\r\n";
        return true;
    }

private:
    std::map<std::wstring, std::map<std::wstring, std::wstring, FoldedLess>, FoldedLess> table;
};

class CommandHistories {
public:
    size_t limit = 50;

    std::vector<std::wstring>& of(const std::wstring& exe) { return lines[exe]; }

    void add(const std::wstring& exe, const std::wstring& line) {
        std::vector<std::wstring>& h = lines[exe];
        if (line.empty() || (!h.empty() && h.back() == line)) return;
        h.push_back(line);
        if (h.size() > limit) h.erase(h.begin(), h.begin() + (std::ptrdiff_t)(h.size() - limit));
    }

    std::wstring joined(const std::wstring& exe) {
        std::wstring out;
        for (const std::wstring& line : lines[exe]) out += line + L'\0';
        return out;
    }

    void expunge(const std::wstring& exe) { lines.erase(exe); }

private:
    std::map<std::wstring, std::vector<std::wstring>, FoldedLess> lines;
};
