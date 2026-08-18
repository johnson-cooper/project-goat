#pragma once

// A tiny cross-platform process-spawning surface for launching the goat-sim
// engine as a subprocess from goat-client-rl. Deliberately its own
// translation unit, separate from main.cpp: the Windows implementation needs
// <windows.h> (CreateProcessW, pipes, handles), and that header collides
// with raylib.h in the same TU (both declare e.g. CloseWindow, Rectangle,
// LoadImage with incompatible signatures — see main.cpp's own comment on
// this for the GetModuleFileNameW case, which sidesteps it by forward-
// declaring one function; that trick doesn't scale to an entire process API,
// so here we just keep them in separate files instead).
//
// Mirrors src/client/main.cpp's CreateProcessW-based start_player_duel/
// run_automatic_duel, generalized behind one interface with a POSIX
// (posix_spawn/fork+exec) implementation alongside the Windows one, per
// docs/CROSS_PLATFORM_CLIENT.md's cross-platform engine IPC phase.

#include <string>
#include <vector>

namespace goat::process {

// Opaque handle to a spawned, still-possibly-running process. Default-
// constructed as invalid (pid == -1) so it's safe to hold in AppState and
// test for "no duel currently running" the same way the Win32 client tests
// PROCESS_INFORMATION::hProcess for truthiness.
struct Process {
    void* native_handle = nullptr; // Windows: HANDLE to the process; unused on POSIX
    long pid = -1;                 // POSIX: process id; Windows: process id too (informational)
    bool valid() const { return pid >= 0; }
};

// Spawns `exe` with `args` (argv[1..], not including the program name)
// running in `cwd`, with stdout+stderr redirected to `log_path` on a
// best-effort basis — if the log file can't be opened, the process still
// spawns, just without redirection (matches the Win32 original's own
// silent-degrade behavior when CreateFileW fails). Returns an invalid
// Process (pid == -1) if the spawn itself fails (e.g. `exe` doesn't exist).
Process spawn(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, const std::string& log_path);

// This process's own id (GetCurrentProcessId on Windows, getpid on POSIX) —
// used to give each running client instance its own unique duel session
// directory, so a stray leftover instance (observed more than once in this
// project's own development) can never write into the same request.txt/
// response.txt/state.txt as the instance actually being used.
long current_pid();

enum class Status { Running, Exited, Failed };

// Non-blocking check of whether `p` is still running. When not Running,
// `exit_code` is set to the process's exit code (Exited) or is unspecified
// (Failed, e.g. `p` was never valid).
Status poll(const Process& p, int& exit_code);

// Hard-kills a still-running process and reaps it (Windows: TerminateProcess;
// POSIX: SIGKILL + waitpid) — used only for "the app is shutting down with a
// duel still active," matching the Win32 client's WM_DESTROY handler. Not a
// graceful shutdown request.
void terminate(Process& p);

// Releases any OS resources still held for a process that has already
// exited on its own (observed via poll() returning Exited) — Windows:
// closes the handle; POSIX: a no-op, since poll() already reaped it via
// waitpid. Always safe to call even if resources are already released.
void close_handles(Process& p);

// Blocking spawn-and-capture, used by the Hub's "automatic duel" smoke-test
// button: runs `exe` with `args` in `cwd`, blocks the calling thread until
// it exits (this freezes the UI for the duration, same as the Win32
// original's WaitForSingleObject(..., INFINITE) — not fixed here, since
// that's an accepted characteristic of that one button, not a bug), and
// returns its combined stdout+stderr output plus exit code.
struct CapturedRun {
    std::string output;
    int exit_code = 1;
    bool started = false;
};
CapturedRun run_and_capture(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd);

} // namespace goat::process
