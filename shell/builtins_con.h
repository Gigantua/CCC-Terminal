#pragma once
#include "shell.h"

namespace ccc {

inline int Shell::cmdMore(const std::string& args, Ctx& c) {
    if (!consoleToolsBuiltIn()) return external("more " + args, c);
    std::vector<std::string> files;
    int skip = 0;
    bool squeeze = false, clearFirst = false;
    for (auto& a : splitArgs(args)) {
        std::string u = upper(a);
        if (u[0] == '/' && !isSwitch(a, "E|S|C|P|T:0123456789")) { files.push_back(unquote(a)); continue; }
        if (u == "/E") continue;
        if (u == "/S") squeeze = true;
        else if (u == "/C") clearFirst = true;
        else if (u == "/P" || u.starts_with("/T")) continue;
        else if (a[0] == '+') skip = atoi(a.c_str() + 1);
        else files.push_back(unquote(a));
    }
    if (clearFirst) { OutputBuffer::flushAll(); host.clear(); }
    int rows = (std::max)(2, host.screenRows()), cols = (std::max)(1, host.screenColumns());
    int shown = 0, budget = rows - 1, lineNo = 0;
    bool previousBlank = false, quit = false;
    unsigned long long total = 0, consumed = 0;
    auto page = [&](const std::string& line) {
        if (quit) return;
        ++lineNo;
        if (lineNo <= skip) return;
        bool blank = line.empty();
        if (squeeze && blank && previousBlank) return;
        previousBlank = blank;
        int need = (std::max)(1, ((int)line.size() + cols - 1) / cols);
        if (shown + need > budget) {
            std::string prompt = total ? "-- More (" + std::to_string(consumed * 100 / total) + "%) -- " : "-- More -- ";
            c.out.write(prompt);
            OutputBuffer::flushAll();
            int key = host.readKey(os::WAIT_FOREVER);
            c.out.write("\r" + std::string(prompt.size(), ' ') + "\r");
            if (key == 'q' || key == 'Q') { quit = true; return; }
            budget = key == '\r' ? 1 : rows - 1;
            shown = 0;
        }
        c.out.line(line);
        shown += need;
    };
    auto pageStream = [&](os::Handle h) {
        std::string pending;
        std::vector<char> buf(1 << 16);
        size_t n;
        while (!quit && (n = os::read(h, buf.data(), buf.size())) != 0) {
            checkInterrupt();
            pending.append(buf.data(), n);
            size_t start = 0, nl;
            while (!quit && (nl = pending.find('\n', start)) != std::string::npos) {
                consumed += nl + 1 - start;
                std::string line = pending.substr(start, nl - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                page(line);
                start = nl + 1;
            }
            pending.erase(0, start);
        }
        if (!quit && !pending.empty()) page(pending);
    };
    if (files.empty()) {
        if (c.in.hasHandle()) pageStream(c.in.h);
        return 0;
    }
    for (auto& f : files) {
        os::Handle h = os::openRead(f);
        os::FileInfo info;
        if (h == os::NO_HANDLE || !os::fileInfo(f, info)) { os::close(h); c.err.line("Cannot access file " + fullPath(f)); return 1; }
        total = info.size;
        consumed = 0;
        pageStream(h);
        os::close(h);
        if (quit) break;
    }
    return 0;
}

inline int Shell::cmdTimeout(const std::string& args, Ctx& c) {
    if (!consoleToolsBuiltIn()) return external("timeout " + args, c);
    auto words = splitArgs(args);
    bool noBreak = false, haveValue = false;
    long long seconds = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string u = upper(words[i]);
        if (u == "/NOBREAK") { noBreak = true; continue; }
        if (u == "/T") { if (++i >= words.size()) break; u = words[i]; }
        char* end = nullptr;
        seconds = strtoll(u.c_str(), &end, 10);
        haveValue = end && *end == 0;
        if (!haveValue) break;
    }
    if (!haveValue || seconds < -1 || seconds > 99999) {
        c.err.line("ERROR: Invalid value for timeout (/T) specified. Valid range is -1 to 99999.");
        return 1;
    }
    if (seconds == -1) {
        c.out.write("\r\nPress any key to continue ...");
        OutputBuffer::flushAll();
        if (noBreak) for (;;) { checkInterrupt(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        host.readKey(os::WAIT_FOREVER);
        c.out.write("\r\n");
        return 0;
    }
    std::string tail = noBreak ? " seconds, press CTRL+C to quit ..." : " seconds, press a key to continue ...";
    int width = (int)std::to_string(seconds).size();
    auto show = [&](long long left) {
        std::string n = std::to_string(left);
        c.out.write("\rWaiting for " + std::string(width - n.size(), ' ') + n + tail);
        OutputBuffer::flushAll();
    };
    c.out.write("\r\n");
    for (long long left = seconds; left > 0; --left) {
        show(left);
        if (noBreak) {
            for (int k = 0; k < 20; ++k) { checkInterrupt(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
            continue;
        }
        if (host.readKey(1000) >= 0) { show(left); c.out.write("\r\n"); return 0; }
    }
    show(0);
    c.out.write("\r\n");
    return 0;
}

inline int Shell::cmdChoice(const std::string& args, Ctx& c) {
    if (!consoleToolsBuiltIn()) return external("choice " + args, c);
    std::string choices = "YN", message, defaultChoice;
    bool hide = false, caseSensitive = false;
    long long timeout = -1;
    auto words = splitArgs(args);
    auto value = [&](size_t& i, const std::string& w) {
        if (w.size() > 3 && w[2] == ':') return unquote(w.substr(3));
        return ++i < words.size() ? unquote(words[i]) : std::string();
    };
    for (size_t i = 0; i < words.size(); ++i) {
        std::string w = words[i], u = upper(w.substr(0, 2));
        if (u == "/C") choices = value(i, w);
        else if (u == "/N") hide = true;
        else if (upper(w) == "/CS") caseSensitive = true;
        else if (u == "/T") timeout = atoll(value(i, w).c_str());
        else if (u == "/D") defaultChoice = value(i, w);
        else if (u == "/M") message = value(i, w);
        else { c.err.line("ERROR: Invalid argument/option - '" + w + "'."); return 255; }
    }
    if (choices.empty()) { c.err.line("ERROR: Invalid syntax. Choices cannot be empty."); return 255; }
    if ((timeout >= 0) != !defaultChoice.empty()) { c.err.line("ERROR: Invalid syntax. Default choice must be specified along with timeout."); return 255; }
    std::string list = caseSensitive ? choices : upper(choices);
    auto indexOf = [&](int key) -> int {
        if (key <= 0) return 0;
        char k = caseSensitive ? (char)key : (char)toupper(key);
        size_t at = list.find(k);
        return at == std::string::npos ? 0 : (int)at + 1;
    };
    if (!defaultChoice.empty() && !indexOf((unsigned char)defaultChoice[0])) {
        c.err.line("ERROR: Invalid syntax. Default choice must be one of the given choices.");
        return 255;
    }
    std::string prompt = message.empty() ? "" : message + " ";
    if (!hide) {
        prompt += "[";
        for (size_t k = 0; k < choices.size(); ++k) { if (k) prompt += ","; prompt += choices[k]; }
        prompt += "]?";
    }
    c.out.write(prompt);
    OutputBuffer::flushAll();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout >= 0 ? timeout : 0);
    for (;;) {
        unsigned long wait = os::WAIT_FOREVER;
        if (timeout >= 0) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            wait = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        }
        int key = host.readKey(wait);
        if (key < 0 && timeout >= 0) break;
        if (int index = indexOf(key)) {
            c.out.line(std::string(1, choices[index - 1]));
            return index;
        }
        os::beep();
    }
    int index = indexOf((unsigned char)defaultChoice[0]);
    c.out.line(std::string(1, choices[index - 1]));
    return index;
}

inline int Shell::cmdChcp(const std::string& args, Ctx& c) {
    if (!consoleToolsBuiltIn()) return external("chcp " + args, c);
    std::string a = trim(args);
    if (!a.empty()) {
        char* end = nullptr;
        unsigned long page = strtoul(a.c_str(), &end, 10);
        if (!end || *end || !os::codePageValid((unsigned)page)) { c.err.line("Invalid code page"); return 1; }
        unicode::setByteCodePage((unsigned)page);
    }
    c.out.line("Active code page: " + std::to_string(unicode::byteCodePage()));
    return 0;
}

inline int Shell::cmdMode(const std::string& args, Ctx& c) {
    if (!consoleToolsBuiltIn()) return external("mode " + args, c);
    std::vector<std::string> words;
    for (size_t i = 0; i < args.size();) {
        while (i < args.size() && (args[i] == ' ' || args[i] == '\t')) ++i;
        size_t e = i;
        while (e < args.size() && args[e] != ' ' && args[e] != '\t') ++e;
        if (e > i) words.push_back(args.substr(i, e - i));
        i = e;
    }
    if (!words.empty() && (ieq(words[0], "CON") || ieq(words[0], "CON:"))) words.erase(words.begin());
    int cols = host.screenColumns(), rows = host.screenRows();
    bool change = false;
    for (auto& w : words) {
        std::string u = upper(w);
        size_t comma = u.find(',');
        if (u.starts_with("COLS=")) { cols = atoi(u.c_str() + 5); change = true; }
        else if (u.starts_with("LINES=")) { rows = atoi(u.c_str() + 6); change = true; }
        else if (comma != std::string::npos && isdigit((unsigned char)u[0])) { cols = atoi(u.c_str()); rows = atoi(u.c_str() + comma + 1); change = true; }
        else if (isdigit((unsigned char)u[0])) { cols = atoi(u.c_str()); change = true; }
        else { c.err.line("Invalid parameter - " + w); return 1; }
    }
    if (change) {
        if (cols < 1 || rows < 1 || !host.resizeScreen(cols, rows)) { c.err.line("The screen cannot be set to the number of lines and columns specified."); return 1; }
        return 0;
    }
    c.out.write("\r\nStatus for device CON:\r\n----------------------\r\n");
    c.out.line("    Lines:          " + std::to_string(rows));
    c.out.line("    Columns:        " + std::to_string(cols));
    c.out.line("    Keyboard rate:  31");
    c.out.line("    Keyboard delay: 1");
    c.out.line("    Code page:      " + std::to_string(unicode::byteCodePage()));
    c.out.write("\r\n");
    return 0;
}

inline int Shell::cmdDoskey(const std::string& args, Ctx& c) {
    std::string a = trim(args);
    std::string u = upper(a);
    if (u.empty()) return 0;
    if (u.starts_with("/MACROS")) {
        for (auto& [key, macro] : macros) c.out.line(macro.first + "=" + macro.second);
        return 0;
    }
    if (u == "/HISTORY" || u == "/H") {
        for (auto& line : host.history()) c.out.line(line);
        return 0;
    }
    if (u == "/REINSTALL") { macros.clear(); host.clearHistory(); return 0; }
    if (u == "/INSERT") { host.setInsertMode(true); return 0; }
    if (u == "/OVERSTRIKE") { host.setInsertMode(false); return 0; }
    if (u.starts_with("/LISTSIZE=") || u.starts_with("/EXENAME=")) return 0;
    if (u.starts_with("/MACROFILE=")) {
        std::string path = unquote(a.substr(11));
        std::string text;
        if (!readFile(path, text)) return fail(c);
        size_t pos = 0;
        while (pos < text.size()) {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            std::string line = trim(text.substr(pos, e - pos));
            if (!line.empty() && line.back() == '\r') line.pop_back();
            defineMacro(line);
            pos = e + 1;
        }
        return 0;
    }
    if (a[0] == '/') { c.err.line("Invalid switch - " + a); return 1; }
    if (!defineMacro(a)) return syntaxError(c);
    return 0;
}

inline bool Shell::defineMacro(const std::string& definition) {
    size_t eq = definition.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    std::string name = trim(definition.substr(0, eq)), text = definition.substr(eq + 1);
    if (name.empty() || name.find_first_of(" \t") != std::string::npos) return false;
    if (text.empty()) macros.erase(upper(name));
    else macros[upper(name)] = { name, text };
    return true;
}

inline std::vector<std::string> Shell::expandMacro(const std::string& line) {
    size_t a = 0;
    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
    size_t b = a;
    while (b < line.size() && line[b] != ' ' && line[b] != '\t') ++b;
    auto it = macros.find(upper(line.substr(a, b - a)));
    if (it == macros.end()) return { line };
    std::string rest = trim(line.substr(b));
    auto params = splitArgs(rest);
    std::vector<std::string> out(1);
    const std::string& text = it->second.second;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '$' || i + 1 >= text.size()) { out.back() += text[i]; continue; }
        char k = (char)toupper((unsigned char)text[++i]);
        if (k >= '1' && k <= '9') { size_t n = k - '1'; if (n < params.size()) out.back() += params[n]; }
        else if (k == '*') out.back() += rest;
        else if (k == 'T') out.emplace_back();
        else if (k == 'G') out.back() += '>';
        else if (k == 'L') out.back() += '<';
        else if (k == 'B') out.back() += '|';
        else if (k == '$') out.back() += '$';
        else { out.back() += '$'; out.back() += text[i]; }
    }
    return out;
}

}
