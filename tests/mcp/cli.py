import json
import os
import re
import http.client
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary, *args, expected=0):
    result = subprocess.run([binary, "mcp", *args], text=True, capture_output=True, timeout=10)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result


def main():
    binary = sys.argv[1]
    assert "Usage:" in run(binary, "--help").stdout
    assert run(binary, "--version").stdout.strip() == "MCP 1.0.0"
    assert "valid JSON" in run(binary, "--url", "http://127.0.0.1:1", "tools", "call", "x", "{", expected=2).stderr
    assert "choose exactly one" in run(binary, "--url", "http://localhost", "--exec", "unused", "tools", "list", expected=2).stderr
    assert "task input responses must be a JSON object" in run(binary, "--url", "http://localhost", "tasks", "update", "id", "[]", expected=2).stderr
    assert "require --exec" in run(binary, "--url", "http://localhost", "--cwd", ".", "tools", "list", expected=2).stderr

    # A real child process verifies argv forwarding and the initialize/list/call exchange.
    peer = r'''import json,sys
sys.stderr.write("stdio-inherited-stderr\n");sys.stderr.flush()
for line in sys.stdin:
    message=json.loads(line)
    method=message.get("method", "")
    if not method or method.startswith("notifications/"): continue
    if method == "initialize":
        result={"protocolVersion":"2026-07-28","capabilities":{"tools":{}},"serverInfo":{"name":"test","version":"1"}}
    elif method == "tools/list":
        result={"tools":[{"name":"echo","inputSchema":{"type":"object"}}]}
    elif method == "tools/call":
        import os
        arguments=message["params"]["arguments"]
        text=os.getcwd()+"|"+os.environ.get("MCP_TEST_ENV", "") if arguments.get("environment") else arguments.get("value", "")
        result={"content":[{"type":"text","text":text} ]}
    elif method == "resources/list": result={"resources":[{"uri":"fixture://one","name":"one"}]}
    elif method == "resources/templates/list": result={"resourceTemplates":[{"uriTemplate":"fixture://{name}","name":"template"}]}
    elif method == "tasks/get": result={"resultType":"complete","taskId":message["params"]["taskId"],"status":"working"}
    elif method == "tasks/update": result={"resultType":"complete","taskId":message["params"]["taskId"],"status":"working"}
    elif method == "tasks/cancel": result={"resultType":"complete","taskId":message["params"]["taskId"],"status":"cancelled"}
    else:
        sys.stdout.write(json.dumps({"jsonrpc":"2.0","id":message["id"],"error":{"code":-32601,"message":"unknown"}})+"\n");sys.stdout.flush();continue
    sys.stdout.write(json.dumps({"jsonrpc":"2.0","id":message["id"],"result":result})+"\n");sys.stdout.flush()
'''
    listed = run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", peer, "tools", "list")
    assert json.loads(listed.stdout)[0]["name"] == "echo"
    assert "stdio-inherited-stderr" in listed.stderr
    called = run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", peer,
                 "tools", "call", "echo", '{"value":"hello"}')
    assert json.loads(called.stdout)["content"][0]["text"] == "hello"

    assert json.loads(run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", peer,
                          "resources", "list").stdout)[0]["uri"] == "fixture://one"
    assert json.loads(run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", peer,
                          "resources", "templates").stdout)[0]["uriTemplate"] == "fixture://{name}"
    for operation, expected in (("get", "working"), ("update", "working"), ("cancel", "cancelled")):
        command = [binary, "mcp", "--exec", sys.executable, "--arg", "-c", "--arg", peer, "tasks", operation, "task-1"]
        if operation == "update":
            command.append('{"answer":{"action":"approve"}}')
        task_result = subprocess.run(command, text=True, capture_output=True, timeout=10)
        assert task_result.returncode == 0, (operation, task_result.stderr)
        assert json.loads(task_result.stdout)["status"] == expected

    with tempfile.TemporaryDirectory(prefix="mcp-cli-") as working_directory:
        output = run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", peer,
                     "--cwd", working_directory, "--env", "MCP_TEST_ENV=ready",
                     "tools", "call", "echo", '{"environment":true}')
        actual_cwd, actual_env = json.loads(output.stdout)["content"][0]["text"].split("|", 1)
        assert Path(actual_cwd).resolve() == Path(working_directory).resolve()
        assert actual_env == "ready"

    inspected = run(binary, "inspect", "--inspector", sys.executable, "--arg", "-c",
                     "--arg", "import sys; print('|'.join(sys.argv[1:]))", "--", "one", "two words")
    assert inspected.stdout.strip() == "one|two words"

    inspected_io = subprocess.run(
        [binary, "mcp", "inspect", "--inspector", sys.executable, "--arg", "-c", "--arg",
         "import sys; print('out:' + sys.stdin.readline().strip()); "
         "print('inherited-stderr', file=sys.stderr); sys.exit(7)"],
        input="inherited-stdin\n", text=True, capture_output=True, timeout=10)
    assert inspected_io.returncode == 7, (inspected_io.returncode, inspected_io.stdout, inspected_io.stderr)
    assert inspected_io.stdout.strip() == "out:inherited-stdin"
    assert inspected_io.stderr.strip() == "inherited-stderr"

    failed = run(binary, "--exec", sys.executable, "--arg", "-c", "--arg", "import sys; sys.exit(7)",
                 "tools", "list", expected=3)
    assert "transport error" in failed.stderr

    with tempfile.TemporaryDirectory(prefix="mcp-serve-") as directory:
        config_path = Path(directory) / "server.json"
        config_path.write_text(json.dumps({
            "name": "cli-fixture", "transport": "stdio",
            "tools": [{"name": "fixture", "inputSchema": {"type": "object"},
                       "result": {"content": [{"type": "text", "text": "from-config"}]}}],
        }), encoding="utf-8")
        server = subprocess.Popen([binary, "mcp", "serve", "--config", str(config_path)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True)
        try:
            metadata = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                        "io.modelcontextprotocol/clientCapabilities": {}}
            server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "server/discover",
                "params": {"_meta": metadata}}) + "\n")
            server.stdin.flush()
            discovered = json.loads(server.stdout.readline())
            assert discovered["id"] == 1 and "result" in discovered
            server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {"_meta": metadata}}) + "\n")
            server.stdin.flush()
            listed_tools = json.loads(server.stdout.readline())
            assert listed_tools["result"]["tools"][0]["name"] == "fixture"
            server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                "params": {"name": "fixture", "arguments": {}, "_meta": metadata}}) + "\n")
            server.stdin.flush()
            called_tool = json.loads(server.stdout.readline())
            assert called_tool["result"]["content"][0]["text"] == "from-config"
            server.stdin.close()
            assert server.wait(timeout=10) == 0
        finally:
            if server.poll() is None:
                server.kill()
                server.wait(timeout=5)

    with tempfile.TemporaryDirectory(prefix="mcp-http-config-") as directory:
        config_path = Path(directory) / "server.json"
        config_path.write_text(json.dumps({
            "transport": "http", "host": "127.0.0.1", "port": 0,
            "allowedHosts": ["localhost"], "allowedOrigins": ["https://example.test"],
        }), encoding="utf-8")
        server = subprocess.Popen([binary, "mcp", "serve", "--config", str(config_path)],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE, text=True)
        try:
            line = server.stderr.readline()
            match = re.search(r"127\.0\.0\.1:(\d+)/mcp", line)
            assert match, line
            port = int(match.group(1))
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            connection.request("GET", "/mcp", headers={"Host": f"localhost:{port}", "Origin": "https://example.test"})
            assert connection.getresponse().status == 405
            connection.close()
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
            connection.request("GET", "/mcp", headers={"Host": f"localhost:{port}", "Origin": "https://blocked.test"})
            assert connection.getresponse().status == 403
            connection.close()
        finally:
            server.kill()
            server.wait(timeout=5)

        config_path.write_text(json.dumps({"transport": "http", "host": "0.0.0.0"}), encoding="utf-8")
        remote = run(binary, "serve", "--config", str(config_path), expected=2)
        assert "loopback only" in remote.stderr


if __name__ == "__main__":
    main()
