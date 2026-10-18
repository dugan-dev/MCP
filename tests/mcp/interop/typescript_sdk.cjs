// Official SDK interoperability in both directions; no protocol mock.
const { createRequire } = require('node:module');
const { resolve } = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { createServer } = require('node:http');
const assert = require('node:assert/strict');
const [buildArg, packagesArg] = process.argv.slice(2);
const build = resolve(buildArg);
const sdk = createRequire(resolve(packagesArg, 'package.json'));
const { Client, StreamableHTTPClientTransport } = sdk('@modelcontextprotocol/client');
const { StdioClientTransport } = sdk('@modelcontextprotocol/client/stdio');
const { McpServer, createMcpHandler } = sdk('@modelcontextprotocol/server');
const { toNodeHandler } = sdk('@modelcontextprotocol/node');
const z = sdk('zod/v4');
const suffix = process.platform === 'win32' ? '.exe' : '';
const executable = name => resolve(build, name + suffix);
async function check(transport) {
    const client = new Client({name:'official-ts-test',version:'1'}, {versionNegotiation:{mode:'auto'}});
    try {
        await client.connect(transport);
        assert((await client.discover()).supportedVersions.includes('2026-07-28'));
        assert((await client.listTools()).tools.some(t => t.name === 'echo'));
        const results = await Promise.all(Array.from({length:16}, (_, i) => client.callTool({name:'echo',arguments:{message:`ts-${i}`}})));
        results.forEach((r, i) => assert.equal(r.content[0].text, `ts-${i}`));
        assert.equal((await client.callTool({name:'add',arguments:{a:2,b:3}})).structuredContent, 5);
        assert((await client.readResource({uri:'example://hello'})).contents[0].text);
        assert((await client.getPrompt({name:'greet',arguments:{name:'TS'}})).messages[0].content.text.includes('TS'));
    } finally { await client.close(); }
}
async function runClient(name, ...clientArguments) {
    const process = spawn(executable(name), clientArguments, {stdio:['ignore','pipe','inherit']});
    let output = '';
    process.stdout.on('data', data => output += data);
    const [code] = await once(process, 'exit');
    assert.equal(code, 0);
    assert(output.includes('Hello, MCP!'));
}
async function main() {
    await check(new StdioClientTransport({command:executable('mcp_stdio_server')}));
    const cppServer = spawn(executable('mcp_http_server'), ['0'], {stdio:['pipe','pipe','inherit']});
    try {
        const [data] = await once(cppServer.stdout, 'data');
        await check(new StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${Number(data.toString().trim())}/mcp`)));
    } finally { cppServer.stdin.end('\n'); await once(cppServer, 'exit'); }
    const handler = createMcpHandler(() => {
        const server = new McpServer({name:'official-ts-server',version:'1'});
        server.registerTool('echo', {inputSchema:z.object({message:z.string()})}, async ({message}) => ({content:[{type:'text',text:message}]}));
        return server;
    });
    const http = createServer(toNodeHandler(handler));
    await new Promise(resolve => http.listen(0, '127.0.0.1', resolve));
    try { await runClient('mcp_http_client', `http://127.0.0.1:${http.address().port}/mcp`); }
    finally { http.closeAllConnections(); await new Promise(resolve => http.close(resolve)); await handler.close(); }
    await runClient('mcp_stdio_client', process.execPath, resolve(__dirname, 'typescript_server.cjs'), resolve(packagesArg));
    console.log(JSON.stringify({sdk:'official-typescript',protocol:'2026-07-28',transports:['stdio','http'],directions:['sdk-client/cpp-server','cpp-client/sdk-server'],status:'passed'}));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
