"""Real official Python SDK endpoints; always exercise the 2026 protocol."""
import argparse
import asyncio
import json
from pathlib import Path
import socket
import sys

from common import PINS, verify_source

VERSION = "2026-07-28"


def configure_sdk(sdk_root):
    global Client, StdioServerParameters, types, Server, stdio_server, MCPError
    verify_source(sdk_root, PINS["python"]["commit"])
    sys.path[:0] = [str(sdk_root / "src"), str(sdk_root / "src/mcp-types")]
    from mcp import Client, StdioServerParameters, types
    from mcp.server.lowlevel import Server
    from mcp.server.stdio import stdio_server
    from mcp.shared.exceptions import MCPError


def server():
    async def tools(ctx, params):
        assert ctx.protocol_version == VERSION
        return types.ListToolsResult(tools=[types.Tool(name="echo", input_schema={"type":"object", "properties":{"message":{"type":"string"}}, "required":["message"]})])
    async def call(ctx, params):
        assert ctx.protocol_version == VERSION
        if params.name != "echo":
            raise MCPError(code=-32602, message="unknown tool")
        return types.CallToolResult(content=[types.TextContent(text=params.arguments["message"])])
    async def resources(ctx, params):
        assert ctx.protocol_version == VERSION
        return types.ListResourcesResult(resources=[types.Resource(uri="example://hello", name="hello")])
    async def read(ctx, params):
        assert ctx.protocol_version == VERSION
        return types.ReadResourceResult(contents=[types.TextResourceContents(uri="example://hello", text="Hello, MCP!")])
    async def prompts(ctx, params):
        assert ctx.protocol_version == VERSION
        return types.ListPromptsResult(prompts=[types.Prompt(name="greet", arguments=[types.PromptArgument(name="name", required=True)])])
    async def prompt(ctx, params):
        assert ctx.protocol_version == VERSION
        return types.GetPromptResult(messages=[types.PromptMessage(role="user", content=types.TextContent(text="Greet " + params.arguments["name"]))])
    return Server("official-python-interop", on_list_tools=tools, on_call_tool=call,
                  on_list_resources=resources, on_read_resource=read, on_list_prompts=prompts, on_get_prompt=prompt)


async def check(target):
    async with Client(target, mode=VERSION) as client:
        assert client.protocol_version == VERSION
        discovery = await client.session.discover()
        assert VERSION in discovery.supported_versions
        tools = await client.list_tools()
        assert "echo" in {t.name for t in tools.tools}
        results = await asyncio.gather(*(client.call_tool("echo", {"message": f"sdk-{i}"}) for i in range(16)))
        assert [result.content[0].text for result in results] == [f"sdk-{i}" for i in range(16)]
        assert any(str(resource.uri) == "example://hello" for resource in (await client.list_resources()).resources)
        assert (await client.read_resource("example://hello")).contents[0].text == "Hello, MCP!"
        assert "greet" in {p.name for p in (await client.list_prompts()).prompts}
        assert "SDK" in (await client.get_prompt("greet", {"name": "SDK"})).messages[0].content.text
        try:
            await client.call_tool("missing-tool", {})
        except MCPError as error:
            assert error.error.code == -32602, error
        else:
            raise AssertionError("unknown tool did not produce invalid_params")
    print(json.dumps({"sdk": "python", "protocol": VERSION, "status": "passed", "concurrent_calls": 16}))


async def http_server(app, port):
    import uvicorn
    listener = socket.socket()
    listener.bind(("127.0.0.1", port))
    listener.listen()
    print(listener.getsockname()[1], flush=True)
    config = uvicorn.Config(app.streamable_http_app(stateless_http=True), log_level="error")
    await uvicorn.Server(config).serve(sockets=[listener])


async def run_stdio(app):
    async with stdio_server() as (read, write):
        await app.run(read, write, app.create_initialization_options())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("mode", choices=["server-stdio", "server-http", "client-stdio", "client-http"])
    parser.add_argument("target", nargs="?")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    configure_sdk(args.sdk_root.resolve())
    if args.mode == "server-stdio":
        asyncio.run(run_stdio(server()))
    elif args.mode == "server-http":
        asyncio.run(http_server(server(), int(args.target or "0")))
    elif args.mode == "client-http":
        asyncio.run(check(args.target))
    else:
        asyncio.run(check(StdioServerParameters(command=args.target, args=args.arguments)))


if __name__ == "__main__":
    main()
