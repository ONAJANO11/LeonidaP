"""Exercise the pipeline checker's index errors before Vulkan initialization."""

import subprocess
import sys
import tempfile
from pathlib import Path


def main(command):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        folder = root / "translated 地形 🎮"
        folder.mkdir()
        index = folder / "index.json"
        cache = root / "previous-cache.bin"
        original = b"previous checkpoint"
        cache.write_bytes(original)

        for case in ("missing", "directory", "malformed"):
            if case == "directory":
                index.mkdir()
            elif case == "malformed":
                index.rmdir()
                index.write_text("[", encoding="utf-8")
            result = subprocess.run([*command, folder.name, cache.name], cwd=root,
                                    capture_output=True, encoding="utf-8", errors="replace")
            if result.returncode != 2 or folder.name not in result.stderr or "index.json" not in result.stderr:
                raise SystemExit(f"Expected an input error for the {case} Unicode index, got {result.returncode}:\n"
                                 + result.stderr)
            message = "malformed JSON" if case == "malformed" else "cannot read"
            if message not in result.stderr:
                raise SystemExit(f"Unexpected diagnostic for the {case} index:\n" + result.stderr)
            if cache.read_bytes() != original:
                raise SystemExit("An unreadable index replaced the previous pipeline cache")


if __name__ == "__main__":
    main(sys.argv[1:])
