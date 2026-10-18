"""Install the configured MCP build and exercise its exported package and runtime."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def run(arguments, **kwargs):
    print("+", " ".join(map(str, arguments)), flush=True)
    return subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def executable(directory, name, configuration):
    suffix = ".exe" if os.name == "nt" else ""
    for candidate in (directory / configuration / (name + suffix), directory / (name + suffix)):
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"Consumer executable was not built: {directory / name}")


def cache_entry(cache, name):
    for line in cache.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith(name + ":"):
            return line.partition("=")[2]
    return ""


def runtime_environment(prefix):
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        environment.pop(name, None)
    if os.name == "nt":
        # os.environ is case-insensitive on Windows; its plain dict copy is not.
        system_root = Path(os.environ["SystemRoot"])
        for name in list(environment):
            if name.casefold() == "path":
                del environment[name]
        environment["PATH"] = os.pathsep.join(map(str, (
            prefix / "bin", system_root / "System32", system_root,
        )))
    return environment


def check_dependency_licenses(prefix):
    requirements = {
        "nlohmann-json": ("Niels Lohmann", "Permission is hereby granted", "IN NO EVENT", "SOFTWARE."),
        "jsoncons": ("Daniel Parker", "Boost Software License - Version 1.0", "CONTRACT, TORT OR OTHERWISE"),
        "boost-process": ("Boost Software License - Version 1.0", "Permission is hereby granted", "CONTRACT, TORT OR OTHERWISE"),
    }
    for package, markers in requirements.items():
        directory = prefix / "share" / "licenses" / "MCP" / "dependencies" / package
        files = [path for path in directory.glob("*") if path.is_file()]
        text = "\n".join(path.read_text(encoding="utf-8-sig") for path in files)
        if not all(marker in text for marker in markers):
            raise RuntimeError(f"Missing or incomplete installed {package} license: {directory}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=("debug", "release"), required=True)
    parser.add_argument("--linkage", choices=("static", "shared"), required=True)
    parser.add_argument("--triplet", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    configuration = args.preset.capitalize()
    source_build = root / "build" / f"MCP-{args.preset}"
    prefix = root / "build" / "ci-install" / args.linkage / args.preset
    consumer_build = root / "build" / f"ci-consumer-{args.linkage}-{args.preset}"
    sdk = root / "build" / "dependencies" / args.triplet
    run(["cmake", "--install", source_build, "--config", configuration, "--prefix", prefix])
    for relative in (
        "include/mcp/all.h",
        "include/mcp/memory.h",
        "lib/cmake/MCP/MCPConfig.cmake",
        "share/licenses/MCP/LICENSE",
        "share/licenses/MCP/NOTICE",
        "share/licenses/MCP/mcp/LICENSE",
        "share/licenses/MCP/schema/MCP-LICENSE",
    ):
        if not (prefix / relative).is_file():
            raise RuntimeError(f"Incomplete installed package: {prefix / relative}")
    if (prefix / "include" / "dearagentkit").exists():
        raise RuntimeError("The installed package still contains obsolete public headers")
    check_dependency_licenses(prefix)
    arguments = [
        "cmake", "-S", root / "tests" / "consumer", "-B", consumer_build,
        f"-DCMAKE_BUILD_TYPE={configuration}",
        f"-DCMAKE_CONFIGURATION_TYPES={configuration}",
        f"-DCMAKE_PREFIX_PATH={prefix.as_posix()};{sdk.as_posix()}",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
    ]
    cache = source_build / "CMakeCache.txt"
    generator = cache_entry(cache, "CMAKE_GENERATOR")
    if not generator:
        raise RuntimeError(f"The source build has no recorded CMake generator: {cache}")
    arguments += ["-G", generator]
    for key, flag in (("CMAKE_GENERATOR_PLATFORM", "-A"), ("CMAKE_GENERATOR_TOOLSET", "-T")):
        value = cache_entry(cache, key)
        if value:
            arguments += [flag, value]
    # No vcpkg toolchain or manifest invocation: resolve the installed MCP package
    # and its already installed dependency SDK through explicit prefixes.
    run(arguments)
    run(["cmake", "--build", consumer_build, "--config", configuration, "--parallel", "4"])
    # Do not hide an installed library's missing RPATH with a source SDK path.
    environment = runtime_environment(prefix)
    if os.name == "nt":
        if args.linkage == "shared":
            library = "MCPd.dll" if args.preset == "debug" else "MCP.dll"
            if not (prefix / "bin" / library).is_file():
                raise RuntimeError(f"Installed shared MCP library is missing: {library}")
    result = run([executable(consumer_build, "mcp_consumer", configuration)],
                 cwd=prefix, env=environment, text=True, capture_output=True)
    if "installed MCP consumer passed" not in result.stdout:
        raise RuntimeError(f"Installed roundtrip failed: {result.stdout}")
    result = run([executable(consumer_build, "mcp_readme", configuration)],
                 cwd=prefix, env=environment, text=True, capture_output=True)
    json.loads(result.stdout)
    suffix = ".exe" if os.name == "nt" else ""
    result = run([prefix / "bin" / ("mcp" + suffix), "--version"],
                 cwd=prefix, env=environment, text=True, capture_output=True)
    if result.stdout.strip() != "MCP 0.0.1":
        raise RuntimeError(f"Unexpected installed CLI identity: {result.stdout}")
    print(f"Installed {args.linkage} {configuration} package and runtime smoke passed.")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, file=sys.stderr)
        if error.stderr:
            print(error.stderr, file=sys.stderr)
        raise
