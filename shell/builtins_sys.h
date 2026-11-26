#pragma once
#include "shell.h"

namespace ccc {

inline int Shell::cmdTitle(const std::string& a, Ctx&) {
    baseTitle = trim(a);
    titleShown = false;
    host.setTitle(baseTitle);
    return 0;
}

inline int Shell::cmdColor(const std::string& a, Ctx&) {
    std::string t = trim(a);
    int v = t.empty() ? 0x07 : (int)strtol(t.c_str(), nullptr, 16);
    if ((v >> 4) == (v & 15) && !t.empty()) return 1;
    host.setColor(v);
    return 0;
}

inline int Shell::cmdCls(const std::string&, Ctx& c) {
    OutputBuffer::flushAll();
    if (!c.out.buf && (c.out.con || c.out.h == os::stdHandle(1))) host.clear();
    else c.out.write("\f");
    return 0;
}

inline int Shell::cmdStart(const std::string& args, Ctx& c) {
    prepareChild();
    std::string a = trim(args), title, dir;
    bool wait = false, sameWindow = false, originalEnv = false;
    os::Launch l;
    size_t i = 0;
    if (!a.empty() && a[0] == '"') { size_t e = a.find('"', 1); title = a.substr(1, e == std::string::npos ? std::string::npos : e - 1); i = e == std::string::npos ? a.size() : e + 1; }
    auto nextWord = [&]() {
        while (i < a.size() && a[i] == ' ') ++i;
        size_t e = i;
        bool q = false;
        while (e < a.size() && (q || a[e] != ' ')) { if (a[e] == '"') q = !q; ++e; }
        std::string w = unquote(a.substr(i, e - i));
        i = e;
        return w;
    };
    for (;;) {
        while (i < a.size() && a[i] == ' ') ++i;
        if (i >= a.size() || a[i] != '/') break;
        size_t e = a.find(' ', i);
        std::string word = a.substr(i, e == std::string::npos ? std::string::npos : e - i);
        if (!isSwitch(word, "WAIT|B|I|MIN|MAX|LOW|BELOWNORMAL|NORMAL|ABOVENORMAL|HIGH|REALTIME|SEPARATE|SHARED|NODE|AFFINITY|MACHINE|D")) break;
        std::string s = upper(word.substr(1));
        i = e == std::string::npos ? a.size() : e;
        if (s == "WAIT") wait = true;
        else if (s == "B") sameWindow = true;
        else if (s == "I") originalEnv = true;
        else if (s == "MIN") l.show = os::Show::Minimized;
        else if (s == "MAX") l.show = os::Show::Maximized;
        else if (s == "LOW") l.priority = os::Priority::Low;
        else if (s == "BELOWNORMAL") l.priority = os::Priority::BelowNormal;
        else if (s == "NORMAL") l.priority = os::Priority::Normal;
        else if (s == "ABOVENORMAL") l.priority = os::Priority::AboveNormal;
        else if (s == "HIGH") l.priority = os::Priority::High;
        else if (s == "REALTIME") l.priority = os::Priority::Realtime;
        else if (s == "SEPARATE" || s == "SHARED") {}
        else if (s == "NODE") l.node = atoll(nextWord().c_str());
        else if (s == "AFFINITY") l.affinity = strtoull(nextWord().c_str(), nullptr, 16);
        else if (s == "MACHINE") nextWord();
        else if (s[0] == 'D') dir = s.size() > 1 ? unquote(a.substr(a.rfind('/', i - 1) + 2, s.size() - 1)) : nextWord();
        else { c.err.line(message(Msg::StartBadParameter, { "/" + s })); return 1; }
    }
    std::string rest = trim(a.substr((std::min)(i, a.size())));
    if (rest.empty()) rest = "\"" + os::selfPath() + "\"";
    size_t e = 0;
    bool q = false;
    while (e < rest.size() && (q || rest[e] != ' ')) { if (rest[e] == '"') q = !q; ++e; }
    std::string prog = rest.substr(0, e), params = e < rest.size() ? rest.substr(e + 1) : "";
    std::string path = resolve(prog);
    std::string ext = upperExtension(path);
    bool script = isBatchExtension(ext);
    if (!path.empty() && (script || !os::opensAsDocument(ext))) {
        l.exe = path;
        l.cmdline = "\"" + path + "\" " + params;
        l.detached = ieq(path, os::selfPath());
        if (script && !sameWindow) {
            std::string line = "\"" + path + "\" " + params, escaped;
            for (char ch : trim(line)) { if (ch == '"') escaped += '\\'; escaped += ch; }
            l.exe = os::selfPath();
            l.cmdline = "\"" + l.exe + "\" " + os::NEW_CONSOLE_SWITCH + " \"" + escaped + "\"";
            l.detached = true;
        }
        else if (script) {
            std::string comspec;
            l.exe = getVar("COMSPEC", comspec) ? comspec : os::defaultComspec();
            l.cmdline = "\"" + l.exe + "\" /K \"" + path + "\" " + params;
        }
        l.title = title;
        l.dir = dir;
        l.newConsole = !sameWindow;
        l.wait = wait;
        l.env = originalEnv ? &startupEnv : nullptr;
        int code = 0;
        if (!os::launch(l, code)) return fail(c);
        return code;
    }
    if (!os::openDocument(path.empty() ? unquote(prog) : path, params, dir, l.show, wait)) return fail(c);
    return 0;
}

inline int Shell::dateTime(const std::string& args, Ctx& c, bool date) {
    Opts o = opts(args, "T");
    os::Clock t = os::localNow();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
    std::string now = date ? dateText() : std::string(buf);
    if (o.has('T')) { c.out.line(now); return 0; }
    std::string value = o.pos.empty() ? "" : o.pos[0];
    if (value.empty()) {
        if (date) c.out.line(message(Msg::CurrentDate) + dateText());
        else c.out.line(message(Msg::CurrentTime) + clockText(','));
        c.out.write(message(date ? Msg::EnterDate : Msg::EnterTime));
        if (!readInput(value, c) || trim(value).empty()) return 0;
    }
    int a = 0, b = 0, d = 0;
    if (std::sscanf(value.c_str(), "%d%*[.:/-]%d%*[.:/-]%d", &a, &b, &d) < 2) { c.err.line(message(date ? Msg::DateInvalid : Msg::TimeInvalid)); return 1; }
    if (date) { t.day = (uint16_t)a; t.month = (uint16_t)b; if (d) t.year = (uint16_t)(d < 100 ? 2000 + d : d); }
    else { t.hour = (uint16_t)a; t.minute = (uint16_t)b; t.second = (uint16_t)d; }
    if (!os::setLocalNow(t)) return fail(c);
    return 0;
}

inline int Shell::cmdFont(const std::string& args, Ctx& c) {
    auto words = splitArgs(args);
    if (words.empty()) {
        std::string info = host.fontInfo();
        if (info.empty()) { c.err.line("FONT needs the ccc window."); return 1; }
        c.out.line(info);
        return 0;
    }
    std::string family;
    float points = 0;
    for (auto& w : words) {
        char* end = nullptr;
        float v = strtof(w.c_str(), &end);
        if (end && *end == 0 && v > 0) points = v;
        else family += (family.empty() ? "" : " ") + unquote(w);
    }
    if (!host.setFont(family, points)) { c.err.line("FONT needs the ccc window."); return 1; }
    return 0;
}

inline int Shell::cmdLinewrap(const std::string& args, Ctx& c) {
    int state = host.lineWrapState();
    if (state < 0) { c.err.line("LINEWRAP needs the ccc window."); return 1; }
    std::string t = upper(trim(args));
    if (t.empty()) { c.out.line(state ? "LINEWRAP is on." : "LINEWRAP is off."); return 0; }
    if (t != "ON" && t != "OFF") return syntaxError(c);
    host.setLineWrap(t == "ON");
    return 0;
}

inline int Shell::cmdDate(const std::string& a, Ctx& c) { return dateTime(a, c, true); }

inline int Shell::cmdTime(const std::string& a, Ctx& c) { return dateTime(a, c, false); }

inline int Shell::cmdVer(const std::string&, Ctx& c) { c.out.write("\r\n" + os::versionLine() + "\r\n"); return 0; }

inline int Shell::cmdVol(const std::string& args, Ctx& c) {
    std::string d = trim(args);
    std::string root = d.empty() ? cwd() : upper(d.substr(0, 1)) + ":" + os::SLASH;
    os::Volume vol;
    if (!os::volumeInfo(root, vol)) return fail(c);
    for (const std::string& line : volumeLines(vol)) c.out.line(line);
    return 0;
}

inline int Shell::cmdVerify(const std::string& args, Ctx& c) {
    std::string t = trim(args);
    if (ieq(t, "on")) verify = true;
    else if (ieq(t, "off")) verify = false;
    else if (t.empty()) c.out.line(message(verify ? Msg::VerifyOn : Msg::VerifyOff));
    else { c.err.line(message(Msg::IncorrectParameter)); return 1; }
    return 0;
}

}
