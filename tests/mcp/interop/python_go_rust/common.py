import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
PINS = json.loads((ROOT / "pins.json").read_text(encoding="utf-8"))


def verify_source(root, expected):
    actual = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    if actual != expected:
        raise RuntimeError(f"SDK revision mismatch: expected {expected}, got {actual}")
    changes = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=no"], text=True)
    if changes:
        raise RuntimeError("SDK checkout has tracked modifications")
