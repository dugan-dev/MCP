const { spawnSync } = require('node:child_process');
const executable = process.env.MCP_CONFORMANCE_CLIENT_EXE;
if (!executable) throw new Error('MCP_CONFORMANCE_CLIENT_EXE is required');
const child = spawnSync(executable, process.argv.slice(2), { stdio: 'inherit', env: process.env });
if (child.error) throw child.error;
process.exit(child.status === null ? 1 : child.status);
