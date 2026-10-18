"""Run real pinned SDKs against C++ endpoints, with optional C++ reverse probe."""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading

from common import ROOT, PINS, verify_source


def first_line(process):
    result = queue.Queue()
    threading.Thread(target=lambda: result.put(process.stdout.readline()), daemon=True).start()
    try:
        line = result.get(timeout=20).strip()
        return int(line)
    except Exception:
        process.terminate()
        _, error = process.communicate(timeout=5)
        raise RuntimeError(f"server did not announce a port: {error}")


def invoke(command, log):
    completed = subprocess.run([str(value) for value in command], text=True, encoding="utf-8",
                               capture_output=True, timeout=60)
    log.write_text(json.dumps({"command": [str(value) for value in command], "returncode": completed.returncode,
                               "stdout": completed.stdout, "stderr": completed.stderr}, indent=2), encoding="utf-8")
    assert completed.returncode == 0, (str(log), completed.stdout, completed.stderr)
    assert '"passed"' in completed.stdout, (str(log), completed.stdout)


def stop(process, graceful=False):
    if process.poll() is None:
        if graceful:
            try:
                process.communicate(input="\n", timeout=5)
                return
            except subprocess.TimeoutExpired:
                pass
        process.terminate()
        try:
            process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-stdio-server", type=Path, required=True)
    parser.add_argument("--cpp-http-server", type=Path, required=True)
    parser.add_argument("--cpp-client", type=Path, required=True)
    parser.add_argument("--python-sdk-root", type=Path, required=True)
    parser.add_argument("--python-executable", type=Path, default=Path(sys.executable))
    parser.add_argument("--go-adapter", type=Path, required=True)
    parser.add_argument("--rust-adapter", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    verify_source(args.python_sdk_root.resolve(), PINS["python"]["commit"])
    commands = {"python": [args.python_executable.resolve(), ROOT / "python_adapter.py", "--sdk-root", args.python_sdk_root.resolve()],
                "go": [args.go_adapter.resolve()], "rust": [args.rust_adapter.resolve()]}
    for sdk, binary in (("go", args.go_adapter.resolve()), ("rust", args.rust_adapter.resolve())):
        record = binary.with_suffix(binary.suffix + ".json")
        if record.is_file():
            import hashlib
            metadata = json.loads(record.read_text(encoding="utf-8"))
            assert metadata["commit"] == PINS[sdk]["commit"], "adapter SDK revision mismatch"
            assert metadata["sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest(), "adapter binary changed"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    results = []
    for sdk in commands:
        command = commands[sdk]
        invoke([*command, "client-stdio", args.cpp_stdio_server.resolve()], output / f"{sdk}-client-stdio.json")
        results.append({"sdk": sdk, "commit": PINS[sdk]["commit"], "direction": "sdk-client/cpp-server", "transport": "stdio", "status": "passed"})
        server = subprocess.Popen([str(args.cpp_http_server.resolve()), "0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8")
        try:
            port = first_line(server)
            invoke([*command, "client-http", f"http://127.0.0.1:{port}/mcp"], output / f"{sdk}-client-http.json")
            results.append({"sdk": sdk, "commit": PINS[sdk]["commit"], "direction": "sdk-client/cpp-server", "transport": "http", "status": "passed"})
        finally:
            stop(server, graceful=True)
        if args.cpp_client:
            invoke([args.cpp_client.resolve(), "stdio", *command, "server-stdio"], output / f"{sdk}-server-stdio.json")
            results.append({"sdk": sdk, "commit": PINS[sdk]["commit"], "direction": "cpp-client/sdk-server", "transport": "stdio", "status": "passed"})
            server = subprocess.Popen([str(value) for value in [*command, "server-http"]], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
            try:
                port = first_line(server)
                invoke([args.cpp_client.resolve(), "http", f"http://127.0.0.1:{port}/mcp"], output / f"{sdk}-server-http.json")
                results.append({"sdk": sdk, "commit": PINS[sdk]["commit"], "direction": "cpp-client/sdk-server", "transport": "http", "status": "passed"})
            finally:
                stop(server)
    (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps({"protocol": "2026-07-28", "checks": results}, indent=2))


if __name__ == "__main__":
    main()
