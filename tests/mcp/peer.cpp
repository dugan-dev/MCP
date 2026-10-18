#include <iostream>
#include <condition_variable>
#include <mutex>
#include <mcp/mcp.h>
#include <thread>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace {
std::string path_text(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
#ifdef _WIN32
std::string utf8(std::wstring_view value) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
#endif
std::string synthetic_environment(const char *name) {
#ifdef _WIN32
    std::wstring key(name, name + std::char_traits<char>::length(name));
    auto size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    size = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    value.resize(size);
    return utf8(value);
#else
    const auto value = std::getenv(name);
    return value ? value : "";
#endif
}
int peer_main(const std::vector<std::string> &args) {
    const std::string mode = args.size() > 1 ? args[1] : "healthy";
    if (mode == "tree-leaf") {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        return 0;
    }
    if (mode == "process-info" || mode == "tree-live" || mode == "tree-exit") {
        using namespace mcp;
        std::string line;
        if (!std::getline(std::cin, line)) return 1;
        const auto request = parse_json(line);
        Json result;
        if (mode == "process-info") {
            result = {{"cwd", path_text(std::filesystem::current_path())},
                      {"arguments", Json::array()},
                      {"parent", synthetic_environment("MCP_PROCESS_PARENT_ONLY")},
                      {"override", synthetic_environment("MCP_PROCESS_OVERRIDE")},
                      {"unicode", synthetic_environment("MCP_PROCESS_UNICODE")}};
            for (std::size_t i = 2; i < args.size(); ++i) result["arguments"].push_back(args[i]);
            std::cerr << "mcp-process-stderr-fixture\n";
#ifdef _WIN32
            STARTUPINFOW startup{};
            GetStartupInfoW(&startup);
            result["hidden"] = (startup.dwFlags & STARTF_USESHOWWINDOW) && startup.wShowWindow == SW_HIDE;
#endif
        } else {
#ifdef _WIN32
            const auto executable = std::filesystem::u8path(args[0]);
            std::wstring command = L"\"" + executable.native() + L"\" tree-leaf";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 2;
            result = {{"descendant", process.dwProcessId}, {"primary", GetCurrentProcessId()}};
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
#else
            const auto pid = fork();
            if (pid < 0) return 2;
            if (pid == 0) {
                close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
                sleep(15);
                _exit(0);
            }
            result = {{"descendant", pid}, {"primary", getpid()}};
#endif
        }
        std::cout << serialize_json(Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}}) << '\n';
        std::cout.flush();
        if (mode == "tree-exit") {
            // Give the requester time to consume the response before the EOF.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return 0;
        }
        if (mode == "tree-live") std::this_thread::sleep_for(std::chrono::seconds(15));
        else while (std::getline(std::cin, line)) {}
        return 0;
    }
    if (mode == "healthy" || mode == "logging" || mode == "tasks") {
        using namespace mcp;
        mcp::Server server;
        if (mode == "tasks") {
            using namespace mcp::extensions;
            auto store = std::make_shared<MemoryTaskStore>();
            store->create(make_task("issuer\nalice", "private-task", TaskStatus::working,
                                   std::chrono::minutes(1), std::chrono::milliseconds(1)));
            install_tasks(server, store);
        }
        std::mutex gate_mutex;
        std::condition_variable gate;
        bool released = false;
        if (mode == "logging") {
            server.add_tool({"logged"}, [&](const Json &args, RequestContext &context) {
                const auto label = args.value("label", std::string("work"));
                context.log("debug", label + "-debug");
                context.log("info", label + "-begin", "peer");
                context.progress(1);
                if (args.value("gate", false)) {
                    std::unique_lock lock(gate_mutex);
                    gate.wait_for(lock, std::chrono::seconds(3), [&] { return released; });
                }
                if (args.value("late", false))
                    std::this_thread::sleep_for(std::chrono::milliseconds(args.value("delayMs", 150)));
                context.log("warning", label + "-end", "peer");
                context.progress(2);
                return text_result(label);
            });
            server.add_tool({"release"}, [&](const Json &, RequestContext &) {
                {
                    std::lock_guard lock(gate_mutex);
                    released = true;
                }
                gate.notify_all();
                return text_result("released");
            });
        }
        server.add_tool({"progress"}, [](const mcp::Json &, mcp::RequestContext &context) {
            context.progress(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return mcp::text_result("done");
        });
        mcp::run_stdio(server, std::cin, std::cout);
        return 0;
    }
    std::string line;
    if (!std::getline(std::cin, line))
        return 0;
    if (mode == "hang-signal" && args.size() > 2) {
        std::ofstream marker(std::filesystem::u8path(args[2]));
        marker << "request received";
    }
    if (mode == "graceful-close") {
        using namespace mcp;
        const auto request = parse_json(line);
        std::cout << serialize_json(Json{{"jsonrpc", "2.0"}, {"id", request["id"]},
                                        {"result", {{"resultType", "complete"}}}}) << '\n';
        std::cout.flush();
        while (std::getline(std::cin, line)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return 0;
    }
    if (mode == "invalid-log" || mode == "log-flood") {
        using namespace mcp;
        const auto request = parse_json(line);
        if (mode == "invalid-log")
            std::cout << serialize_json(make_notification("notifications/message",
                             {{"level", 42}, {"data", "invalid level"}})) << '\n';
        else {
            for (int index = 0; index < 32; ++index)
                std::cout << serialize_json(make_notification("notifications/message",
                                 {{"level", "info"}, {"data", std::string(512, 'x')}})) << '\n';
            std::cout << serialize_json(Json{{"jsonrpc", "2.0"}, {"id", request["id"]},
                                            {"result", text_result("done")}}) << '\n';
        }
        std::cout.flush();
        std::getline(std::cin, line);
        return 0;
    }
    if (mode == "crash")
        return 2;
    if (mode == "malformed")
        std::cout << "{invalid}\n";
    else if (mode == "oversize")
        std::cout << std::string(4096, 'x') << '\n';
    else if (mode == "wrong-id")
        std::cout << R"({"jsonrpc":"2.0","id":999999,"result":{"resultType":"complete"}})" << '\n';
    std::cout.flush();
    std::getline(std::cin, line);
    return 0;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(utf8(argv[i]));
    return peer_main(args);
}
#else
int main(int argc, char **argv) {
    return peer_main(std::vector<std::string>(argv, argv + argc));
}
#endif
