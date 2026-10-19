#pragma once
// Test-only native allocation fault injection shared by lifecycle regressions.
#include <atomic>
#include <cstddef>
#include <string>
#if defined(_MSC_VER) && defined(_DEBUG)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <crtdbg.h>
#include <dbghelp.h>
#endif
namespace mcp_test {
#if defined(_MSC_VER) && defined(_DEBUG)
class AllocationFault {
 public:
  AllocationFault(const char* pattern, std::size_t bytes = 0, unsigned occurrence = 1, DWORD thread = 0)
      : bytes_(bytes), remaining_(occurrence), thread_(thread) {
    // An imported function address may be an EXE import thunk. Resolve the
    // owning MCP image explicitly; a static build places it in the EXE.
    HMODULE module = GetModuleHandleW(L"MCPd.dll");
    if (!module)
      module = GetModuleHandleW(nullptr);
    wchar_t image[32768]{};
    const auto length = GetModuleFileNameW(module, image, 32768);
    if (!length || length >= 32768)
      return;
    const std::wstring image_path(image, length);
    const auto separator = image_path.find_last_of(L"\\/");
    const auto search = image_path.substr(0, separator);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    symbols_ = SymInitializeW(GetCurrentProcess(), search.c_str(), FALSE) != FALSE;
    // The CRT reporter may already own a session. Reuse it when initialization
    // reports "already initialized", and only clean up a session we created.
    // Explicit image + local PDB directory removes working-directory and
    // automatic module-invasion timing from symbol resolution.
    SymSetSearchPathW(GetCurrentProcess(), search.c_str());
    SymLoadModuleExW(GetCurrentProcess(), nullptr, image_path.c_str(), nullptr, reinterpret_cast<DWORD64>(module), 0, nullptr, 0);
    SymEnumSymbols(GetCurrentProcess(), reinterpret_cast<DWORD64>(module), pattern, collect, this);
  }
  ~AllocationFault() {
    disarm();
    if (symbols_)
      SymCleanup(GetCurrentProcess());
  }
  bool available() const noexcept { return ranges_ != 0; }
  void arm() noexcept {
    active_.store(this);
    previous_.store(_CrtSetAllocHook(hook));
    armed_ = true;
  }
  void disarm() noexcept {
    if (armed_) {
      _CrtSetAllocHook(previous_.load());
      active_.store(nullptr);
      armed_ = false;
    }
  }
  bool rejected() const noexcept { return rejected_.load(); }

 private:
  struct Range {
    DWORD64 begin, end;
  } range_[64]{};
  unsigned ranges_ = 0;
  const std::size_t bytes_;
  std::atomic<unsigned> remaining_;
  const DWORD thread_;
  bool symbols_ = false, armed_ = false;
  std::atomic<bool> rejected_{false};
  std::atomic<_CRT_ALLOC_HOOK> previous_{nullptr};
  static inline std::atomic<AllocationFault*> active_{nullptr};
  static BOOL CALLBACK collect(PSYMBOL_INFO symbol, ULONG, PVOID context) {
    auto& self = *static_cast<AllocationFault*>(context);
    if (self.ranges_ < 64 && symbol->Size)
      self.range_[self.ranges_++] = {symbol->Address, symbol->Address + symbol->Size};
    return TRUE;
  }
  static int hook(int type, void* data, std::size_t size, int block, long request, const unsigned char* file, int line) {
    auto* self = active_.load();
    if (self && type == _HOOK_ALLOC && !self->rejected_.load() && (!self->bytes_ || size == self->bytes_) &&
        (!self->thread_ || self->thread_ == GetCurrentThreadId())) {
      void* stack[40];
      const auto count = CaptureStackBackTrace(0, 40, stack, nullptr);
      bool matched = false;
      for (USHORT frame = 0; frame < count && !matched; ++frame)
        for (unsigned index = 0; index < self->ranges_; ++index) {
          const auto address = reinterpret_cast<DWORD64>(stack[frame]);
          if (address >= self->range_[index].begin && address < self->range_[index].end) {
            matched = true;
            break;
          }
        }
      auto remaining = self->remaining_.load();
      while (matched && remaining && !self->remaining_.compare_exchange_weak(remaining, remaining - 1)) {}
      if (matched && remaining == 1) {
        self->rejected_.store(true);
        return FALSE;
      }
    }
    const auto previous = self ? self->previous_.load() : nullptr;
    return previous ? previous(type, data, size, block, request, file, line) : TRUE;
  }
};
#endif
}  // namespace mcp_test
