"""Read synthetic vk_pipeline_check statistic fields with the standard CSV reader."""
import csv
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    system, *command = sys.argv[1:]
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(command, cwd=tmp, check=True)
        path = Path(tmp) / "stats.csv"
        eol = b"\r\n" if system == "Windows" else b"\n"
        assert path.read_bytes().startswith(b"vs_plain,vs,Register Count,17" + eol)
        with path.open(encoding="utf-8", newline="") as f:
            rows = list(csv.reader(f, strict=True))
        assert rows == [
            ["vs_plain", "vs", "Register Count", "17"],
            ["vs_comma,variant", "ps", 'Reg "Count"', "17"],
            ["cr\ronly", "lf\nonly", "crlf\r\nonly", "17"],
            ["", " vs ", 'bytes, "quoted"\r\nnext', "17"],
        ], rows


if __name__ == "__main__":
    main()
