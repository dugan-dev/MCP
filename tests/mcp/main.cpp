#include <cstdint>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <gtest/gtest.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace {
void print_active_exception() noexcept {
    if (const auto error = std::current_exception()) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception &exception) {
            std::fprintf(stderr, "Active exception: %s\n", exception.what());
        } catch (...) {
            std::fputs("Active exception is not derived from std::exception\n", stderr);
        }
    }
}
#ifdef _WIN32
void print_frame(void *address) noexcept {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "unknown module";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(address), &module))
        GetModuleFileNameA(module, name, MAX_PATH);
    const auto offset = reinterpret_cast<std::uintptr_t>(address) -
                        reinterpret_cast<std::uintptr_t>(module);
    std::fprintf(stderr, "  %p %s+0x%llx\n", address, name,
                 static_cast<unsigned long long>(offset));
    alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    const auto location = static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(address));
    if (SymFromAddr(GetCurrentProcess(), location, &displacement, symbol))
        std::fprintf(stderr, "    %s+0x%llx\n", symbol->Name,
                     static_cast<unsigned long long>(displacement));
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD line_offset = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), location, &line_offset, &line))
        std::fprintf(stderr, "    %s:%lu\n", line.FileName, line.LineNumber);
}
void print_stack() noexcept {
    // DbgHelp and the PDBs come from the existing Windows SDK/build. No remote symbols.
    char search[MAX_PATH]{};
    GetModuleFileNameA(nullptr, search, MAX_PATH);
    for (auto *end = search + std::strlen(search); end != search; --end) {
        if (end[-1] == '\\' || end[-1] == '/') {
            end[-1] = '\0';
            break;
        }
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    const bool symbols = SymInitialize(GetCurrentProcess(), search, TRUE) != FALSE;
    void *frames[32];
    const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
    std::fputs("Windows stack (module offsets):\n", stderr);
    for (unsigned i = 0; i < count; ++i)
        print_frame(frames[i]);
    if (symbols)
        SymCleanup(GetCurrentProcess());
    std::fflush(stderr);
}
void abort_signal(int) noexcept {
    // MSVC maintains terminate handlers per thread. SIGABRT also observes an abort
    // on an IO/worker thread that never installed main's terminate handler.
    std::fprintf(stderr, "Unexpected SIGABRT on Windows thread %lu\n", GetCurrentThreadId());
    print_active_exception();
    print_stack();
    // Returning preserves CRT abort's original nonzero process exit code.
}
LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *exception) noexcept {
    std::fprintf(stderr, "Unhandled Windows exception 0x%08lx\n",
                 exception->ExceptionRecord->ExceptionCode);
    print_frame(exception->ExceptionRecord->ExceptionAddress);
    print_stack();
    return EXCEPTION_CONTINUE_SEARCH;
}
#ifdef _DEBUG
int __cdecl crt_report(int, char *, int *) noexcept {
    print_stack();
    return FALSE;
}
#endif
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(unhandled_exception);
    std::signal(SIGABRT, abort_signal);
#endif
#ifdef _MSC_VER
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    _CrtSetReportHook(crt_report);
#endif
#endif
    std::set_terminate([] {
        std::fputs("Unexpected std::terminate in MCP test process\n", stderr);
        print_active_exception();
#ifdef _WIN32
        print_stack();
#endif
        std::fflush(stderr);
        std::abort();
    });
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
