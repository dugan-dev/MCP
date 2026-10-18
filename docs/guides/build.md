# Build and installation

Use CMake 3.25+, a C++20 compiler and vcpkg. Set `VCPKG_ROOT` to its checkout. Dependencies are selected before `project()`; an explicit toolchain or dependency prefix takes precedence.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --install build/MCP-release --config Release
```

For Debug replace `release` with `debug`, and `Release` with `Debug`. Only these two presets exist. The default generator and architecture are used; choose another generator with `-G` on the first configure in a clean build directory.

| Item | Debug | Release |
| --- | --- | --- |
| Build directory | `build/MCP-debug` | `build/MCP-release` |
| Install prefix | `build/install/debug` | `build/install/release` |
| vcpkg packages | `build/dependencies` | `build/dependencies` |

## Targets and options

The executable target **`exe_cli`** produces **`mcp`** and is the Visual Studio startup project. Library targets begin `lib_`, example targets `example_`, tests `test_`, and helpers `fixture_`. Example binaries are `mcp_stdio_server`, `mcp_stdio_client`, `mcp_http_server`, `mcp_http_client`; Visual Studio places them in `examples/mcp/Debug` or `Release` under the build directory.

`MCP_BUILD_TESTS`, `MCP_BUILD_CLI` and `MCP_BUILD_EXAMPLES` default ON. `BUILD_SHARED_LIBS` selects the MCP library type. Tests also require `BUILD_TESTING`; examples used as fixtures are built when tests need them.

Optional instrumentation uses `MCP_SANITIZER=address|thread`, `MCP_COVERAGE=ON`, or `MCP_BUILD_FUZZERS=ON`. Sanitizers/coverage require GCC or Clang; fuzzers require Clang. These are CMake options, not extra presets or claims that every instrumentation combination has been qualified.

## Dependency profile

The manifest pins a vcpkg baseline. Required components include Boost 1.91+ Asio, Beast and Process, libcurl 7.85+, OpenSSL 3+, nlohmann/json 3.11+ and jsoncons. GoogleTest is used by tests.

Boost.Process V2 must be **compiled** with `BOOST_PROCESS_USE_STD_FS=ON`. Defining the macro only in a consumer is not enough: it changes the compiled Process interface. The repository's `vcpkg-configuration.json` automatically selects the `ports/boost-process` profile for CMake and direct `vcpkg install`. Source and installed-package configuration reject a conflicting Process profile. Boost.Filesystem is not required.

For package-manager dependencies, supply their toolchain/prefix. Ensure their Boost.Process target exports `BOOST_PROCESS_USE_STD_FS` and was built accordingly. An explicit vcpkg overlay may override this project's overlay; it must retain that profile.

Installation retains complete dependency notices, including libraries compiled from headers. With vcpkg these are found beside the selected package headers. For a system package with a different license layout, supply its original files through the reported `MCP_<PACKAGE>_LICENSE_FILES` cache variable. Configuration names the missing package and variable; notices from an unrelated SDK prefix are not substituted.

UniMemory selection uses an existing `UniMemory::UniMemory` target, a compatible installed package, or the pinned official fallback source in `external/unimemory`. The default profile is `standard`, statistics OFF, checks AUTO. Do not mix incompatible allocator profiles in a process. The installed package also requires the exact nlohmann/json version used to build it because JSON types cross public interfaces.

## Installed consumers

```cmake
cmake_minimum_required(VERSION 3.25)
project(MyApp LANGUAGES CXX)
find_package(MCP 0.0.1 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE MCP::MCP)
```

Configure with `CMAKE_PREFIX_PATH` pointing to the MCP install and the matching dependency toolchain. The static library still needs its dependency development packages. On Windows the installation collects runtime DLLs and their notices; an application must deploy the runtime closure beside its own executable or on its runtime search path.

The checked-in `tests/consumer` builds and runs two real installed-package consumers. A Debug-only installation with a multi-config generator requires `-DCMAKE_CONFIGURATION_TYPES=Debug`; do not map a Debug library into an unrelated Release configuration.

For source embedding, set unwanted `MCP_BUILD_*` options OFF before `add_subdirectory`, then link `MCP::MCP`. The parent must configure the dependency toolchain before its own `project()`; a subdirectory cannot retroactively select it.

## Troubleshooting

- **Missing `nlohmann_jsonConfig.cmake`:** confirm `VCPKG_ROOT` or an explicit vcpkg toolchain was available on the first configure. Start with a fresh build directory after correcting dependency selection. A stale cache can retain a previous toolchain/prefix.
- **Boost.Process filesystem mismatch:** rebuild the dependency using the included overlay/profile; do not suppress the check or mix its compiled path types.
- **Missing DLL at launch:** use the installed CLI and its collected `bin` contents, or deploy the application dependency closure. Matching import libraries alone are insufficient.
- **UniMemory profile mismatch:** use one backend/statistics/checks profile across all linked libraries, then rebuild.

Do not put API keys, personal configurations or build products into source control.
