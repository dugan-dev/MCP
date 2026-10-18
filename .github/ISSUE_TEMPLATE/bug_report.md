---
name: Bug report
about: Report a reproducible API, transport, build or packaging defect.
---

For vulnerabilities, follow [SECURITY.md](https://github.com/dugan-dev/MCP/blob/main/SECURITY.md).
Remove credentials and private application data from logs and examples.

## Revision and environment

- Commit or source snapshot, in addition to the version:
- OS, architecture, compiler and C++ standard library:
- Debug/Release, static/shared and UniMemory profile:
- Dependency versions, vcpkg baseline and any custom overlays:

## Reproducer

Provide the smallest complete program or project and the exact configure,
build and run commands. Explain expected behavior and actual output.
For transport failures, identify stdio/HTTP, lifecycle steps and the request
sequence using synthetic payloads.

## Evidence

Include relevant text diagnostics and failing tests. Identify whether the
failure occurs in the source build, an installed consumer or both, and list
other configurations that were checked.
