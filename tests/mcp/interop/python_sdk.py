"""Exercise cpp-mcp through a pinned official Python SDK checkout.

Run with its uv environment; --sdk-root makes workspace sources importable.
"""
import argparse
import asyncio
import json
import os
from pathlib import Path
import sys


async def check(client):
    discovery = await client.session.discover()
    assert "2026-07-28" in discovery.supported_versions
    tools = await client.list_tools()
    assert {"echo", "add", "slow"} <= {tool.name for tool in tools.tools}
    results = await asyncio.gather(*(client.call_tool("echo", {"message": f"sdk-{i}"}) for i in range(16)))
    assert [r.content[0].text for r in results] == [f"sdk-{i}" for i in range(16)]
    result = await client.call_tool("add", {"a": 2, "b": 3})
    assert result.structured_content == 5
    resource = await client.read_resource("example://hello")
    assert resource.contents[0].text
    prompt = await client.get_prompt("greet", {"name": "SDK"})
    assert "SDK" in prompt.messages[0].content.text


async def main(args):
    if args.sdk_root:
        root = Path(args.sdk_root).resolve()
        sys.path[:0] = [str(root / "src"), str(root / "src/mcp-types")]
    from mcp import Client, StdioServerParameters
    build = Path(args.build).resolve()
    suffix = ".exe" if os.name == "nt" else ""
    async with Client(StdioServerParameters(command=str(build / f"mcp_stdio_server{suffix}"))) as client:
        await check(client)
    server = await asyncio.create_subprocess_exec(str(build / f"mcp_http_server{suffix}"), "0",
                                                 stdin=asyncio.subprocess.PIPE,
                                                 stdout=asyncio.subprocess.PIPE)
    try:
        port = int(await asyncio.wait_for(server.stdout.readline(), 10))
        async with Client(f"http://127.0.0.1:{port}/mcp") as client:
            await check(client)
    finally:
        server.stdin.write(b"\n")
        await server.stdin.drain()
        await asyncio.wait_for(server.wait(), 10)
    print(json.dumps({"sdk": "official-python", "protocol": "2026-07-28",
                      "transports": ["stdio", "http"], "concurrent_requests": 16, "status": "passed"}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--sdk-root")
    asyncio.run(main(parser.parse_args()))
