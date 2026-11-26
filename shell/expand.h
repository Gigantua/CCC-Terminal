#pragma once
#include "shell.h"

namespace ccc {

inline bool Shell::getVar(const std::string& name, std::string& value) {
    if (ieq(name, "=ExitCode") || ieq(name, "=ExitCodeAscii")) {
        char code[16];
        std::snprintf(code, sizeof(code), "%08X", (unsigned)lastExitCode);
        value = name.size() == 9 ? std::string(code) : lastExitCode >= 32 && lastExitCode < 127 ? std::string(1, (char)lastExitCode) : "";
        return true;
    }
    if (const std::string* v = env.find(name)) { value = *v; return true; }
    std::string u = upper(name);
    if (u == "CD") { value = cwd(); return true; }
    if (u == "ERRORLEVEL") { value = std::to_string(errorlevel); return true; }
    if (u == "RANDOM") { value = std::to_string(rand() % 32768); return true; }
    if (u == "DATE") { value = dateText(); return true; }
    if (u == "TIME") { value = clockText('.'); return true; }
    if (u == "CMDCMDLINE") { value = cmdline; return true; }
    if (u == "CMDEXTVERSION") { value = "2"; return true; }
    if (u == "HIGHESTNUMANODENUMBER") { value = std::to_string(os::highestNumaNode()); return true; }
    if (u == "__CD__") { value = cwd(); if (value.empty() || value.back() != os::SLASH) value += os::SLASH; return true; }
    if (u == "__APPDIR__") { value = appDir(); return true; }
    return false;
}

inline void Shell::setVar(const std::string& name, const std::string* value) {
    env.set(name, value);
}

inline std::string Shell::modify(const std::string& v, const std::string& spec) {
    if (!spec.empty() && spec[0] == '~') {
        int n = (int)v.size();
        std::string rest = spec.substr(1);
        size_t comma = rest.find(',');
        int a = atoi(rest.substr(0, comma).c_str());
        if (a < 0) a = (std::max)(0, n + a);
        a = (std::min)(a, n);
        int len = n - a;
        if (comma != std::string::npos) {
            int l = atoi(rest.substr(comma + 1).c_str());
            len = l < 0 ? (std::max)(0, n - a + l) : (std::min)(l, n - a);
        }
        return v.substr(a, len);
    }
    size_t eq = spec.find('=');
    if (eq == std::string::npos) return v;
    std::string from = spec.substr(0, eq), to = spec.substr(eq + 1);
    bool star = !from.empty() && from[0] == '*';
    if (star) from = from.substr(1);
    if (from.empty()) return v;
    std::string lv = upper(v), lf = upper(from), out;
    size_t pos = 0;
    if (star) {
        size_t f = lv.find(lf);
        return f == std::string::npos ? v : to + v.substr(f + from.size());
    }
    for (;;) {
        size_t f = lv.find(lf, pos);
        if (f == std::string::npos) { out += v.substr(pos); break; }
        out += v.substr(pos, f - pos) + to;
        pos = f + from.size();
    }
    return out;
}

inline std::string Shell::tildeMods(const std::string& mods, const std::string& value) {
    std::string v = unquote(value);
    if (mods.empty()) return v;
    std::string m = mods;
    size_t dollar = m.find('$');
    if (dollar != std::string::npos) {
        std::string var = m.substr(dollar + 1);
        m = m.substr(0, dollar);
        std::string list;
        getVar(var, list);
        bool found = false;
        for (auto& dir : splitList(list, os::PATH_LIST_SEPARATOR)) {
            std::string p = dir + os::SLASH + v;
            if (exists(p)) { v = fullPath(p); found = true; break; }
        }
        if (!found) return "";
        if (m.empty()) return v;
    }
    if (v.empty()) return "";
    std::string full = fullPath(v);
    auto has = [&](const char* letters) { return m.find_first_of(letters) != std::string::npos; };
    bool f = has("fF"), d = has("dD"), p = has("pP"), n = has("nN"), x = has("xX"), a = has("aA"), t = has("tT"), z = has("zZ"), sh = has("sS");
    os::FileInfo fa;
    bool exists = (a || t || z || sh) && os::fileInfo(full, fa);
    if (sh && exists) {
        std::string brief = os::shortPath(full);
        if (!brief.empty()) full = brief;
    }
    std::string out;
    if (a && exists) {
        std::string s = "---------";
        uint32_t at = fa.attr;
        if (at & os::ATTR_DIRECTORY) s[0] = 'd';
        if (at & os::ATTR_READONLY) s[1] = 'r';
        if (at & os::ATTR_ARCHIVE) s[2] = 'a';
        if (at & os::ATTR_HIDDEN) s[3] = 'h';
        if (at & os::ATTR_SYSTEM) s[4] = 's';
        if (at & os::ATTR_COMPRESSED) s[5] = 'c';
        if (at & os::ATTR_OFFLINE) s[6] = 'o';
        if (at & os::ATTR_TEMPORARY) s[7] = 't';
        if (at & os::ATTR_REPARSE) s[8] = 'l';
        out += s;
    }
    if (t && exists) { if (!out.empty()) out += ' '; out += fileTime(fa.written); }
    if (z && exists) { if (!out.empty()) out += ' '; out += std::to_string(fa.size); }
    if ((f || (sh && !(a || t || z))) && !(d || p || n || x)) d = p = n = x = true;
    if (d || p || n || x) {
        if (!out.empty()) out += ' ';
        size_t slash = full.find_last_of(os::SLASH);
        std::string dir = slash == std::string::npos ? "" : full.substr(0, slash + 1);
        std::string file = slash == std::string::npos ? full : full.substr(slash + 1);
        size_t dot = file.find_last_of('.');
        std::string drive = os::driveOf(full);
        if (d) out += drive;
        if (p) out += dir.size() > drive.size() ? dir.substr(drive.size()) : std::string(1, os::SLASH);
        if (n) out += dot == std::string::npos ? file : file.substr(0, dot);
        if (x && dot != std::string::npos) out += file.substr(dot);
    }
    return out;
}

inline std::string Shell::fileTime(os::FileTime ft) {
    os::Clock c = os::localTime(ft);
    return os::shortDate(c) + "  " + os::shortTime(c);
}

inline std::string Shell::batchArg(Batch* b, int k) {
    if (!b) return "";
    size_t idx = (size_t)k;
    return idx < b->args.size() ? b->args[idx] : "";
}

inline std::string Shell::expandPercent(const std::string& s, Batch* b) {
    if (s.find('%') == std::string::npos) return s;
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '%') { o += c; continue; }
        if (b && i + 1 < s.size() && s[i + 1] == '%') { o += '%'; ++i; continue; }
        if (b && i + 1 < s.size() && s[i + 1] == '*') {
            o += b->tail;
            ++i;
            continue;
        }
        if (b && i + 1 < s.size() && isdigit((unsigned char)s[i + 1])) { o += batchArg(b, s[i + 1] - '0'); ++i; continue; }
        if (b && i + 1 < s.size() && s[i + 1] == '~') {
            size_t j = i + 2;
            std::string mods;
            while (j < s.size() && !isdigit((unsigned char)s[j]) && (isalpha((unsigned char)s[j]) || s[j] == '$' || s[j] == ':')) {
                if (s[j] == '$') { size_t colon = s.find(':', j); if (colon == std::string::npos) break; mods += s.substr(j, colon - j); j = colon + 1; continue; }
                mods += s[j++];
            }
            if (j < s.size() && isdigit((unsigned char)s[j])) { o += tildeMods(mods, s[j] == '0' && !mods.empty() ? b->path : batchArg(b, s[j] - '0')); i = j; continue; }
            o += '%';
            continue;
        }
        size_t close = s.find_first_of("%\n", i + 1);
        if (close != std::string::npos && s[close] == '\n') close = std::string::npos;
        if (close == std::string::npos) { if (!b) o += c; continue; }
        std::string name = s.substr(i + 1, close - i - 1);
        if (name.empty()) { if (!b) o += "%%"; i = close; continue; }
        std::string spec;
        size_t colon = name.find(':');
        if (colon != std::string::npos && colon > 0) { spec = name.substr(colon + 1); name = name.substr(0, colon); }
        std::string v;
        if (getVar(name, v)) { o += spec.empty() ? v : modify(v, spec); i = close; }
        else if (b) i = close;
        else { o += s.substr(i, close - i + 1); i = close; }
    }
    return o;
}

