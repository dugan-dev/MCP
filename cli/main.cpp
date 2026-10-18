#include <csignal>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif
namespace mcp::cli { int entry(int, char**); }
namespace {
int run(std::vector<std::string> args) {
    const bool compatibility = !args.empty() && args.front() == "mcp";
    if (compatibility)
        args.erase(args.begin());
    if (!compatibility && args.size() == 1 && args.front() == "--version") {
        std::cout << "MCP " << MCP_VERSION << '\n';
        return 0;
    }
    if (args.empty() || (args.size() == 1 && args.front() == "help"))
        args = {"--help"};
    args.insert(args.begin(), "mcp");
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    return mcp::cli::entry(static_cast<int>(args.size()), argv.data());
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return 2;
        std::string arg(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                arg.data(), size, nullptr, nullptr))
            return 2;
        arg.pop_back();
        args.push_back(std::move(arg));
    }
#else
int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::cerr.tie(nullptr);
    return run(std::move(args));
}
