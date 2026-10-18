#!/usr/bin/env python3
"""Exercise built C++ servers as independent long-lived MCP processes."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import subprocess
import time


VERSION = "2026-07-28"
META = {
    "io.modelcontextprotocol/protocolVersion": VERSION,
    "io.modelcontextprotocol/clientCapabilities": {},
    "io.modelcontextprotocol/clientInfo": {"name": "real-usage", "version": "1"},
}


def request(identifier, method, params=None):
    arguments = dict(params or {})
    arguments["_meta"] = META
    return {"jsonrpc": "2.0", "id": identifier, "method": method, "params": arguments}


def checked_echo(response, identifier, message):
    assert response["id"] == identifier, response
    assert response["result"]["content"][0]["text"] == message, response


def stdio_round_trip(server, client, calls):
    process = subprocess.Popen(
        [str(server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
    )
    try:
        def exchange(payload):
            process.stdin.write(json.dumps(payload, ensure_ascii=False) + "\n")
            process.stdin.flush()
            line = process.stdout.readline()
            assert line, f"stdio server exited: {process.poll()}"
            return json.loads(line)

        discovery = exchange(request(1, "server/discover"))
        assert VERSION in discovery["result"]["supportedVersions"], discovery
        listed = exchange(request(2, "tools/list"))
        assert {tool["name"] for tool in listed["result"]["tools"]} >= {"echo", "add"}
        resource = exchange(request(3, "resources/read", {"uri": "example://hello"}))
        assert resource["result"]["contents"][0]["text"] == "Hello, MCP!"
        prompt = exchange(request(4, "prompts/get", {"name": "greet", "arguments": {"name": "Ada"}}))
        assert "Ada" in prompt["result"]["messages"][0]["content"]["text"]

        for index in range(calls):
            message = f"stdio-{index}-世界"
            checked_echo(exchange(request(index + 100, "tools/call", {
                "name": "echo", "arguments": {"message": message}
            })), index + 100, message)

        invalid = exchange(request(calls + 100, "tools/call", {
            "name": "echo", "arguments": {"message": 7}
        }))
        assert "error" in invalid, invalid
        unknown = exchange(request(calls + 101, "unknown/method"))
        assert unknown["error"]["code"] == -32601, unknown
        process.stdin.write("{\n")
        process.stdin.flush()
        malformed = json.loads(process.stdout.readline())
        assert malformed["error"]["code"] == -32700, malformed
        checked_echo(exchange(request(calls + 102, "tools/call", {
            "name": "echo", "arguments": {"message": "recovered"}
        })), calls + 102, "recovered")
        process.stdin.close()
        assert process.wait(timeout=20) == 0, process.stderr.read()
        native = subprocess.run([str(client), str(server)], text=True, capture_output=True,
                                encoding="utf-8", timeout=15)
        assert native.returncode == 0 and "Hello, MCP!" in native.stdout, native
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def http_round_trip(server, client, cli, calls, workers):
    process = subprocess.Popen(
        [str(server), "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
    )
    try:
        port_line = process.stdout.readline().strip()
        assert port_line.isdigit(), (port_line, process.poll())
        port = int(port_line)
        native = subprocess.run([str(client), f"http://127.0.0.1:{port}/mcp"],
                                text=True, capture_output=True, encoding="utf-8", timeout=15)
        assert native.returncode == 0 and "Hello, MCP!" in native.stdout, native
        if cli:
            command = [str(cli), "mcp", "--url", f"http://127.0.0.1:{port}/mcp",
                       "tools", "call", "echo", json.dumps({"message": "cli-live"})]
            command_result = subprocess.run(command, text=True, capture_output=True,
                                            encoding="utf-8", timeout=15)
            assert command_result.returncode == 0, command_result
            assert json.loads(command_result.stdout)["content"][0]["text"] == "cli-live"

        def exchange(connection, payload, *, method="POST", path="/mcp", headers=None, body=None):
            fields = {
                "Content-Type": "application/json",
                "Accept": "application/json, text/event-stream",
                "Mcp-Protocol-Version": VERSION,
                "Mcp-Method": payload["method"],
            }
            if payload["method"] in {"tools/call", "prompts/get", "resources/read"}:
                fields["Mcp-Name"] = payload["params"].get("name", payload["params"].get("uri", ""))
            if headers:
                fields.update(headers)
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if body is None else body
            connection.request(method, path, body=data, headers=fields)
            response = connection.getresponse()
            raw = response.read()
            return response.status, json.loads(raw) if raw else None

        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        status, discovery = exchange(connection, request(1, "server/discover"))
        assert status == 200 and VERSION in discovery["result"]["supportedVersions"]
        status, listed = exchange(connection, request(2, "tools/list"))
        assert status == 200 and any(tool["name"] == "echo" for tool in listed["result"]["tools"])
        status, resource = exchange(connection, request(3, "resources/read", {"uri": "example://hello"}))
        assert status == 200 and resource["result"]["contents"][0]["text"] == "Hello, MCP!"

        started = time.monotonic()

        def worker(worker_id):
            peer = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
            try:
                for index in range(worker_id, calls, workers):
                    message = f"http-{index}-世界"
                    identifier = index + 100
                    status, result = exchange(peer, request(identifier, "tools/call", {
                        "name": "echo", "arguments": {"message": message}
                    }))
                    assert status == 200, (identifier, status, result)
                    checked_echo(result, identifier, message)
            finally:
                peer.close()

        with ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(worker, range(workers)))
        elapsed = time.monotonic() - started

        status, invalid = exchange(connection, request(calls + 100, "tools/call", {
            "name": "echo", "arguments": {"message": 7}
        }))
        assert status in {200, 400} and invalid["error"]["code"] == -32602, (status, invalid)
        status, unknown = exchange(connection, request(calls + 101, "unknown/method"))
        assert unknown["error"]["code"] == -32601, (status, unknown)
        status, _ = exchange(connection, request(calls + 102, "tools/list"), body=b"{")
        assert status == 400
        status, _ = exchange(connection, request(calls + 103, "tools/list"), method="GET")
        assert status == 405
        status, _ = exchange(connection, request(calls + 104, "tools/list"), path="/wrong")
        assert status == 404
        status, _ = exchange(connection, request(calls + 105, "tools/list"),
                             headers={"Origin": "https://untrusted.example"})
        assert status == 403
        status, _ = exchange(connection, request(calls + 106, "tools/list"),
                             headers={"Mcp-Method": "tools/call"})
        assert status == 400
        status, recovery = exchange(connection, request(calls + 107, "tools/call", {
            "name": "echo", "arguments": {"message": "recovered"}
        }))
        assert status == 200
        checked_echo(recovery, calls + 107, "recovered")
        connection.close()
        process.stdin.write("\n")
        process.stdin.flush()
        assert process.wait(timeout=20) == 0, process.stderr.read()
        return elapsed
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--calls", type=int, default=1000)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--cli", type=Path)
    args = parser.parse_args()
    if not 1 <= args.calls <= 100000 or not 1 <= args.workers <= 128 or not 1 <= args.cycles <= 10:
        parser.error("calls must be 1..100000, workers 1..128 and cycles 1..10")
    suffix = ".exe" if os.name == "nt" else ""
    build = args.build.resolve()
    stdio_server = build / ("mcp_stdio_server" + suffix)
    http_server = build / ("mcp_http_server" + suffix)
    if not stdio_server.is_file() or not http_server.is_file():
        parser.error(f"C++ server executables missing under {build}")
    stdio_client = build / ("mcp_stdio_client" + suffix)
    http_client = build / ("mcp_http_client" + suffix)
    if not stdio_client.is_file() or not http_client.is_file():
        parser.error(f"C++ client executables missing under {build}")
    cli = args.cli.resolve() if args.cli else None
    if cli and not cli.is_file():
        parser.error(f"CLI executable missing: {cli}")
    elapsed = 0.0
    for _ in range(args.cycles):
        stdio_round_trip(stdio_server, stdio_client, args.calls)
        elapsed += http_round_trip(http_server, http_client, cli, args.calls, args.workers)
    print(json.dumps({"status": "passed", "server_start_stop_cycles": args.cycles,
                      "stdio_calls": args.calls * args.cycles,
                      "http_calls": args.calls * args.cycles, "http_workers": args.workers,
                      "http_seconds": round(elapsed, 3)}))


if __name__ == "__main__":
    main()
