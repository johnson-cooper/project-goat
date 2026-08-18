#include "ProcessBridge.hpp"

#include <array>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace goat::process {

long current_pid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

#ifdef _WIN32

namespace {

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

// Quotes one argument for CreateProcessW's single command-line string per
// the same rules CommandLineToArgvW expects (doubling backslashes that
// immediately precede a literal quote, or that end the argument right
// before the closing quote) — the standard algorithm for this, since naive
// wrap-in-quotes breaks on paths ending in a backslash or containing one.
std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result += ch;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

std::wstring build_command_line(const std::string& exe, const std::vector<std::string>& args) {
    std::wstring command = quote_arg(utf8_to_wide(exe));
    for (const auto& arg : args) { command += L' '; command += quote_arg(utf8_to_wide(arg)); }
    return command;
}

// Opens `log_path` for the child's stdout+stderr, if possible. Failure is
// non-fatal to the caller — the process still spawns, just without
// redirection, matching the Win32 original's own silent-degrade behavior.
HANDLE open_log_handle(const std::string& log_path) {
    if (log_path.empty()) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    return CreateFileW(utf8_to_wide(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

Process spawn(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::string& log_path) {
    std::wstring command = build_command_line(exe, args);
    const HANDLE logHandle = open_log_handle(log_path);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (logHandle != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = logHandle;
        startup.hStdError = logHandle;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, logHandle != INVALID_HANDLE_VALUE,
                                         CREATE_NO_WINDOW, nullptr, utf8_to_wide(cwd).c_str(), &startup, &info);
    if (logHandle != INVALID_HANDLE_VALUE) CloseHandle(logHandle);
    if (!created) return {};

    CloseHandle(info.hThread); // never needed past spawn
    Process p;
    p.native_handle = info.hProcess;
    p.pid = static_cast<long>(info.dwProcessId);
    return p;
}

Status poll(const Process& p, int& exit_code) {
    if (!p.valid() || !p.native_handle) return Status::Failed;
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(p.native_handle), &code)) return Status::Failed;
    if (code == STILL_ACTIVE) return Status::Running;
    exit_code = static_cast<int>(code);
    return Status::Exited;
}

void terminate(Process& p) {
    if (p.valid() && p.native_handle) TerminateProcess(static_cast<HANDLE>(p.native_handle), 1);
    close_handles(p);
}

void close_handles(Process& p) {
    if (p.native_handle) CloseHandle(static_cast<HANDLE>(p.native_handle));
    p.native_handle = nullptr;
    p.pid = -1;
}

CapturedRun run_and_capture(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    CapturedRun result;

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inheritable, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0); // parent's read end must not be inherited

    std::wstring command = build_command_line(exe, args);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                         utf8_to_wide(cwd).c_str(), &startup, &info);
    CloseHandle(writePipe);
    if (!created) { CloseHandle(readPipe); return result; }
    result.started = true;

    std::array<char, 512> chunk{};
    DWORD received = 0;
    while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &received, nullptr) && received) {
        result.output.append(chunk.data(), received);
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    result.exit_code = static_cast<int>(code);

    CloseHandle(readPipe);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return result;
}

#else // POSIX

namespace {

std::vector<char*> to_argv(const std::string& exe, const std::vector<std::string>& args, std::vector<std::string>& storage) {
    storage.clear();
    storage.push_back(exe);
    for (const auto& a : args) storage.push_back(a);
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    return argv;
}

} // namespace

// fork()+chdir()+execve() rather than posix_spawn() with
// posix_spawn_file_actions_addchdir_np(): that function's `_np` suffix means
// exactly what it says ("non-portable") — it's a glibc/BSD extension outside
// base POSIX, and this project's CI builds run on real macOS/Linux runners
// this developer's own Windows-only sandbox can never directly verify. fork
// avoids the question entirely: chdir/dup2/open/close/execve/_exit are all
// plain, universally-available, async-signal-safe POSIX calls, so this is
// safe to use in the child between fork() and exec().
Process spawn(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::string& log_path) {
    std::vector<std::string> storage;
    std::vector<char*> argv = to_argv(exe, args, storage);

    const pid_t pid = fork();
    if (pid < 0) return {};
    if (pid == 0) {
        // Child. Only async-signal-safe calls until execve() replaces this
        // process image entirely.
        if (chdir(cwd.c_str()) != 0) _exit(127);
        if (!log_path.empty()) {
            // Failure to open the log is non-fatal (matches the Windows
            // side's own degrade-without-redirection behavior when
            // CreateFileW fails) — just skip the redirection and continue.
            const int fd = open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO) close(fd);
            }
        }
        execve(exe.c_str(), argv.data(), environ);
        _exit(127); // execve() only returns on failure
    }

    Process p;
    p.pid = static_cast<long>(pid);
    return p;
}

Status poll(const Process& p, int& exit_code) {
    if (!p.valid()) return Status::Failed;
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(p.pid), &status, WNOHANG);
    if (result == 0) return Status::Running;
    if (result < 0) return Status::Failed;
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return Status::Exited;
}

void terminate(Process& p) {
    if (p.valid()) {
        kill(static_cast<pid_t>(p.pid), SIGKILL);
        int status = 0;
        waitpid(static_cast<pid_t>(p.pid), &status, 0);
    }
    close_handles(p);
}

void close_handles(Process& p) {
    // Nothing to release on POSIX — poll()/terminate() already reap via
    // waitpid, and there's no separate OS handle object like Windows' HANDLE.
    p.pid = -1;
}

CapturedRun run_and_capture(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    CapturedRun result;

    int pipefd[2];
    if (pipe(pipefd) != 0) return result;

    std::vector<std::string> storage;
    std::vector<char*> argv = to_argv(exe, args, storage);

    const pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return result; }
    if (pid == 0) {
        // Child: same async-signal-safe-only constraint as spawn() above.
        if (chdir(cwd.c_str()) != 0) _exit(127);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execve(exe.c_str(), argv.data(), environ);
        _exit(127);
    }

    close(pipefd[1]); // parent must close its copy of the write end or the read loop below blocks forever
    result.started = true;

    std::array<char, 512> chunk{};
    ssize_t received = 0;
    while ((received = read(pipefd[0], chunk.data(), chunk.size())) > 0) {
        result.output.append(chunk.data(), static_cast<size_t>(received));
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

#endif

} // namespace goat::process
