const {createRequire} = require('node:module');
const {resolve} = require('node:path');
const sdk = createRequire(resolve(process.argv[2], 'package.json'));
const {McpServer} = sdk('@modelcontextprotocol/server');
const {serveStdio} = sdk('@modelcontextprotocol/server/stdio');
const z = sdk('zod/v4');
serveStdio(() => {
    const server = new McpServer({name:'official-ts-stdio',version:'1'});
    server.registerTool('echo', {inputSchema:z.object({message:z.string()})}, async ({message}) => ({content:[{type:'text',text:message}]}));
    return server;
}, {legacy:'reject', onerror: error => { console.error(error); process.exitCode = 1; }});
