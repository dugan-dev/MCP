#!/usr/bin/env python3
"""Run independent fresh stdio client processes concurrently; emit seed and per-child output."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--seed", type=int, default=1297047635)
    parser.add_argument("--transport", choices=("stdio", "http"), default="stdio")
    args = parser.parse_args()
    if not 1 <= args.clients <= 128:
        parser.error("--clients must be 1..128")
    suffix = ".exe" if os.name == "nt" else ""
    executable = args.build.resolve() / ("mcp_" + args.transport + "_client" + suffix)
    server = args.build.resolve() / ("mcp_" + args.transport + "_server" + suffix)
    if not executable.is_file() or not server.is_file():
        parser.error(f"client/server executable missing under {args.build}")

    def run(index, target):
        command = [str(executable), str(server)] if args.transport == "stdio" else [str(executable), target]
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        if result.returncode or "Hello, MCP!" not in result.stdout:
            raise RuntimeError(f"seed={args.seed} child={index} rc={result.returncode} stdout={result.stdout!r} stderr={result.stderr!r}")
        return {"child": index, "returncode": result.returncode, "stdout": result.stdout.strip()}

    http_server = None
    try:
        target = ""
        if args.transport == "http":
            http_server = subprocess.Popen([str(server), "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            port_line = http_server.stdout.readline()
            if not port_line.strip().isdigit():
                raise RuntimeError(f"seed={args.seed} HTTP server failed to report port: {port_line!r}")
            target = f"http://127.0.0.1:{int(port_line)}/mcp"
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
            children = list(pool.map(lambda index: run(index, target), range(args.clients)))
    finally:
        if http_server:
            http_server.stdin.write("\n")
            http_server.stdin.flush()
            http_server.wait(timeout=30)
    print(json.dumps({"seed": args.seed, "transport": args.transport, "independent_processes": args.clients, "status": "passed", "children": children}))


if __name__ == "__main__":
    main()
