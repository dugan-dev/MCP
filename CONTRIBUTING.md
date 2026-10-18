# Contributing

Submit changes through a pull request with the revision, a concrete problem and
reproducible evidence. Keep C++20, the `mcp` namespace, public headers under
`include/mcp` and the exported `MCP::MCP` target. Explain API, wire behavior,
dependency and allocator-profile changes in the pull request.

For a fix, add a regression that fails on the previous implementation and
describe the root cause. Default tests use synthetic payloads, subprocesses and
loopback servers. Do not commit credentials, private payloads, local settings,
dependency binaries or build output. Report security issues using [SECURITY.md](SECURITY.md).

## Local validation

Set `VCPKG_ROOT` to a bootstrapped vcpkg checkout, then use the existing presets:

```sh
cmake --preset debug
cmake --build --preset debug --parallel 4
ctest --preset debug --output-on-failure
```

Repeat with `release`. For exported API, dependency or lifecycle changes, also
check static/shared builds, independent public-header compilation and an
installed consumer. Use separate build and install directories when comparing
linkage or allocator profiles. The [validation guide](docs/validation.md)
describes project evidence and additional interoperability checks. Report
untested configurations and intentional skips; a successful test count does not
establish correctness for every input.

## CI configurations

[The build workflow](.github/workflows/build.yml) configures, builds, tests and
installs Debug and Release with both static and shared MCP libraries:

| Runner | Compiler / generator | Dependency triplet |
| --- | --- | --- |
| Ubuntu 24.04 | Runner GCC / Ninja | `x64-linux` |
| macOS 15 | Runner AppleClang / Ninja | Native `arm64-osx` or `x64-osx` |
| Windows 2022 | MSVC / Visual Studio 2022 | `x64-windows` |

The workflow reads the vcpkg revision from the manifest and uses this repository's
Process V2 overlay. Cached binary packages are separated by runner OS,
architecture and manifest/overlay content; vcpkg also checks each package ABI.
`BUILD_SHARED_LIBS` selects MCP linkage independently of the dependency triplet's
own linkage. These jobs are configured checks, not a claim that an unrun platform
has already passed.

Each job configures `tests/consumer` against the installation and the existing
dependency SDK, without another manifest install. It runs the consumer roundtrip,
README consumer and installed CLI. Windows runs with only the installation's
`bin` and Windows system directories on `PATH`; Unix runs without
library-path overrides so ordinary installed-library loading is exercised.
Failure diagnostics are retained as workflow artifacts. CI uses read-only
repository permissions and commit-pinned actions.