inline std::string Shell::expandForVars(const std::string& s) {
    if (forVars.empty() || !contains(s, '%')) return s;
    std::string t;
    {
        const std::string& in = s;
        std::string& o = t;
        size_t i = 0;
        while (i < in.size()) {
            const char* pct = findChar(in.data() + i, in.data() + in.size(), '%');
            size_t at = pct ? (size_t)(pct - in.data()) : in.size();
            o.append(in, i, at - i);
            i = at;
            if (i >= in.size()) break;
            if (i + 1 < in.size()) {
                if (forVars.has(in[i + 1])) { o += forVars.get(in[i + 1]); i += 2; continue; }
                if (in[i + 1] == '~') {
                    const std::string& t = in;
                    size_t j = i + 2;
                    std::string mods;
                    while (j < t.size()) {
                        if (t[j] == '$') { size_t colon = t.find(':', j); if (colon == std::string::npos) break; mods += t.substr(j, colon - j); j = colon + 1; continue; }
                        if (!isalpha((unsigned char)t[j])) break;
                        mods += t[j++];
                    }
                    if (!mods.empty() && forVars.has(mods.back())) {
                        char v = mods.back();
                        mods.pop_back();
                        o += tildeMods(mods, forVars.get(v));
                        i = j;
                        continue;
                    }
                }
            }
            o += in[i++];
        }
    }
    return t;
}

inline std::string Shell::expandLate(const std::string& s) {
    std::string t = expandForVars(s);
    if (delayed && contains(t, '!')) {
        std::string o;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '^' && i + 1 < t.size()) { o += t[++i]; continue; }
            if (t[i] != '!') { o += t[i]; continue; }
            size_t close = t.find('!', i + 1);
            if (close == std::string::npos) continue;
            std::string name = t.substr(i + 1, close - i - 1), spec, v;
            size_t colon = name.find(':');
            if (colon != std::string::npos && colon > 0) { spec = name.substr(colon + 1); name = name.substr(0, colon); }
            if (getVar(name, v)) o += spec.empty() ? v : modify(v, spec);
            i = close;
        }
        t = o;
    }
    return t;
}

}
