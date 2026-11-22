"""Every tests/semantic/test_*.py module imports cleanly and exposes unittest CLI under `python -I`.

    python -I tests/test_semantic_isolated_cli.py

With `-I` (isolated mode), Python does not put the script's directory on `sys.path`, so semantic tests that import
sibling helpers (`support`, `spirv_mini_interp`, etc.) fail at import time unless they add their own directory to
`sys.path`. Invoking each file with `--help` checks import + `unittest.main()` entry in a few seconds without
re-running the full semantic suite inside the stdlib CI job.
"""
import concurrent.futures
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEMANTIC_DIR = os.path.join(ROOT, "tests", "semantic")


def semantic_test_files():
    return sorted(
        os.path.join(SEMANTIC_DIR, name)
        for name in os.listdir(SEMANTIC_DIR)
        if name.startswith("test_") and name.endswith(".py")
    )


def _probe_help(path, cwd):
    r = subprocess.run(
        [sys.executable, "-I", path, "--help"],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=60,
    )
    return path, r.returncode, r.stdout, r.stderr


class SemanticIsolatedCliTests(unittest.TestCase):
    def test_all_semantic_tests_accept_help_under_isolated_python(self):
        files = semantic_test_files()
        self.assertGreater(len(files), 0)
        failures = []
        with tempfile.TemporaryDirectory() as tmp:
            with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 4)) as ex:
                for path, rc, out, err in ex.map(lambda p: _probe_help(p, tmp), files):
                    name = os.path.basename(path)
                    if rc != 0 or "usage:" not in out.lower():
                        failures.append(f"{name}: rc={rc}\n{err.strip() or out.strip()}")
        self.assertEqual(failures, [], "\n\n".join(failures))


if __name__ == "__main__":
    unittest.main()
