# Validation

MCP targets protocol **2026-07-28**. Tests use synthetic data, real local subprocesses and loopback HTTP servers.

## Native tests

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release --parallel 4 --output-on-failure
```

Repeat with `debug` to exercise MSVC Debug CRT fault injection. The suite covers messages, schemas, authorization, pagination, TLS/HTTP/SSE, Unicode process launch, cancellation, ownership, shutdown races and stress workloads. Public headers are compiled independently; the offline feature example is also executed.

## Installed packages

```sh
python .github/scripts/installed_consumer.py --preset release --linkage static --triplet x64-windows
```

Use the triplet and linkage selected for the source build. The check installs MCP, builds external consumers, all ten C++ feature-guide snippets and the actual C++ blocks extracted from both READMEs, then runs the CLI and English README server. Windows uses a restricted runtime search path; dependency notices are checked too. Static/shared and Debug/Release are separate configurations.

## Protocol and SDK interoperability

The [official conformance guide](conformance.md) describes the fixed **37 server + 32 client** requirement set and its strict, zero-failure gate. Upstream extension/pending scenarios are retained as separate diagnostics; they are not counted as successes.

The [SDK interoperability harness](https://github.com/dugan-dev/MCP/tree/main/tests/mcp/interop/python_go_rust) exercises official Python, Go and Rust SDKs in both client/server directions over stdio and HTTP. Exact external revisions and adapters are included with the harness.

## Continuous integration

The [build workflow](https://github.com/dugan-dev/MCP/actions/workflows/build.yml) contains 12 native combinations: Windows, Linux and macOS; Debug and Release; static and shared libraries. Each builds, tests, installs MCP and checks external consumers. Release/static jobs also verify package/source embedding with a host-provided UniMemory target, run both official conformance roles and record performance without a speed threshold.

Three additional Linux profiles run Clang ASan/UBSan with static analysis and two 10,000-execution fuzz smoke runs, GCC TSan, and GCC coverage. Sanitizer diagnostics are fatal. Coverage is exported as HTML/XML; its nonzero floor rejects a missing report rather than claiming complete coverage. Fuzz inputs are copied into the build directory before mutation. A separate repository check verifies metadata, local documentation links and clang-format 19.1.7. Logs and measurements are uploaded as artifacts; configured profiles are not evidence of execution until that commit runs.

Live results establish which configurations passed for a particular commit. A local pass does not establish an unrun platform result or an official SDK tier. Comparative benchmarks, arbitrary third-party services and every experimental extension are outside this qualification.
