# Roadmap

The supported protocol is **2026-07-28**. The target is the technical and documentation requirements of [SDK Tier 1](https://modelcontextprotocol.io/community/sdk-tiers). An SDK Working Group decision, a published production-ready release and ongoing maintenance commitments remain separate requirements.

| Work item | Acceptance evidence | Status |
| --- | --- | --- |
| Stateless discovery, metadata, HTTP headers and DNS rebinding protection | Official server/client requirement set | Implemented; repeat in CI |
| Tools, resources, templates, prompts and completions | Official server scenarios and compiled feature example | Implemented; repeat in CI |
| Input-required results, capability enforcement and multi-round state | Official MRTR scenarios in both directions | Implemented; repeat in CI |
| OAuth discovery, registration, PKCE, token authentication, issuer validation and scope escalation | Official client auth scenarios and native regressions | Implemented; repeat in CI |
| Reproducible official conformance integration | Pinned harness, raw checks, strict 69/69 required-scenario gate | Local verification; CI configuration included |
| Complete current-protocol usage documentation | Feature guide, runnable offline example, OAuth example and API contracts | Local verification |
| CMake host integration | Package/source consumers, host UniMemory reuse and preserved host configurations | Local verification |
| Native error detection | ASan/UBSan, TSan, Clang Analyzer, fuzz smoke and coverage artifacts | CI profiles configured; execution evidence required |
| Repeatable performance evidence | Seven controlled workloads, raw samples and native platform artifacts; no speed gate | Windows measurement; repeat on other platforms |
| Production-ready release | Green full-platform build, installed consumers, official conformance and release notes tied to one commit | Pending publication |
| Official tier assessment | Submit the published evidence to the SDK Working Group | Pending maintainer decision |

Experimental task/app extensions and legacy protocol compatibility are not part of this release's Tier 1 conformance denominator. Their tests remain visible and their independent limitations must be documented. Add legacy support only as an explicit feature with its own conformance revision and migration tests.

For a future protocol revision, review the stable specification and its frozen requirement set, list concrete implementation gaps here, update client/server fixtures, and run both the old claimed revision and the new revision before declaring support. Do not infer a scheduling commitment from this roadmap.
