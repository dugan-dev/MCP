# Dependency updates

MCP uses a fixed vcpkg baseline, the local Boost.Process overlay, and the UniMemory Git submodule. Protocol schemas, SDK interoperability fixtures and official conformance tests have explicit source revisions. Dependencies do not silently follow upstream branches.

Before each release, review upstream releases and security advisories for runtime and test dependencies. Propose a baseline, submodule or lockfile update as a reviewable change; document compatibility and license changes. Do not automatically run an audit tool's bulk dependency rewrite.

An update is accepted only after:

1. The declared C++20 toolchains build Debug/Release and static/shared configurations.
2. The full native tests and installed consumers pass.
3. Both official conformance roles pass every required scenario for the supported protocol revision, without an expected-failure baseline.
4. The affected SDK interoperability, TLS, cancellation, memory ownership and process-lifetime regressions pass.
5. Source attribution, third-party notices, dependency versions and reproducible evidence are updated.

Changes to nlohmann/json, compiler runtimes, public layouts or allocator profiles require rebuilding consumers. Preserve the selected UniMemory profile across every library in a process. Security fixes take priority over feature upgrades; incompatible updates require the versioning and migration process in CONTRIBUTING.md. This policy does not promise a vulnerability response deadline.

The conformance pin is in `tests/conformance/pin.json`. Update it deliberately, retaining the official requirement sets and reporting new tests separately from the frozen revision requirements. Never edit the upstream tests to change a failure into a pass.
