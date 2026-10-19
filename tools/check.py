"""Check release metadata, local documentation links and project C++ formatting."""

import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    version = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))["version-string"]
    project = re.search(r"project\(MCP VERSION ([0-9.]+)", (root / "CMakeLists.txt").read_text(encoding="utf-8"))
    header = re.search(r'library_version = "([0-9.]+)"', (root / "include/mcp/protocol.h").read_text(encoding="utf-8"))
    if not project or not header or project[1] != version or header[1] != version:
        raise RuntimeError("CMake, vcpkg and public library versions differ")
    errors = []
    for document in [*root.glob("*.md"), *(root / "docs").glob("*.md")]:
        source = re.sub(r"```.*?```", "", document.read_text(encoding="utf-8"), flags=re.S)
        source = re.sub(r"`[^`]*`", "", source)
        for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", source):
            target = target.strip().split(' "')[0].strip("<>")
            if re.match(r"^(?:[a-z]+:|#)", target):
                continue
            target = unquote(target.split("#")[0])
            if target and not (document.parent / target).exists():
                errors.append(f"{document.relative_to(root)}: {target}")
    if errors:
        raise RuntimeError("Broken documentation links:\n" + "\n".join(errors))
    files = []
    for directory in ("include", "src", "cli", "examples", "tests", "benchmarks"):
        files.extend(path for path in (root / directory).rglob("*") if path.suffix in (".h", ".cpp"))
    for path in files:
        if path.read_bytes().endswith(b"\n\n"):
            raise RuntimeError(f"Trailing blank line: {path.relative_to(root)}")
    subprocess.run([args.clang_format, "--dry-run", "--Werror", "--style=file", *map(str, files)], check=True)
    print(f"MCP {version}: metadata, documentation links and {len(files)} C++ files passed")


if __name__ == "__main__":
    main()
