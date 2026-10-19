# Contributing

Submit changes through a pull request with the revision, a concrete problem and
reproducible evidence. Keep C++20, the `mcp` namespace, public headers under
`include/mcp` and the exported `MCP::MCP` target. Explain API, wire behavior,
dependency and allocator-profile changes in the pull request.

For a fix, add a regression that fails on the previous implementation and
describe the root cause. Default tests use synthetic payloads, subprocesses and
loopback servers. Do not commit credentials, private payloads, local settings,
dependency binaries or build output. Report security issues using [SECURITY.md](SECURITY.md).

## Versioning and breaking changes

Use semantic versioning. Patch releases fix defects without intentionally changing documented valid source usage; minor releases add compatible functionality. A source-breaking change requires a major release once 1.0 is published; before 1.0 it requires a minor-version change. Changes to public signatures, ownership, protocol guarantees or accepted valid inputs are breaking changes. Tightening malformed or unsafe input handling is a security/correctness fix, with migration notes when callers may be affected. Public option/result aggregates may gain fields: use named members and default initialization, not a fixed positional structured-binding count.

Explain breaking changes and migration steps in the GitHub Release notes before publication. Do not overwrite an existing release tag. Public C++ binary ABI compatibility is not promised across compiler/runtime/dependency changes: rebuild consumers, including after public aggregate layouts change. Internal `src/` details are not public API.

A production-ready stable release is designated explicitly in its GitHub Release notes, tied to one commit, after the complete supported platform matrix, installation consumers and both official conformance roles pass. A green local run alone is not an official tier assignment or a production-readiness declaration for an older release. Version notes live in GitHub Releases.

See `docs/dependency-policy.md` and `docs/roadmap.md` for dependency review and the technical release gates. These documents intentionally do not create response-time or critical-bug repair promises.

## Native validation

Set `VCPKG_ROOT` to a bootstrapped vcpkg checkout, then use the existing presets:

```sh
git submodule update --init --recursive
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
