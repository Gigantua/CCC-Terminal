#pragma once
#include "shell.h"

namespace ccc {

inline bool Shell::runnable(const std::string& path) { return isBatchExtension(upperExtension(path)) || os::isExecutable(path); }

inline std::string Shell::resolve(const std::string& name0) {
    std::string name = unquote(name0);
    std::string pathext;
    if (!getVar("PATHEXT", pathext)) pathext = os::DEFAULT_PATHEXT;
    auto exts = splitList(pathext);
    size_t slash = name.find_last_of(os::SLASHES);
    bool hasExt = name.find('.', slash == std::string::npos ? 0 : slash) != std::string::npos;
    bool bare = hasExt || os::EXTENSIONLESS_PROGRAMS;
    auto tryBase = [&](const std::string& base) -> std::string {
        if (bare && isFile(base) && runnable(base)) return fullPath(base);
        for (auto& e : exts) if (isFile(base + e) && runnable(base + e)) return fullPath(base + e);
        return "";
    };
    if (name.find_first_of(os::PATH_STOPS) != std::string::npos) return tryBase(name);
    std::string r, skipCwd;
    if (!getVar("NoDefaultCurrentDirectoryInExePath", skipCwd)) { r = tryBase(name); if (!r.empty()) return r; }
    std::string path;
    getVar("PATH", path);
    auto dirs = splitList(path, os::PATH_LIST_SEPARATOR);
    auto join = [&](size_t k, const std::string& ext) { return withSlash(dirs[k]) + name + ext; };
    size_t k;
    std::string ext;
    if (!exeDirs.find(dirs, name, bare, exts, [&](size_t i, const std::string& e) { return runnable(join(i, e)); }, k, ext)) return "";
    return fullPath(join(k, ext));
}

inline int Shell::external(const std::string& text, Ctx& c) {
    size_t e = tokenEnd(text);
    std::string prog = text.substr(0, e), args = text.substr(e);
    std::string path = resolve(prog);
    if (path.empty()) return notRun(c, message(Msg::NotRecognized, { unquote(prog) }));
    std::string ext = upperExtension(path);
    if (isBatchExtension(ext)) {
        auto a = splitArgs(args);
        a.insert(a.begin(), prog);
        int rc = runBatchFile(path, a, argTail(args), &c);
        return batch() ? exitBatch(rc) : rc;
    }
    if (os::opensAsDocument(ext)) {
        prepareChild();
        if (!os::openDocument(path, trim(args), "", os::Show::Normal, false)) { c.err.line(systemMessage(os::lastError())); errorlevel = 1; return 1; }
        errorlevel = 0;
        return 0;
    }
    return spawn(path, "\"" + path + "\"" + args, c, !os::isGuiProgram(path) || batch() != nullptr);
}

inline os::EnvList Shell::colorEnvironment() const {
    std::vector<std::pair<std::string, std::string>> defaults = { { "FORCE_COLOR", "1" }, { "CLICOLOR_FORCE", "1" } };
    if (!env.find("GIT_CONFIG_COUNT"))
        defaults.insert(defaults.end(), { { "GIT_CONFIG_COUNT", "1" }, { "GIT_CONFIG_KEY_0", "color.ui" }, { "GIT_CONFIG_VALUE_0", "always" } });
    return env.listWith(defaults);
}

inline int Shell::waitChild(os::Process& process) {
    { std::lock_guard<std::mutex> lock(childMutex); children.push_back(process); }
    if (interrupted) os::interruptProcess(process);
    int code = os::waitProcess(process);
    { std::lock_guard<std::mutex> lock(childMutex); std::erase(children, process); }
    return code;
}

inline int Shell::childDone(int code) {
    errorlevel = lastExitCode = code;
    checkInterrupt();
    return code;
}

inline int Shell::notRun(Ctx& c, const std::string& text) {
    c.err.line(text);
    errorlevel = 9009;
    return 9009;
}

inline bool Shell::attachable(const Ctx& c) {
    auto console = [](const Sink& s) { return s.con && !s.buf && !s.discard && !s.hasHandle(); };
    return console(c.out) && console(c.err) && !c.in.hasHandle();
}

inline int Shell::spawnAttached(ConsoleServer& server, const std::string& exe, const std::string& cmdline, Ctx& c) {
    prepareChild();
    server.begin(host.screenColumns(), host.screenRows());
    os::Process process;
    int err = 0;
    if (!launchInConsole(server, exe, cmdline, process, err)) {
        server.end();
        return notRun(c, systemMessage(err));
    }
    int code = waitChild(process);
    server.end();
    os::closeProcess(process);
    return childDone(code);
}

inline int Shell::spawn(const std::string& exe, const std::string& cmdline, Ctx& c, bool wait) {
    if (ConsoleServer* server = host.server(); server && wait && attachable(c) && !os::isGuiProgram(exe)) return spawnAttached(*server, exe, cmdline, c);
    os::Handle outRead = os::NO_HANDLE, outWrite = os::NO_HANDLE, inRead = os::NO_HANDLE, inWrite = os::NO_HANDLE;
    Handles nulls;
    for (Sink* s : { &c.out, &c.err })
        if (s->discard) { s->h = os::openNull(); nulls.h.push_back(s->h); }
    bool pumpOut = !c.out.hasHandle(), pumpErr = !c.err.hasHandle();
    if (pumpOut || pumpErr) os::createReadPipe(outRead, outWrite);
    os::Spawn s;
    s.exe = exe;
    s.cmdline = cmdline;
    s.out = pumpOut ? outWrite : c.out.h;
    s.err = pumpErr ? outWrite : c.err.h;
    if (c.in.hasHandle()) s.in = c.in.h;
    else if (host.console()) { os::createPipe(inRead, inWrite, 0); s.in = inRead; }
    else s.in = os::stdHandle(0);
    prepareChild();
    os::EnvList colorEnv;
    if (pumpOut && host.console()) { colorEnv = colorEnvironment(); s.env = &colorEnv; }
    s.hidden = host.console() != nullptr;
    os::Process process;
    int err = 0;
    bool ok = os::spawn(s, process, err);
    os::close(outWrite);
    os::close(inRead);
    if (!ok) {
        os::close(outRead);
        os::close(inWrite);
        return notRun(c, systemMessage(err));
    }
    std::thread pump;
    bool pumpConsole = false;
    if (outRead != os::NO_HANDLE) {
        Sink sink = pumpOut ? c.out : c.err;
        pumpConsole = sink.con && !sink.buf && !sink.discard;
        const os::Process* writer = pumpConsole && wait ? &process : nullptr;
        pump = std::thread([outRead, sink, pumpConsole, writer] {
            if (pumpConsole) pumpToConsole(outRead, *sink.con, writer);
            else pumpToSink(outRead, sink);
            os::close(outRead);
        });
    }
    if (inWrite != os::NO_HANDLE) host.childInput(inWrite);
    int code = wait ? waitChild(process) : 0;
    if (inWrite != os::NO_HANDLE) { host.childInput(os::NO_HANDLE); os::close(inWrite); }
    if (pump.joinable()) { if (wait) pump.join(); else pump.detach(); }
    os::closeProcess(process);
    return childDone(code);
}

}
