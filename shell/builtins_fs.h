#pragma once
#include "shell.h"

namespace ccc {

inline std::array<std::string, 2> Shell::volumeLines(const os::Volume& v) {
    char serial[16];
    std::snprintf(serial, sizeof(serial), "%04X-%04X", v.serial >> 16, v.serial & 0xFFFF);
    return { v.label.empty() ? message(Msg::VolumeNoLabel, { v.drive }) : message(Msg::VolumeLabel, { v.drive, v.label }), message(Msg::VolumeSerial, { serial }) };
}

inline int Shell::cmdCd(const std::string& args, Ctx& c) {
    std::string a = trim(args);
    bool d = false;
    if (a.size() >= 2 && (a[0] == '/') && toupper((unsigned char)a[1]) == 'D' && isSwitch(a.substr(0, a.find_first_of(" \t")), "D")) { d = true; a = trim(a.substr(2)); }
    a = unquote(a);
    if (a.empty()) { c.out.line(cwd()); return 0; }
    if (a.size() == 2 && a[1] == ':' && !os::driveOf(a).empty()) {
        if (d) return changeDrive(a, c);
        std::string v;
        c.out.line(getVar("=" + upper(a), v) ? v : upper(a) + os::SLASH);
        return 0;
    }
    std::string target = fullPath(a);
    std::string drive = upper(os::driveOf(target));
    if (!d && drive != upper(os::driveOf(cwd()))) {
        setVar("=" + drive, &target);
        return 0;
    }
    if (!os::setCurrentDirectory(target)) return fail(c);
    std::string now = cwd();
    if (!drive.empty()) setVar("=" + upper(os::driveOf(now)), &now);
    return 0;
}

inline int Shell::cmdMd(const std::string& args, Ctx& c) {
    auto a = splitArgs(args);
    if (a.empty()) return syntaxError(c);
    int r = 0;
    for (auto& p0 : a) {
        std::string p = fullPath(unquote(p0));
        if (exists(p)) { c.err.line(message(Msg::AlreadyExists, { unquote(p0) })); r = 1; continue; }
        std::error_code ec;
        std::filesystem::create_directories(fsPath(p), ec);
        if (!exists(p)) r = fail(c);
    }
    return r;
}

inline bool Shell::removeTree(const std::string& d) {
    for (const os::FileInfo& f : os::listFiles(d + os::SLASH + "*")) {
        if (f.name == "." || f.name == "..") continue;
        std::string p = d + os::SLASH + f.name;
        if (f.attr & os::ATTR_READONLY) os::makeWritable(p);
        if ((f.attr & os::ATTR_DIRECTORY) && !(f.attr & os::ATTR_REPARSE)) removeTree(p);
        else if (f.attr & os::ATTR_DIRECTORY) os::removeDirectory(p);
        else os::removeFile(p);
    }
    return os::removeDirectory(d);
}

inline int Shell::cmdRd(const std::string& args, Ctx& c) {
    Opts o = opts(args, "S|Q");
    if (o.pos.empty()) return syntaxError(c);
    int r = 0;
    for (auto& p0 : o.pos) {
        std::string p = fullPath(unquote(p0));
        if (o.has('S')) {
            if (!o.has('Q')) {
                c.out.write(message(Msg::AreYouSure, { unquote(p0) }));
                std::string ans;
                if (!readAnswer(ans, c) || answerKey(ans, Msg::NoYesKeys, "NY") != 'Y') continue;
            }
            if (!removeTree(p)) r = fail(c);
        }
        else if (!os::removeDirectory(p)) r = fail(c);
    }
    return r;
}

inline int Shell::cmdPushd(const std::string& args, Ctx& c) {
    std::string a = unquote(trim(args));
    std::string here = cwd();
    if (a.empty()) return 0;
    if (a.size() > 2 && a[0] == '\\' && a[1] == '\\') {
        char letter = 0;
        if (!os::connectNetworkDrive(a, letter)) return fail(c);
        dirStack.push_back(here + "|" + std::string(1, letter));
        os::setCurrentDirectory(std::string(1, letter) + ":" + os::SLASH);
        return 0;
    }
    if (!os::setCurrentDirectory(fullPath(a))) return fail(c);
    dirStack.push_back(here);
    return 0;
}

inline int Shell::cmdPopd(const std::string&, Ctx&) {
    if (dirStack.empty()) return 0;
    std::string top = dirStack.back();
    dirStack.pop_back();
    size_t bar = top.find('|');
    os::setCurrentDirectory(top.substr(0, bar));
    if (bar != std::string::npos) os::disconnectNetworkDrive(top[bar + 1]);
    return 0;
}

inline int Shell::cmdDir(const std::string& args, Ctx& c) {
    Opts o = opts(args, "A:DHSRAILO-|O:NSEDG-|T:CAW|B|S|W|D|L|P|Q|R|X|C|N|4|-C");
    std::string attrs, order;
    bool attrAll = false, bare = false, sub = false, wide = false, byColumn = false, lowercase = false, pause = false,
         owner = false, streams = false, shortNames = false, separators = true;
    char timeField = 'W';
    for (auto& s : o.sws) {
        std::string v = s.size() > 1 && s[1] == ':' ? s.substr(2) : s.substr(1);
        switch (s[0]) {
        case 'A': attrAll = true; attrs = v; break;
        case 'O': order = v.empty() ? "GN" : v; break;
        case 'T': timeField = v.empty() ? 'W' : v[0]; break;
        case 'B': bare = true; break;
        case 'S': sub = true; break;
        case 'W': wide = true; break;
        case 'D': wide = byColumn = true; break;
        case 'L': lowercase = true; break;
        case 'P': pause = true; break;
        case 'Q': owner = true; break;
        case 'R': streams = true; break;
        case 'X': shortNames = true; break;
        case 'C': separators = true; break;
        case 'N': case '4': break;
        case '-': if (s == "-C") { separators = false; break; } [[fallthrough]];
        default: c.err.line(message(Msg::InvalidSwitch, { s })); return 1;
        }
    }
    auto num = [&](unsigned long long v) { return separators ? fmtGroup(v) : std::to_string(v); };
    auto when = [&](const os::FileInfo& f) { return timeField == 'C' ? f.created : timeField == 'A' ? f.accessed : f.written; };

    std::string out;
    int linesOnScreen = 0;
    int screen = (std::max)(2, host.screenRows() - 1);
    auto emit = [&](const std::string& line) {
        out += line + "\r\n";
        if (pause && ++linesOnScreen >= screen) {
            c.out.write(out);
            out.clear();
            c.out.write(message(Msg::PressAnyKey));
            if (c.in.hasHandle()) { std::string l; readInput(l, c); } else { OutputBuffer::flushAll(); host.waitKey(); }
            c.out.write("\r\n");
            linesOnScreen = 0;
        }
        else if (out.size() > (1 << 20)) { c.out.write(out); out.clear(); }
    };

    std::vector<std::string> targets = o.pos;
    if (targets.empty()) targets.push_back(".");
    int rc = 0;
    bool headerDone = false;
    for (auto& t : targets) {
        std::string full = fullPath(unquote(t)), pattern = "*";
        uint32_t ta = os::attributes(full);
        if (ta == os::ATTR_INVALID || !(ta & os::ATTR_DIRECTORY)) {
            size_t sl = full.find_last_of(os::SLASH);
            pattern = full.substr(sl + 1);
            full = full.substr(0, sl == full.find(os::SLASH) ? sl + 1 : sl);
        }
        if (!bare && !headerDone) {
            os::Volume vol;
            os::volumeInfo(full, vol);
            for (const std::string& line : volumeLines(vol)) emit(line);
            headerDone = true;
        }
        unsigned long long totalFiles = 0, totalBytes = 0, totalDirs = 0;
        bool found = false;
        std::function<void(const std::string&)> walk = [&](const std::string& dir) {
            checkInterrupt();
            std::string base = withSlash(dir);
            std::vector<os::FileInfo> shown;
            for (os::FileInfo& f : os::listFiles(base + pattern, shortNames))
                if (attrAll ? attrMatch(f.attr, attrs) : !(f.attr & (os::ATTR_HIDDEN | os::ATTR_SYSTEM))) shown.push_back(std::move(f));
            if (!order.empty()) {
                std::stable_sort(shown.begin(), shown.end(), [&](const os::FileInfo& x, const os::FileInfo& y) {
                    for (size_t k = 0; k < order.size(); ++k) {
                        bool rev = order[k] == '-';
                        if (rev && ++k >= order.size()) break;
                        int cmp = 0;
                        switch (order[k]) {
                        case 'N': cmp = compareNoCase(x.name, y.name); break;
                        case 'E': { auto ex = [](const std::string& s) { size_t d = s.rfind('.'); return d == std::string::npos ? std::string_view() : std::string_view(s).substr(d); };
                            cmp = compareNoCase(ex(x.name), ex(y.name)); break; }
                        case 'S': cmp = x.size < y.size ? -1 : x.size > y.size; break;
                        case 'D': { os::FileTime a = when(x), b = when(y); cmp = a < b ? -1 : a > b; break; }
                        case 'G': cmp = (int)!(x.attr & os::ATTR_DIRECTORY) - (int)!(y.attr & os::ATTR_DIRECTORY); break;
                        default: break;
                        }
                        if (cmp) return rev ? cmp > 0 : cmp < 0;
                    }
                    return false;
                });
            }
            if (!shown.empty()) {
                found = true;
                unsigned long long files = 0, bytes = 0, dirs = 0;
                if (!bare) { emit(""); emit(message(Msg::DirectoryOf, { dir })); }
                std::vector<std::string> cells;
                size_t widest = 0;
                for (auto& e : shown) {
                    std::string name = e.name;
                    if (lowercase) name = lower(name);
                    bool isDir = (e.attr & os::ATTR_DIRECTORY) != 0;
                    if (isDir) ++dirs; else { ++files; bytes += e.size; }
                    if (bare) {
                        if (name == "." || name == "..") continue;
                        emit((sub ? base : "") + name);
                        continue;
                    }
                    if (wide) {
                        cells.push_back(isDir ? "[" + name + "]" : name);
                        widest = (std::max)(widest, cells.back().size());
                        continue;
                    }
                    std::string kind;
                    if (e.attr & os::ATTR_REPARSE)
                        kind = e.junction ? "    <JUNCTION>     " : isDir ? "    <SYMLINKD>     " : "    <SYMLINK>      ";
                    else kind = isDir ? "    <DIR>          " : pad(num(e.size), 18) + " ";
                    std::string line = fileTime(when(e)) + kind;
                    if (owner) { std::string ow = os::fileOwner(base + name); ow.resize((std::max)(ow.size(), (size_t)22), ' '); line += ow + " "; }
                    if (shortNames) { std::string sn = lowercase ? lower(e.shortName) : e.shortName; sn.resize(12, ' '); line += sn + " "; }
                    emit(line + name);
                    if (streams)
                        for (auto& [stream, size] : os::alternateStreams(base + name))
                            emit(std::string(fileTime(when(e)).size(), ' ') + pad(num(size), 18) + " " + name + stream);
                }
                if (wide && !cells.empty()) {
                    size_t width = widest + 3;
                    size_t screenW = (size_t)host.screenColumns();
                    size_t perRow = (std::max)((size_t)1, screenW / width);
                    size_t rows = (cells.size() + (byColumn ? 1 : 0) + perRow - 1) / perRow;
                    size_t columns = byColumn ? (cells.size() + rows - 1) / rows : perRow;
                    for (size_t r = 0; r < rows; ++r) {
                        std::string line;
                        for (size_t k = 0; k < columns; ++k) {
                            size_t idx = byColumn ? k * rows + r : r * perRow + k;
                            if (idx >= cells.size()) break;
                            std::string cell = cells[idx];
                            if (k + 1 < perRow) cell.resize(width, ' ');
                            line += cell;
                        }
                        emit(line);
                    }
                }
                if (!bare) emit(message(Msg::FilesSummary, { pad(std::to_string(files), 11), pad(num(bytes), 14) }));
                totalFiles += files;
                totalBytes += bytes;
                totalDirs += dirs;
            }
            if (sub) {
                std::vector<std::string> subdirs;
                for (const os::FileInfo& d : os::listFiles(base + "*"))
                    if ((d.attr & os::ATTR_DIRECTORY) && !(d.attr & os::ATTR_REPARSE) && d.name != "." && d.name != "..") subdirs.push_back(d.name);
                for (auto& sd : subdirs) walk(base + sd);
            }
        };
        walk(full);
        if (!found) {
            if (!bare) { emit(""); emit(message(Msg::DirectoryOf, { full })); }
            c.out.write(out);
            out.clear();
            c.err.line(message(Msg::FileNotFound));
            rc = 1;
            continue;
        }
        if (!bare) {
            if (sub) { emit(""); emit(message(Msg::TotalFilesListed)); emit(message(Msg::FilesSummary, { pad(std::to_string(totalFiles), 11), pad(num(totalBytes), 14) })); }
            emit(message(Msg::DirsSummary, { pad(std::to_string(totalDirs), 11), pad(num(freeSpace(full)), 14) }));
        }
    }
    c.out.write(out);
    return rc;
}

inline bool Shell::readInput(std::string& line, Ctx& c) {
    OutputBuffer::flushAll();
    if (!c.in.hasHandle()) return host.readLine(line);
    line.clear();
    char ch;
    while (os::read(c.in.h, &ch, 1)) { if (ch == '\n') return true; if (ch != '\r') line += ch; }
    return !line.empty();
}

inline bool Shell::readAnswer(std::string& answer, Ctx& c) {
    if (!readInput(answer, c)) return false;
    if (c.in.hasHandle() && !os::isConsole(c.in.h)) c.out.line(answer);
    return true;
}

inline char Shell::askOverwrite(const std::string& name, Ctx& c) {
    for (;;) {
        c.out.write(message(Msg::OverwritePrompt, { name }));
        std::string a;
        if (!readAnswer(a, c)) return 'N';
        if (char k = answerKey(a, Msg::NoYesAllKeys, "NYA")) return k;
    }
}

inline bool Shell::promptOverwrite(const std::vector<std::string>& sws) {
    bool ask = batch() == nullptr;
    std::string preset;
    if (getVar("COPYCMD", preset)) for (auto& s : splitArgs(preset)) { if (ieq(s, "/Y")) ask = false; else if (ieq(s, "/-Y")) ask = true; }
    for (auto& s : sws) { if (s == "Y") ask = false; else if (s == "-Y") ask = true; }
    return ask;
}

inline bool Shell::isDevice(const std::string& name, const char* device) {
    std::string n = name;
    if (!n.empty() && n.back() == ':') n.pop_back();
    return ieq(n, device);
}

template <class Emit>
inline void Shell::readUntilCtrlZ(Ctx& c, Emit emit) {
    std::string line;
    while (readInput(line, c)) {
        size_t z = line.find('\x1A');
        if (z != std::string::npos) { if (z) emit(line.substr(0, z)); return; }
        emit(line + "\r\n");
    }
}

inline int Shell::cmdCopy(const std::string& args, Ctx& c) {
    std::vector<std::string> parts, sws;
    bool binary = false, binarySet = false, verifyCopy = false, shortName = false;
    uint32_t flags = 0;
    for (auto& a : splitArgs(args)) {
        if (!isSwitch(a, "A|B|V|N|Y|-Y|Z|L|D")) { parts.push_back(a); continue; }
        std::string s = upper(a.substr(1));
        sws.push_back(s);
        if (s == "B") binary = binarySet = true;
        else if (s == "A") { binary = false; binarySet = true; }
        else if (s == "V") verifyCopy = true;
        else if (s == "N") shortName = true;
        else if (s == "Z") flags |= os::COPY_RESTARTABLE;
        else if (s == "L") flags |= os::COPY_SYMLINK;
        else if (s == "D") flags |= os::COPY_DECRYPTED;
        else if (s != "Y" && s != "-Y") return syntaxError(c);
    }
    if (parts.empty()) return syntaxError(c);
    bool ask = promptOverwrite(sws), all = false;
    std::vector<std::string> tok;
    {
        std::string cur;
        for (auto& p : parts) {
            if (p == "+") { cur += "+"; continue; }
            if (!cur.empty() && cur.back() == '+') cur += p;
            else { if (!cur.empty()) tok.push_back(cur); cur = p; }
        }
        if (!cur.empty()) tok.push_back(cur);
    }
    std::string src = tok[0], dest = tok.size() > 1 ? unquote(tok[1]) : "";
    if (isDevice(unquote(src), "NUL") || isDevice(unquote(src), "CON")) {
        if (dest.empty()) return syntaxError(c);
        std::string data;
        if (isDevice(unquote(src), "CON")) readUntilCtrlZ(c, [&](const std::string& part) { data += part; });
        os::Handle out = os::openWrite(fullPath(dest), false);
        if (out == os::NO_HANDLE) return fail(c);
        if (!data.empty()) os::write(out, data.data(), data.size());
        os::close(out);
        c.out.line(message(Msg::FilesCopied, { pad("1", 9) }));
        return 0;
    }
    std::vector<std::string> srcs = splitList(src, '+');
    bool concat = srcs.size() > 1;
    std::vector<std::string> files;
    for (auto& s : srcs) {
        std::string f = fullPath(s);
        uint32_t a = os::attributes(f);
        if (a != os::ATTR_INVALID && (a & os::ATTR_DIRECTORY)) f += std::string(1, os::SLASH) + "*";
        if (wild(f)) { std::string d = dirOf(f); for (auto& e : list(f)) if (!(e.attr & os::ATTR_DIRECTORY)) files.push_back(d + e.name); }
        else if (a != os::ATTR_INVALID) files.push_back(f);
        else return fail(c, os::ERR_FILE_NOT_FOUND);
    }
    if (files.empty()) return copyFailed(c, systemMessage(os::ERR_FILE_NOT_FOUND));
    if (wild(srcs[0]) && srcs.size() == 1 && files.size() > 1 && !dest.empty()) {
        if (!isDir(fullPath(dest))) concat = true;
    }
    if (!binarySet) binary = !concat;
    std::string d = fullPath(dest.empty() ? "." : dest);
    bool destDir = isDir(d);
    auto nameOf = [&](const std::string& f) {
        if (shortName) { std::string sp = os::shortPath(f); if (!sp.empty()) return afterSlash(sp); }
        return afterSlash(f);
    };
    auto sameSize = [](const std::string& a, const std::string& b) {
        os::FileInfo x, y;
        return os::fileInfo(a, x) && os::fileInfo(b, y) && x.size == y.size;
    };
    int copied = 0;
    if (concat) {
        std::string target = destDir ? d + os::SLASH + nameOf(files[0]) : d;
        if (ask && exists(target) && !ieq(fullPath(files[0]), target)) {
            if (askOverwrite(afterSlash(target), c) == 'N') { c.out.line(message(Msg::FilesCopied, { pad("0", 9) })); return 0; }
        }
        os::Handle out = os::openWrite(target, false);
        if (out == os::NO_HANDLE) return fail(c);
        std::vector<char> buf(1 << 20);
        for (auto& f : files) {
            checkInterrupt();
            if (ieq(fullPath(f), fullPath(target))) continue;
            os::Handle in = os::openRead(f);
            if (in == os::NO_HANDLE) continue;
            if (files.size() > 1) c.out.line(afterSlash(f));
            while (size_t n = os::read(in, buf.data(), buf.size())) {
                const char* eof = binary ? nullptr : (const char*)memchr(buf.data(), 0x1A, n);
                os::write(out, buf.data(), eof ? (size_t)(eof - buf.data()) : n);
                if (eof) break;
            }
            os::close(in);
        }
        if (!binary) os::write(out, "\x1A", 1);
        os::close(out);
        copied = 1;
    }
    else {
        for (auto& f : files) {
            checkInterrupt();
            std::string name = nameOf(f);
            std::string target = destDir ? d + os::SLASH + name : d;
            if (ieq(f, target)) return copyFailed(c, message(Msg::CopyOntoItself));
            if (files.size() > 1 || wild(srcs[0])) c.out.line(afterSlash(f));
            if (ask && !all && exists(target)) {
                char k = askOverwrite(afterSlash(target), c);
                if (k == 'N') continue;
                if (k == 'A') all = true;
            }
            if (!os::copyFile(f, target, flags)) return fail(c);
            if (verifyCopy && !(flags & os::COPY_SYMLINK) && !sameSize(f, target)) { c.err.line(message(Msg::VerifyError, { target })); return 1; }
            ++copied;
        }
    }
    c.out.line(message(Msg::FilesCopied, { pad(std::to_string(copied), 9) }));
    return 0;
}

inline int Shell::cmdMove(const std::string& args, Ctx& c) {
    Opts o = opts(args, "Y|-Y");
    for (auto& s : o.sws) if (s != "Y" && s != "-Y") return syntaxError(c);
    if (o.pos.size() < 2) return syntaxError(c);
    bool ask = promptOverwrite(o.sws), all = false;
    std::string dest = fullPath(unquote(o.pos.back()));
    bool destDir = isDir(dest);
    std::vector<std::string> files;
    for (size_t k = 0; k + 1 < o.pos.size(); ++k) {
        std::string src = fullPath(unquote(o.pos[k]));
        if (wild(src)) { std::string d = dirOf(src); for (auto& e : list(src)) if (e.name != "." && e.name != "..") files.push_back(d + e.name); }
        else files.push_back(src);
    }
    if (files.size() > 1 && !destDir) { c.err.line(message(Msg::MoveManyToOne)); return 1; }
    int moved = 0, movedDirs = 0;
    for (auto& f : files) {
        checkInterrupt();
        uint32_t a = os::attributes(f);
        if (a == os::ATTR_INVALID) return fail(c, os::ERR_FILE_NOT_FOUND);
        std::string target = destDir ? dest + os::SLASH + afterSlash(f) : dest;
        if (files.size() > 1) c.out.line(f);
        if (ask && !all && !(a & os::ATTR_DIRECTORY) && exists(target)) {
            char k = askOverwrite(target, c);
            if (k == 'N') continue;
            if (k == 'A') all = true;
        }
        if (!os::moveFile(f, target)) return fail(c);
        if (a & os::ATTR_DIRECTORY) ++movedDirs; else ++moved;
    }
    if (movedDirs && !moved) c.out.line(message(Msg::DirsMoved, { pad(std::to_string(movedDirs), 9) }));
    else c.out.line(message(Msg::FilesMoved, { pad(std::to_string(moved), 9) }));
    return 0;
}

inline int Shell::cmdDel(const std::string& args, Ctx& c) {
    Opts o = opts(args, "P|F|S|Q|A:RHSAILO-");
    if (o.pos.empty()) return syntaxError(c);
    bool force = o.has('F'), quiet = o.has('Q'), recurse = o.has('S'), prompt = o.has('P');
    std::string attrs;
    bool attrFilter = false;
    for (auto& s : o.sws) {
        if (s[0] == 'A') { attrFilter = true; attrs = s.substr(s.size() > 1 && s[1] == ':' ? 2 : 1); }
        else if (s != "P" && s != "F" && s != "S" && s != "Q") { c.err.line(message(Msg::InvalidSwitch, { s })); return 1; }
    }
    std::string slashStar = std::string(1, os::SLASH) + "*";
    int r = 0;
    for (auto& p0 : o.pos) {
        std::string p = fullPath(unquote(p0));
        uint32_t a = os::attributes(p);
        if (a != os::ATTR_INVALID && (a & os::ATTR_DIRECTORY)) p += slashStar;
        if (!quiet && (p.ends_with(slashStar) || p.ends_with(slashStar + ".*"))) {
            c.out.write(message(Msg::AreYouSure, { dirOf(p) + "*" }));
            std::string ans;
            if (!readAnswer(ans, c) || answerKey(ans, Msg::NoYesKeys, "NY") != 'Y') continue;
        }
        std::string pattern = afterSlash(p);
        bool any = false;
        std::function<void(const std::string&)> walk = [&](const std::string& dir) {
            for (auto& e : list(dir + pattern)) {
                if (e.attr & os::ATTR_DIRECTORY) continue;
                if (attrFilter ? !attrMatch(e.attr, attrs) : (e.attr & (os::ATTR_HIDDEN | os::ATTR_SYSTEM)) != 0) continue;
                checkInterrupt();
                std::string f = dir + e.name;
                any = true;
                if (prompt) {
                    c.out.write(message(Msg::DeletePrompt, { f }));
                    std::string ans;
                    if (!readAnswer(ans, c) || answerKey(ans, Msg::NoYesKeys, "NY") != 'Y') continue;
                }
                if ((e.attr & os::ATTR_READONLY) && !force) { c.err.line(f); r = fail(c, os::ERR_ACCESS_DENIED); continue; }
                if (e.attr & os::ATTR_READONLY) os::makeWritable(f);
                if (!os::removeFile(f)) { c.err.line(f); r = fail(c); }
                else if (recurse && !quiet) c.out.line(message(Msg::DeletedFile, { f }));
            }
            if (recurse)
                for (auto& e : list(dir + "*"))
                    if ((e.attr & os::ATTR_DIRECTORY) && e.name != "." && e.name != "..") walk(dir + e.name + os::SLASH);
        };
        walk(dirOf(p));
        if (!any && !wild(pattern)) c.err.line(message(Msg::CouldNotFind, { p }));
    }
    return r;
}

inline std::string Shell::renameTo(const std::string& src, const std::string& pat) {
    std::string o;
    size_t s = 0;
    for (size_t p = 0; p < pat.size(); ++p) {
        if (pat[p] == '?') { if (s < src.size() && src[s] != '.') o += src[s++]; }
        else if (pat[p] == '*') {
            char stop = p + 1 < pat.size() ? pat[p + 1] : 0;
            size_t e = stop ? src.find_last_of(stop) : std::string::npos;
            if (e == std::string::npos || e < s) e = src.size();
            o += src.substr(s, e - s);
            s = e;
        }
        else { o += pat[p]; if (s < src.size() && src[s] == pat[p]) ++s; else if (pat[p] == '.') { size_t d = src.find('.', s); s = d == std::string::npos ? src.size() : d + 1; } else if (s < src.size()) ++s; }
    }
    return o;
}

inline int Shell::cmdRen(const std::string& args, Ctx& c) {
    auto a = splitArgs(args);
    if (a.size() != 2) return syntaxError(c);
    std::string src = fullPath(unquote(a[0])), pat = unquote(a[1]);
    if (pat.find_first_of(os::PATH_STOPS) != std::string::npos) return syntaxError(c);
    std::string dir = dirOf(src);
    auto files = list(src);
    if (files.empty()) return fail(c, os::ERR_FILE_NOT_FOUND);
    int r = 0;
    for (auto& e : files) {
        std::string to = wild(pat) ? renameTo(e.name, pat) : pat;
        if (!os::renameFile(dir + e.name, dir + to)) {
            int err = os::lastError();
            c.err.line(err == os::ERR_ALREADY_EXISTS ? message(Msg::DuplicateOrNotFound) : systemMessage(err));
            r = 1;
        }
    }
    return r;
}

inline int Shell::cmdType(const std::string& args, Ctx& c) {
    auto a = splitArgs(args);
    if (a.empty()) return syntaxError(c);
    int r = 0;
    for (auto& p0 : a) {
        if (isSwitch(p0, "")) return syntaxError(c);
        if (isDevice(unquote(p0), "NUL")) continue;
        if (isDevice(unquote(p0), "CON")) { readUntilCtrlZ(c, [&](const std::string& part) { c.out.write(part); }); continue; }
        std::string p = fullPath(unquote(p0));
        std::vector<std::string> files;
        if (wild(p)) { std::string d = dirOf(p); for (auto& e : list(p)) if (!(e.attr & os::ATTR_DIRECTORY)) files.push_back(d + e.name); }
        else files.push_back(p);
        if (files.empty()) { r = fail(c, os::ERR_FILE_NOT_FOUND); continue; }
        for (auto& f : files) {
            if (wild(p) || a.size() > 1) c.err.write("\r\n" + afterSlash(f) + "\r\n\r\n\r\n");
            os::Mapped m;
            if (!os::mapFile(f, m)) { r = fail(c); continue; }
            if (!m.size) continue;
            if (m.size >= 2 && (unsigned char)m.data[0] == 0xFF && (unsigned char)m.data[1] == 0xFE)
                c.out.write(os::fromUtf16(reinterpret_cast<const char16_t*>(m.data + 2), (m.size - 2) / 2));
            else c.out.redirect(m.data, m.size, std::move(m.keep));
        }
    }
    return r;
}

inline int Shell::cmdMklink(const std::string& args, Ctx& c) {
    Opts o = opts(args, "D|H|J");
    if (o.pos.size() != 2) return syntaxError(c);
    std::string link = unquote(o.pos[0]), target = unquote(o.pos[1]);
    namespace fs = std::filesystem;
    std::error_code ec;
    Msg created = Msg::SymlinkCreated;
    if (o.has('H')) { fs::create_hard_link(fsPath(target), fsPath(link), ec); created = Msg::HardlinkCreated; }
    else if (o.has('J')) { if (!os::junction(link, target)) return fail(c); created = Msg::JunctionCreated; }
    else if (o.has('D')) fs::create_directory_symlink(fsPath(target), fsPath(link), ec);
    else fs::create_symlink(fsPath(target), fsPath(link), ec);
    if (ec) return fail(c, ec.value());
    c.out.line(message(created, { link, target }));
    return 0;
}

}
