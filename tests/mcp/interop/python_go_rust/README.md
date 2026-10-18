# Official Python / Go / Rust interoperability

This optional suite runs the actual official SDKs pinned in [pins.json](pins.json).
Each SDK acts as both client and server, over stdio and HTTP: twelve combinations.
Every combination checks modern discovery, tools list/call, sixteen concurrent
correlated calls, resources list/read, prompts list/get, and an unknown-tool
`-32602` error. Test data and services are synthetic and bound to localhost.

Python pins `Client(..., mode="2026-07-28")`; Rust uses the `Discover` lifecycle;
Go requests that version and asserts the negotiated result. Legacy fallback
cannot make a failed modern test pass. The SDK server fixtures also require
modern request metadata. This is core interoperability coverage, not an OAuth,
Tasks, extensions, cancellation, or full protocol conformance certification.

The suite is deliberately separate from default CTest: it requires externally
provisioned official SDK checkouts, their runtime dependencies and language
toolchains. No SDK, toolchain, downloaded package, or credentials are included.
The scripts verify checkout revisions and reject tracked SDK modifications.

## Prepare fixtures

Check out each official repository at the commit recorded in `pins.json`.
Use Python 3.11+, Go 1.25+, Rust 1.88+, and an installed MCP 0.0.1 package.
Install the Python checkout's runtime dependencies in an isolated environment;
the adapter imports `mcp` and `mcp-types` directly from that checkout. For frozen
provisioning, export the pinned checkout's `uv.lock` and install the exported
runtime requirements with hash checking. The neighboring older runtime lock
belongs to its separately documented SDK pin and must not be relabeled as this
checkout's lock.

Build the language fixtures into disposable directories. The checked Go sums
and Cargo lock constrain adapter dependencies; `build_adapter.py` writes the
local SDK path into generated manifests, leaving these source files unchanged.
On Windows, run the Rust build in a Visual Studio developer shell and use the
desired `CARGO_HOME` / `RUSTUP_HOME` when using a portable Rust installation.

```sh
python build_adapter.py go --sdk-root /path/to/go-sdk --tool /path/to/go \
  --build-dir /scratch/go-fixture --output /scratch/go-adapter
python build_adapter.py rust --sdk-root /path/to/rust-sdk --tool /path/to/cargo \
  --build-dir /scratch/rust-fixture --output /scratch/rust-adapter
```

Use `.exe` outputs on Windows. Each build writes a sibling JSON manifest with
the SDK commit and executable SHA-256; the matrix runner checks it when present.
Existing `GOCACHE`, `GOMODCACHE`, and `CARGO_TARGET_DIR` settings are honored;
otherwise generated caches stay in the selected build directory.

Build the C++ reverse client as an external installed-package consumer:

```sh
cmake -S . -B /scratch/cpp-client -DCMAKE_PREFIX_PATH=/path/to/MCP/install \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/dependency/toolchain.cmake
cmake --build /scratch/cpp-client --config Release
```

The dependency toolchain must use the same Boost.Process filesystem profile
as the MCP installation. Add dependency runtime DLL directories to the test
process PATH or stage the required DLLs beside the C++ probe on Windows.

## Execute all twelve combinations

```sh
python run_matrix.py \
  --cpp-stdio-server /path/to/mcp_stdio_server \
  --cpp-http-server /path/to/mcp_http_server \
  --cpp-client /scratch/cpp-client/interop_cpp_client \
  --python-executable /path/to/python-env/python \
  --python-sdk-root /path/to/python-sdk \
  --go-adapter /scratch/go-adapter \
  --rust-adapter /scratch/rust-adapter \
  --output-dir /scratch/interop-results
```

Visual Studio places the C++ probe under `Release/`; Windows executables have
the `.exe` suffix. Every executable and checkout path is explicit. The runner
writes each command, exit status, stdout/stderr and a machine-readable
`results.json` into the requested output directory. Only successful checks
enter the completed result matrix; the first failure raises an error with its
individual log. All launched processes are cleaned up on failure.
