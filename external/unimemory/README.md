# UniMemory source snapshot

Pinned upstream main: [`3dbe431a6c9a4bb5cf0aebd9e472baebfd96f925`](https://github.com/dugan-dev/UniMemory/tree/3dbe431a6c9a4bb5cf0aebd9e472baebfd96f925).

Build files, headers, inline implementation, core C++ tests, examples and optional benchmarks are copied without modification. `snapshot.json` records their upstream hashes and the previous revision. Report-tool tests, data, images and documentation are omitted.

Select `UNIMEMORY_BACKEND`, `UNIMEMORY_STATISTICS` and `UNIMEMORY_CHECKS` through DearAgentKit CMake. Include `<unimem/all.h>` and construct `unimem::Global`, `Heap` or `Stack`; retain the concrete context while its consumers exist. [Upstream lifetime guide](https://github.com/dugan-dev/UniMemory/blob/3dbe431a6c9a4bb5cf0aebd9e472baebfd96f925/docs/compatibility.md).

Correctness and performance are separate acceptance results. This snapshot does not claim that the ongoing upstream performance measurement has met every threshold.
