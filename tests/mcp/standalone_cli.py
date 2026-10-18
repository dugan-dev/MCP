"""Standalone command entry smoke test against real retained C++ server."""
import json
from pathlib import Path
import subprocess
import sys

binary, server = map(str, map(Path, sys.argv[1:3]))
def run(*args):
    result = subprocess.run([binary, *args], text=True, capture_output=True, timeout=20)
    assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout
assert run('--help').startswith('MCP command line client')
assert run('--version').strip() == 'MCP 0.0.1'
result = json.loads(run('--exec', server, 'tools', 'call', 'echo', json.dumps({'message': 'direct standalone MCP'})))
assert result['content'][0]['text'] == 'direct standalone MCP', result
print('Direct MCP help/version and real stdio echo roundtrip passed')
