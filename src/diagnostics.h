#pragma once

#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lavender_shift::diagnostics {

// Pinyon Shift's diagnostics (docs/PINYON_PARITY.md LP-1.10). Initialize
// starts the session: its ID (UTC time and process ID), the crash reporter
// writing to <user>/crashes, and the event file <user>/logs/<session>.jsonl.
void Initialize(const std::filesystem::path& user_root);

// Installs the unhandled-crash handler again, for runtime components that
// replace it after startup.
void RefreshCrashReporter();

const std::string& SessionId();
// <user>/logs, or the working folder before Initialize.
std::filesystem::path LogsDir();

// A named event with string fields: one JSON object in the session's event
// file and one line in the log ("event {...}"). The host UI and the settings
// menu record what was opened and changed, so a log shows what happened just
// before a problem.
using Field = std::pair<std::string_view, std::string>;
void RecordEvent(std::string_view event, std::initializer_list<Field> fields = {});
// The same with fields built at run time.
void RecordEvent(std::string_view event, std::span<const Field> fields);

}  // namespace lavender_shift::diagnostics
