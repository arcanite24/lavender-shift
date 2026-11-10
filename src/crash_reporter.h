#pragma once

// The process crash reporter behind the diagnostics interface:
// Windows writes a minidump and a symbolized text report, POSIX hosts a text
// report with the signal, faulting address, registers and backtrace. Reports
// go to <crash_root>/<session>-*.

#include <filesystem>
#include <string>

namespace lavender_shift::diagnostics::crash {

// Installs the handlers; later crashes in any thread are reported.
void Install(const std::filesystem::path& crash_root, const std::string& session_id);

// Installs the unhandled-crash handler again, for runtime components that
// replace it after startup.
void Refresh();

// Crash self-tests (LAVENDER_SHIFT_CRASH_SELF_TEST and
// LAVENDER_SHIFT_EXECUTE_CRASH_SELF_TEST): an access violation, and a call
// through a null function pointer. Neither returns when the reporter works.
void RaiseAccessViolation();
void ExecuteNull();

}  // namespace lavender_shift::diagnostics::crash
