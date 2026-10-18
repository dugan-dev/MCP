# Release validation

MCP **0.0.1** targets protocol **2026-07-28**. The checks below use synthetic data, real local subprocesses and loopback HTTP servers. They do not certify arbitrary third-party services or every extension combination.

## Reproduce

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release --parallel 4 --output-on-failure
cmake --install build/MCP-release --config Release
```

Repeat with the `debug` preset and `Debug` configuration. Configure prerequisites and the two presets are described in the [build guide](guides/build.md). The public headers are compiled individually; installed consumers also compile the exact README example and execute a real client/server roundtrip.

## Local qualification — 2026-10-19

Windows x64 used CMake 4.4.2, MSVC 19.51, Boost 1.91 and the standard UniMemory profile (statistics OFF, checks AUTO). Dependencies were restored from the pinned vcpkg baseline and local Boost.Process overlay.

| Check | Result |
| --- | --- |
| Complete Release build and CTest | 243 entries: 237 passed, 6 explicitly skipped, 0 failed |
| Complete Debug build and CTest | 243 passed, 0 skipped, 0 failed; all six native fault-injection tests executed |
| Static Debug and Release install and external consumer | Passed |
| Shared Release install and external consumer | Passed |
| Installed CLI, consumer and README example with restricted Windows PATH | Passed |
| Installed third-party notices | Six dependency notice files matched their selected SDK sources byte for byte |
| Official Python, Go and Rust SDK interop | 12/12 passed: each SDK, both client/server directions, stdio and HTTP |
| Native Linux Release build and CTest (GCC 13.3) | 245 entries: 239 passed, 6 MSVC-only skips, 0 failed |
| Native Linux static install and external consumer / README example | Passed with independently built Linux dependencies |
| Native Linux environment-lifetime and send/close race regressions | Passed |
| Workflow syntax, documentation links and source provenance hashes | Checked before publication |

The six Release skips exercise native allocation/thread-start failure through the MSVC Debug CRT. They are not protocol exclusions. The tests cover malformed messages, schemas, authorization helpers, task deadlines, pagination, HTTP/TLS/SSE, process launch options, Unicode, cancellation, process-tree cleanup, resource ownership and shutdown races. Failure injection verifies that failed thread creation and shutdown allocation do not leak the subprocess or strand pending requests.

The SDK matrix asserts the modern protocol lifecycle and exercises discovery, tool listing/calls, 16 concurrent calls, resources, prompts and structured invalid-parameter errors. Exact SDK revisions, adapter sources and reproduction commands are retained in the [interop harness](https://github.com/dugan-dev/MCP/tree/main/tests/mcp/interop/python_go_rust). See [compatibility](compatibility.md) for the version boundary.

## Continuous integration

The [build workflow](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) has 12 combinations: Windows, Linux and macOS; Debug and Release; static and shared libraries. Each job builds, runs its configured test suite, installs the package and runs an external consumer. The workflow retains failure diagnostics. Its live results, rather than the existence of a configuration, establish which platforms passed for a commit.

Local macOS execution, alternative allocator profiles, exhaustive fuzzing, long-running production deployments and comparative performance benchmarks are not claimed by this release. API contracts and limitations are documented in the [API reference](api-reference.md).
