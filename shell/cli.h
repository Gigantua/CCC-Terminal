#pragma once
#include "ccc.h"
#include <algorithm>

namespace ccc {

inline Shell* g_shell = nullptr;

struct StdHost : Host {
    char buf[1 << 16];
    size_t have = 0, pos = 0;

    bool readLine(std::string& line) override {
        line.clear();
        for (;;) {
            if (pos == have) {
                pos = have = 0;
                os::ReadResult r = os::readStdin(buf, sizeof(buf), have);
                if (r != os::ReadResult::Data) {
                    if (r == os::ReadResult::Cancelled || (g_shell && g_shell->interrupted)) { have = 0; std::this_thread::sleep_for(std::chrono::milliseconds(1)); return true; }
                    return !line.empty();
                }
            }
            char c = buf[pos++];
            if (c == '\n') return true;
            if (c != '\r') line += c;
        }
    }
    void waitKey() override { os::readKey(os::WAIT_FOREVER); }
    int readKey(unsigned long timeoutMs) override {
        int key = os::readKey(timeoutMs);
        if (key == os::KEY_CANCELLED) throw Interrupt();
        return key;
    }
    void cancelInput() override { os::cancelStdinRead(); }
    void setTitle(const std::string& t) override { os::setConsoleTitle(t); }
    std::string windowTitle() override { return os::consoleTitle(); }
    void setColor(int attr) override { os::setConsoleColor(attr); }
    void clear() override { os::clearConsole(); }
    Console* console() override { return nullptr; }
    void childInput(os::Handle) override {}
    bool resizeScreen(int cols, int rows) override { return os::resizeConsole(cols, rows); }
    int screenRows() override {
        int cols, rows;
        return os::consoleSize(cols, rows) ? rows : 25;
    }
    int screenColumns() override {
        int cols, rows;
        return os::consoleSize(cols, rows) ? cols : 80;
    }
};

inline int cliMain() {
    os::handleSignals([] { if (g_shell) g_shell->interrupt(); });
    StdHost host;
    Shell sh(host);
    g_shell = &sh;
    std::string cl = os::commandLine();
    sh.cmdline = cl;
    size_t i = tokenEnd(cl);
    std::string command;
    bool runAndExit = false, runAndStay = false, noAutoRun = false;
    int color = -1;
    sh.applyRegistryDefaults();
    while (i < cl.size()) {
        while (i < cl.size() && (cl[i] == ' ' || cl[i] == '\t')) ++i;
        if (i + 1 >= cl.size() || cl[i] != '/') break;
        char s = (char)toupper((unsigned char)cl[i + 1]);
        std::string rest = upper(cl.substr(i + 1, 6));
        if (s == 'C' || s == 'K') {
            (s == 'C' ? runAndExit : runAndStay) = true;
            command = cl.substr(i + 2);
            break;
        }
        if (s == '?') {
            sh.stdoutSink().write(crlf(helpTexts().at("CMD")));
            return 0;
        }
        if (s == 'Q') sh.echoOn = false;
        if (s == 'D') noAutoRun = true;
        if (s == 'U') sh.setUnicodeOutput(true);
        if (s == 'A') sh.setUnicodeOutput(false);
        if (rest.starts_with("T:")) color = (int)strtol(rest.substr(2).c_str(), nullptr, 16);
        if (rest.starts_with("V:ON")) sh.delayed = true;
        if (rest.starts_with("V:OFF")) sh.delayed = false;
        if (rest.starts_with("E:OFF")) sh.extensions = false;
        while (i < cl.size() && cl[i] != ' ' && cl[i] != '\t') ++i;
    }
    command = trim(command);
    if (!command.empty() && command.front() == '"') {
        size_t last = command.rfind('"');
        std::string inner = command.substr(1, last - 1);
        bool keep = std::count(command.begin(), command.end(), '"') == 2 && inner.find_first_of("&<>()@^|") == std::string::npos &&
                    inner.find_first_of(" \t") != std::string::npos && exists(inner);
        if (!keep) command = command.substr(1, last - 1) + command.substr(last + 1);
    }
    int rc = 0;
    try {
        if (!noAutoRun) sh.runAutoRun();
        if (color >= 0 && (color >> 4 & 15) != (color & 15)) host.setColor(color);
        else if (!runAndExit) sh.applyDefaultColor();
        if (runAndExit) return sh.runLine(command, nullptr);
        if (!runAndStay) sh.stdoutSink().write(Shell::banner());
        if (runAndStay) sh.runLine(command, nullptr);
        rc = sh.run();
    }
    catch (ExitShell& e) { rc = e.code; }
    catch (Interrupt&) { rc = os::INTERRUPTED_EXIT_CODE; }
    return rc;
}

}
