// Session events and the crash reporter, after Pinyon Shift's
// src/pinyon_shift_diagnostics.cpp without its build manifest and CPU checks
// (Lavender builds only on the PC that runs it).

#include "diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "crash_reporter.h"

namespace lavender_shift::diagnostics {
namespace {

std::filesystem::path g_logs;
std::filesystem::path g_event_path;
std::string g_session_id;
std::mutex g_event_mutex;

std::string UtcTimestamp(bool filename_safe) {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_s(&utc, &time);
  std::ostringstream stream;
  stream << std::put_time(&utc, filename_safe ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

std::string JsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  constexpr char hex[] = "0123456789ABCDEF";
  for (const unsigned char c : value) {
    switch (c) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (c < 0x20) {
          escaped += "\\u00";
          escaped += hex[c >> 4];
          escaped += hex[c & 0x0F];
        } else {
          escaped += char(c);
        }
    }
  }
  return escaped;
}

bool EnvironmentFlag(const char* name) {
  const char* value = std::getenv(name);
  return value && *value && std::string_view(value) != "0";
}

}  // namespace

void Initialize(const std::filesystem::path& user_root) {
  if (!g_session_id.empty()) return;
  g_session_id = UtcTimestamp(true) + "-p" + std::to_string(GetCurrentProcessId());
  g_logs = user_root / "logs";
  std::error_code error;
  std::filesystem::create_directories(g_logs, error);
  std::filesystem::create_directories(user_root / "crashes", error);
  g_event_path = g_logs / (g_session_id + ".jsonl");
  crash::Install(user_root / "crashes", g_session_id);
  RecordEvent("process.start", {{"diagnostics_schema", "1"},
                                {"build_config", REXGLUE_BUILD_CONFIG},
                                {"user_root", rex::path_to_utf8(user_root)}});
  // Self-tests: each crashes and should leave a report in <user>/crashes.
  if (EnvironmentFlag("LAVENDER_SHIFT_CRASH_SELF_TEST")) {
    RecordEvent("diagnostics.crash_test.begin");
    crash::RaiseAccessViolation();
  }
  if (EnvironmentFlag("LAVENDER_SHIFT_EXECUTE_CRASH_SELF_TEST")) {
    RecordEvent("diagnostics.execute_crash_test.begin");
    crash::ExecuteNull();
  }
  if (const char* seconds = std::getenv("LAVENDER_SHIFT_DEFERRED_CRASH_SELF_TEST")) {
    const int delay = std::max(1, std::atoi(seconds));
    std::thread([delay] {
      std::this_thread::sleep_for(std::chrono::seconds(delay));
      RecordEvent("diagnostics.deferred_crash_test.begin");
      crash::RaiseAccessViolation();
    }).detach();
  }
}

void RefreshCrashReporter() {
  if (!g_session_id.empty()) crash::Refresh();
}

const std::string& SessionId() { return g_session_id; }

std::filesystem::path LogsDir() { return g_logs.empty() ? std::filesystem::path("logs") : g_logs; }

void RecordEvent(std::string_view event, std::initializer_list<Field> fields) {
  RecordEvent(event, std::span<const Field>(fields.begin(), fields.size()));
}

void RecordEvent(std::string_view event, std::span<const Field> fields) {
  std::ostringstream json;
  json << "{\"schema\":1,\"utc\":\"" << UtcTimestamp(false) << "\",\"session\":\""
       << JsonEscape(g_session_id) << "\",\"event\":\"" << JsonEscape(event) << "\",\"tid\":\""
       << GetCurrentThreadId() << '"';
  for (const auto& [key, value] : fields) {
    json << ",\"" << JsonEscape(key) << "\":\"" << JsonEscape(value) << '"';
  }
  json << '}';
  const std::string line = json.str();
  if (!g_event_path.empty()) {
    std::scoped_lock lock(g_event_mutex);
    std::ofstream output(g_event_path, std::ios::app | std::ios::binary);
    if (output) output << line << '\n';
  }
  REXLOG_INFO("{} {}", event, line);
}

}  // namespace lavender_shift::diagnostics
