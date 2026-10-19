# Performance measurements

Build and run separately from correctness tests:

```sh
cmake --preset release -DMCP_BUILD_BENCHMARKS=ON
cmake --build --preset release --parallel 4
build/MCP-release/benchmarks/mcp_benchmark --iterations 1000 --output build/performance.json
```

Visual Studio places the executable in `benchmarks/Release/mcp_benchmark.exe`. Use a quiet machine and repeat independent process runs before interpreting small differences.

| Measurement | Timed work |
| --- | --- |
| `vector/std`, `vector/pmr-unimemory` | Reserve 256 64-bit integers, fill, consume and destroy; identical data and work |
| `json/parse` | MCP parsing, including duplicate-key and nesting checks |
| `json/serialize` | Serialize a prebuilt 52-byte input document |
| `schema/validate` | Validate against a precompiled schema, including JSON representation conversion |
| `rpc/local` | Synchronous client/server tool roundtrip through an in-process transport |
| `rpc/http-loopback` | The same tool call over a reused loopback HTTP connection |

Each measurement warms up, runs seven batches and records every batch's average nanoseconds per operation, its median, minimum and maximum. These are **batch averages**, not per-request latency percentiles. RPC concurrency is one. HTTP uses at most 1,000 iterations per batch. The report records the compiler, configuration, allocator backend, statistics option, payload size and hardware thread count.

The vector comparison isolates container allocation choices in one executable; switching to PMR does not guarantee an improvement. RPC timings include protocol validation and handler execution. JSON's standalone parsing/checking and allocator ownership costs are part of the measured implementation; an unchecked parser is not an equivalent baseline.

CI uploads results from each native Release/static platform job. Measurements have **no speed pass/fail threshold** and do not establish superiority over another SDK. Compare equivalent protocol revisions, payloads, validation, transports, connection reuse, concurrency and runtime/build options. Include CPU, OS and dependency versions when publishing external comparisons.
