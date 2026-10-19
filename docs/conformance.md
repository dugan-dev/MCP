# Official conformance

Target: **2026-07-28**. The unmodified official requirement set contains **37 server scenarios and 32 client scenarios**. Both roles must pass: a server-only result is not complete SDK conformance.

The external referee is pinned to `modelcontextprotocol/conformance` commit `c37eec888e1c6ff140af79987a40008548b7cc5f`; the requirement-file hash is recorded in `tests/conformance/pin.json`. The runner verifies both before executing. No expected-failure baseline, edited test, mocked protocol implementation, or selectively reduced required suite is used. The fixture registers the tools/resources/prompts requested by the official tests and invokes the actual MCP library.

## Run locally

Run from the repository root after initializing the UniMemory submodule and restoring the normal build dependencies:

```sh
cmake --preset release -DMCP_BUILD_CONFORMANCE=ON
cmake --build --preset release --parallel 4
git clone https://github.com/modelcontextprotocol/conformance.git build/official-conformance
git -C build/official-conformance checkout c37eec888e1c6ff140af79987a40008548b7cc5f
npm ci --ignore-scripts --prefix build/official-conformance
npm run build --prefix build/official-conformance
python tests/conformance/run.py --harness build/official-conformance --bin-dir build/MCP-release/tests/conformance --output build/conformance-results
```

With Visual Studio, add `/Release` to `--bin-dir`. If the harness is already cloned, omit `git clone`. Use a new output directory on each run: the runner refuses to reuse old evidence. Node.js 24 and Python 3.12 are used in CI; npm uses the official checkout's lockfile. Tests contact local loopback fixtures, not an external MCP service or a real authorization account.

## Evidence and scoring

The runner executes the official CLI's `--requirements 2026-07-28` for each role, requires exit code zero, and independently checks that every required scenario produced exactly one nonempty result without failures. This prevents a missing result or a similarly named scenario from being counted as a pass. `summary.json`, raw `checks.json`, client stdout/stderr and fixture logs are retained under the chosen output directory; CI uploads them as artifacts.

The current local qualification passed **37/37 server and 32/32 client scenarios (100%)**. This is a result for the source built in that run, not a statement that the previously published 0.0.1 tag already contains these changes.

The official requirement set also runs **13 server and 7 client scenarios outside its score**. Those are marked upstream as experimental extensions, pending, or added after the revision's release. Their raw failures/skips remain visible. In particular, this qualification does not claim DPoP/WIF or complete experimental Tasks coverage. Legacy initialize checks are not applicable because this SDK does not claim that lifecycle; the harness may report such subchecks as skipped. They are never relabeled as successes.

## Tier and release boundaries

A 100% technical result is one part of Tier 1. Full feature documentation, a dependency policy, a roadmap, a production-ready published release and ongoing maintenance commitments are also required. Only the SDK Working Group assigns the official tier. Local documentation and CI edits are not published evidence until their commit is pushed and a release is designated.

Before a production-ready release, require the complete supported platform matrix, both official conformance roles, native regressions, compiled examples and installed static/shared consumers to pass on the same source. Retain the exact commit, toolchains, dependency pins and conformance artifacts in the GitHub Release notes. Do not change the old release tag to point at a new implementation.

References: [SDK tiers](https://modelcontextprotocol.io/community/sdk-tiers), [official conformance framework](https://github.com/modelcontextprotocol/conformance), [dependency policy](dependency-policy.md), [roadmap](roadmap.md).
