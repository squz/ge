// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T136 Crash diagnostics. Two halves:
//   * guardCallback — wraps a consumer callback, logs an uncaught exception
//     through the app-channel logger, and RE-THROWS so the OS crash report is
//     unchanged (here we only assert the re-throw + the opt-out gate; the log
//     content lands on stderr in the test runner).
//   * installCrashHandlers — a fatal-signal last-gasp. Verified with a fork
//     death-test: the child installs the handlers and raises SIGABRT; we assert
//     it both reported the signal to stderr AND still died of SIGABRT (the
//     handler re-raised the default, preserving the OS crash report).

#include <ge/Signal.h>

#include <doctest.h>

#include <stdexcept>
#include <string>

TEST_CASE("🎯T136 guardCallback runs fn and returns when it doesn't throw") {
    ge::setCrashDiagnosticsEnabled(true);
    int n = 0;
    ge::guardCallback("noThrow", [&] { n = 42; });
    CHECK(n == 42);
}

TEST_CASE("🎯T136 guardCallback re-throws the original exception after reporting") {
    ge::setCrashDiagnosticsEnabled(true);
    bool caught = false;
    try {
        ge::guardCallback("throws", [] { throw std::runtime_error("boom"); });
    } catch (const std::runtime_error& e) {
        caught = true;
        CHECK(std::string(e.what()) == "boom");  // propagated unchanged
    }
    CHECK(caught);
}

TEST_CASE("🎯T136 opt-out: disabled diagnostics still run fn and propagate raw") {
    ge::setCrashDiagnosticsEnabled(false);
    bool ran = false, caught = false;
    try {
        ge::guardCallback("disabled", [&] { ran = true; throw std::runtime_error("x"); });
    } catch (...) {
        caught = true;
    }
    CHECK(ran);
    CHECK(caught);
    ge::setCrashDiagnosticsEnabled(true);  // restore the default for later tests
}

TEST_CASE("🎯T136 crashDiagnosticsEnabled round-trips") {
    ge::setCrashDiagnosticsEnabled(false);
    CHECK_FALSE(ge::crashDiagnosticsEnabled());
    ge::setCrashDiagnosticsEnabled(true);
    CHECK(ge::crashDiagnosticsEnabled());
}

#if !defined(_WIN32)

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

// Fork a child that installs the crash handlers and aborts; assert it reported
// the signal to stderr and that the default handler still fired (the process
// died of SIGABRT, so the OS crash report is preserved).
TEST_CASE("🎯T136 installCrashHandlers reports the signal then re-raises the default") {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    pid_t pid = fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child: redirect stderr into the pipe, install the handlers, crash.
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        ge::setCrashDiagnosticsEnabled(true);
        ge::installCrashHandlers();
        std::raise(SIGABRT);
        _exit(0);  // unreachable: the handler re-raises SIGABRT
    }

    // Parent: drain the child's stderr, then reap it.
    close(pipefd[1]);
    std::string out;
    char buf[512];
    ssize_t r;
    while ((r = read(pipefd[0], buf, sizeof buf)) > 0) {
        out.append(buf, static_cast<size_t>(r));
    }
    close(pipefd[0]);

    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);

    CHECK(WIFSIGNALED(status));                 // died of a signal, not exit()
    CHECK(WTERMSIG(status) == SIGABRT);         // the default handler re-fired
    CHECK(out.find("fatal signal SIGABRT") != std::string::npos);  // and it reported first
}

#endif  // !_WIN32
