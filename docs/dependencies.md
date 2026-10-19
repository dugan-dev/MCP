# MCP dependencies

The package retains the pinned implementation's dependency versions and vcpkg baseline `2f1d605400c8727cc00c15797aba796c88ccd523`.

| Dependency | Required scope | License |
|---|---|---|
| Official UniMemory >=0.0.1 and <0.1.0 | Public allocation owner; Git submodule pinned at `3dbe431` | MIT |
| nlohmann/json >=3.11 | Public JSON ABI; installed package enforces exact build version | MIT |
| libcurl >=7.85 | HTTP client/TLS | curl license |
| Boost Asio/Beast/Process >=1.91 | HTTP server and subprocess V2; standard filesystem profile | BSL-1.0 |
| OpenSSL >=3 | Authentication and HTTP TLS | Apache-2.0 |
| jsoncons | Compiled implementation's JSON Schema validation; build-time header dependency | BSL-1.0 |
| Threads | Thread support | Platform toolchain |
| GoogleTest | Tests only | BSD-3-Clause |
| mimalloc / jemalloc | Optional official allocator backends | MIT / BSD-2-Clause |

On Windows the library additionally links ws2_32 and bcrypt. Installed consumers discover UniMemory, exact nlohmann/json, CURL, OpenSSL, Boost Asio/Beast/Process and Threads. Boost.Process must be compiled with `BOOST_PROCESS_USE_STD_FS=ON`; the manifest's local overlay selects that profile without Boost.Filesystem. jsoncons does not appear in public headers or the export link interface. Header/transitive dependencies are resolved through vcpkg or the selected package manager. Native binaries are absent from the source package; Windows installation collects runtime dependency DLLs and their package notices.
