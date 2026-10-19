#pragma once
#include <algorithm>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#include <boost/process/v2/windows/show_window.hpp>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace mcp::detail {
namespace process = boost::process::v2;

inline void validate_process_text(const std::string& text) {
  if (text.find('\0') != std::string::npos)
    throw std::invalid_argument("Subprocess launch inputs cannot contain NUL");
}
inline std::filesystem::path process_path(const std::string& text) {
  validate_process_text(text);
  // Public process strings are UTF-8, including Windows paths.
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline void validate_process_arguments(const std::vector<std::string>& arguments) {
  for (const auto& argument : arguments)
    validate_process_text(argument);
}

inline process::process_environment process_environment(bool inherit, const std::map<std::string, std::string, std::less<>>& overrides) {
  std::vector<process::environment::key_value_pair> values;
  if (inherit)
    for (auto value : process::environment::current())
      values.emplace_back(value);
  for (const auto& [key, value] : overrides) {
    if (key.empty() || key.find_first_of("=\0", 0, 2) != std::string::npos)
      throw std::invalid_argument("Invalid subprocess environment key");
    validate_process_text(value);
    process::environment::key_value_pair replacement(std::make_pair(key, value));
    auto existing = std::find_if(values.begin(), values.end(), [&](const auto& item) { return item.key() == replacement.key(); });
    if (existing == values.end())
      values.push_back(std::move(replacement));
    else
      *existing = std::move(replacement);
  }
  // Windows environment names compare without case; keep its block ordered.
  std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) { return left.key() < right.key(); });
#ifdef _WIN32
  process::process_environment result(values);
  // Boost 1.91 produces a single terminator for an empty range. Windows
  // requires two NULs even for an explicitly empty environment block.
  if (result.unicode_env.size() == 1)
    result.unicode_env.push_back(L'\0');
#else
  process::process_environment result(std::vector<std::string>{});
  // The POSIX range constructor can borrow c_str() pointers. Transfer the
  // bytes into the initializer itself before constructing its envp array;
  // a move of these vectors then retains both the storage and its addresses.
  result.env_buffer = std::move(values);
  result.env.clear();
  result.env.reserve(result.env_buffer.size() + 1);
  for (const auto& value : result.env_buffer)
    result.env.push_back(value.c_str());
  result.env.push_back(nullptr);
#endif
  return result;
}

// V2 owns a single process. This narrow initializer retains the V1 transport's
// ownership of its descendants, including after the primary process exits.
class ProcessTree final {
#ifdef _WIN32
  HANDLE job_ = nullptr;
  process::error_code launch_error_;

 public:
  ProcessTree() {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_)
      throw std::system_error(GetLastError(), std::system_category(), "CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      const auto error = GetLastError();
      CloseHandle(job_);
      job_ = nullptr;
      throw std::system_error(error, std::system_category(), "SetInformationJobObject");
    }
  }
  process::error_code on_setup(process::windows::default_launcher& launcher, const std::filesystem::path&, const std::wstring&) noexcept {
    // Assign the still-suspended child before it can create descendants.
    launcher.creation_flags |= CREATE_SUSPENDED;
    return {};
  }
  void on_success(process::windows::default_launcher& launcher, const std::filesystem::path&, const std::wstring&) noexcept {
    const auto& info = launcher.process_information;
    if (!AssignProcessToJobObject(job_, info.hProcess))
      launch_error_.assign(GetLastError(), process::system_category());
    else if (ResumeThread(info.hThread) == static_cast<DWORD>(-1))
      launch_error_.assign(GetLastError(), process::system_category());
    if (launch_error_)
      TerminateProcess(info.hProcess, 1);
    // Do not throw here: V2 closes hThread and transfers hProcess to its
    // process object only after this hook returns.
  }
  void check_launch() const {
    if (launch_error_)
      throw process::system_error(launch_error_, "Subprocess tree setup failed");
  }
  void track(process::pid_type) noexcept {}
  void terminate() noexcept {
    if (job_) {
      TerminateJobObject(job_, 1);
      CloseHandle(job_);
      job_ = nullptr;
    }
  }
#else
  pid_t group_ = -1;

 public:
  ProcessTree() = default;
  process::error_code on_exec_setup(process::posix::default_launcher&, const std::filesystem::path&, const char* const*) noexcept {
    // Runs between fork and exec: setpgid is async-signal-safe. Failure is
    // returned through the launcher's existing child-error pipe.
    if (::setpgid(0, 0) == -1)
      return {errno, process::system_category()};
    return {};
  }
  void check_launch() const noexcept {}
  void track(process::pid_type id) noexcept { group_ = id; }
  void terminate() noexcept {
    if (group_ > 0) {
      ::kill(-group_, SIGKILL);
      group_ = -1;
    }
  }
#endif
  ProcessTree(const ProcessTree&) = delete;
  ProcessTree& operator=(const ProcessTree&) = delete;
  ~ProcessTree() { terminate(); }
};
}  // namespace mcp::detail
