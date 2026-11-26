#pragma once
#include "shell.h"
#ifdef _WIN32
#include "server.h"
#else
#include "pty.h"
#endif

namespace ccc {

#ifdef _WIN32
inline bool launchInConsole(ConsoleServer& server, const std::string& exe, const std::string& cmdline, os::Process& process, int& error) {
    std::wstring wcmd = os::wide(cmdline);
    PROCESS_INFORMATION pi = {};
    DWORD err = 0;
    if (!server.launch(os::wide(exe), wcmd, pi, err)) { error = (int)err; return false; }
    CloseHandle(pi.hThread);
    process.handle = pi.hProcess;
    return true;
}
#else
inline bool launchInConsole(ConsoleServer& server, const std::string& exe, const std::string& cmdline, os::Process& process, int& error) {
    return server.launch(exe, cmdline, process, error);
}
#endif

}

#include "expand.h"
#include "exec.h"
#include "external.h"
#include "builtins_fs.h"
#include "builtins_env.h"
#include "builtins_flow.h"
#include "builtins_sys.h"
#include "builtins_con.h"
#include "batch.h"
