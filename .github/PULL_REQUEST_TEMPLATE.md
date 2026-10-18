## Problem and change

Describe the trigger, root cause and resulting behavior. Identify public API,
wire behavior, ownership, dependency or packaging changes.

## Validation

Give the commands and results for the configurations you checked. For a fix,
show how the regression fails before the change. List skipped or untested
platforms and configurations explicitly.

- [ ] Relevant tests and independent public headers were checked.
- [ ] Static/shared installed consumers were checked when the exported API or dependencies changed.
- [ ] Runtime and third-party notices are retained when packaging changed.
- [ ] No credentials, private payloads, local configuration or build outputs are included.

See [CONTRIBUTING.md](https://github.com/dugan-dev/MCP/blob/main/CONTRIBUTING.md).
Report vulnerabilities through the route described in
[SECURITY.md](https://github.com/dugan-dev/MCP/blob/main/SECURITY.md), without public exploit details.
