"""Build fixtures in a caller-selected scratch directory, using verified SDK sources."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from common import ROOT, PINS, verify_source


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sdk", choices=["go", "rust"])
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sdk_root, build, output = args.sdk_root.resolve(), args.build_dir.resolve(), args.output.resolve()
    verify_source(sdk_root, PINS[args.sdk]["commit"])
    build.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    if args.sdk == "go":
        shutil.copy2(ROOT / "go/main.go", build / "main.go")
        shutil.copy2(ROOT / "go/go.sum", build / "go.sum")
        module = (ROOT / "go/go.mod").read_text(encoding="utf-8")
        module += "\nreplace github.com/modelcontextprotocol/go-sdk => " + json.dumps(sdk_root.as_posix()) + "\n"
        (build / "go.mod").write_text(module, encoding="utf-8")
        environment.setdefault("GOCACHE", str(build / "cache"))
        environment.setdefault("GOMODCACHE", str(build / "modules"))
        environment["GOTOOLCHAIN"] = "local"
        command = [str(args.tool.resolve()), "build", "-mod=readonly", "-o", str(output), "."]
        subprocess.run(command, cwd=build, env=environment, check=True)
    else:
        (build / "src").mkdir(exist_ok=True)
        shutil.copy2(ROOT / "rust/src/main.rs", build / "src/main.rs")
        shutil.copy2(ROOT / "rust/Cargo.lock", build / "Cargo.lock")
        manifest = (ROOT / "rust/Cargo.toml").read_text(encoding="utf-8")
        manifest = manifest.replace('"@RUST_SDK_CRATE@"', json.dumps((sdk_root / "crates/rmcp").as_posix()))
        (build / "Cargo.toml").write_text(manifest, encoding="utf-8")
        target = Path(environment.get("CARGO_TARGET_DIR", build / "target")).resolve()
        environment["CARGO_TARGET_DIR"] = str(target)
        command = [str(args.tool.resolve()), "build", "--locked", "--manifest-path", str(build / "Cargo.toml")]
        subprocess.run(command, cwd=build, env=environment, check=True)
        binary = target / "debug" / ("mcp-interop-audit.exe" if os.name == "nt" else "mcp-interop-audit")
        shutil.copy2(binary, output)
    record = {"sdk": args.sdk, "repository": PINS[args.sdk]["repository"], "commit": PINS[args.sdk]["commit"],
              "adapter": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps(record))


if __name__ == "__main__":
    main()
