"""Run the unmodified, pinned official requirement set for both SDK roles."""

import argparse
from collections import Counter
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import queue
import threading
import socket
import subprocess
import sys


def inspect_results(directory, role, names):
    scenario_results = []
    for scenario in names:
        pattern = f"server-{scenario}-20[0-9][0-9]-*/checks.json" if role == "server" else f"{scenario}-20[0-9][0-9]-*/checks.json"
        matches = list(directory.glob(pattern))
        if len(matches) != 1:
            scenario_results.append({"name": scenario, "passed": False, "error": "Missing or duplicate official result"})
            continue
        checks = json.loads(matches[0].read_text(encoding="utf-8"))
        failures = [check for check in checks if check["status"] == "FAILURE"]
        scenario_results.append({"name": scenario, "passed": any(check["status"] == "SUCCESS" for check in checks) and not failures,
                                 "statuses": dict(Counter(check["status"] for check in checks)),
                                 "failures": failures})
    return scenario_results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--role", choices=("server", "client", "both"), default="both")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    pin = json.loads((Path(__file__).parent / "pin.json").read_text(encoding="utf-8"))
    harness = args.harness.resolve()
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError("Use a fresh output directory so previous passes cannot hide missing scenarios")
    output.mkdir(parents=True)
    revision = subprocess.check_output(["git", "-C", str(harness), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin["revision"]:
        raise RuntimeError("Official conformance checkout does not match pin.json")
    subprocess.run(["git", "-C", str(harness), "diff", "--exit-code", "HEAD"], check=True)
    requirement_file = harness / "requirements" / f'{pin["protocol"]}.yaml'
    requirements = requirement_file.read_text(encoding="utf-8")
    if hashlib.sha256(requirements.encode()).hexdigest() != pin["requirements_sha256"]:
        raise RuntimeError("Official requirement set differs from the pinned source")
    required = {}
    for role in ("server", "client"):
        match = re.search(rf"^{role}:\n(.*?)(?=^\w+:|\Z)", requirements, re.M | re.S)
        required[role] = re.findall(r"^  - (\S+)$", match[1], re.M)
    suffix = ".exe" if os.name == "nt" else ""
    binaries = args.bin_dir.resolve()
    environment = os.environ.copy()
    environment["MCP_CONFORMANCE_CLIENT_EXE"] = str(binaries / f"test_conformance_client{suffix}")
    fingerprint = hashlib.sha256()
    sources = [root / name for name in ("CMakeLists.txt", "CMakePresets.json", "vcpkg.json", "vcpkg-configuration.json", ".gitmodules")]
    for folder in ("include", "src", "cmake", "tests/conformance"):
        sources.extend(path for path in (root / folder).rglob("*") if path.is_file() and path.suffix in (".h", ".cpp", ".py", ".cjs", ".json", ".cmake", ".txt"))
    for source in sorted(sources, key=lambda path: path.relative_to(root).as_posix()):
        fingerprint.update(source.relative_to(root).as_posix().encode() + b"\0")
        fingerprint.update(source.read_bytes().replace(b"\r\n", b"\n"))
    summary = {"harness": revision, "protocol": pin["protocol"], "roles": {}, "official_tier_assignment": False,
               "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
               "source_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True).strip()),
               "source_fingerprint": fingerprint.hexdigest(), "platform": platform.system(), "architecture": platform.machine()}
    roles = ("server", "client") if args.role == "both" else (args.role,)
    overall = True
    for role in roles:
        command = ["node", str(harness / "dist/index.js"), role, "--requirements", pin["protocol"],
                   "--output-dir", str(output / role), "--verbose"]
        server = None
        with (output / f"{role}-fixture.log").open("w", encoding="utf-8") as fixture_log:
            if role == "server":
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                server = subprocess.Popen([str(binaries / f"test_conformance_server{suffix}"), str(port)],
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=fixture_log, text=True)
                ready = queue.Queue()
                threading.Thread(target=lambda: ready.put(server.stdout.readline().strip()), daemon=True).start()
                try:
                    announced = ready.get(timeout=15)
                except queue.Empty:
                    announced = ""
                if announced != str(port):
                    server.kill()
                    server.wait()
                    raise RuntimeError("Conformance server failed to start")
                command += ["--url", f"http://127.0.0.1:{port}/mcp"]
            else:
                command += ["--command", "node tests/conformance/launch-client.cjs"]
            try:
                with (output / f"{role}.log").open("w", encoding="utf-8") as log:
                    result = subprocess.run(command, cwd=root, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                            timeout=1800)
            finally:
                if server:
                    server.stdin.close()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
        scenario_results = inspect_results(output / role, role, required[role])
        passed = sum(item["passed"] for item in scenario_results)
        role_passed = result.returncode == 0 and passed == len(required[role])
        overall = overall and role_passed
        summary["roles"][role] = {"passed": passed, "required": len(required[role]), "cli_exit": result.returncode,
                                  "scenarios": scenario_results}
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(f"{role}: {passed}/{len(required[role])} required scenarios passed; official CLI exit {result.returncode}", flush=True)
    return 0 if overall else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Conformance validation failed: {error}", file=sys.stderr)
        sys.exit(1)
