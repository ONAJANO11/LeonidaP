"""tools/_paths.py: the one shared `same_report_path` the report-writing tools use (BUILD-589).

    python -I tests/test_paths.py

Temporary files only; no game data.
"""
from __future__ import annotations

import importlib.util
import os
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import _paths  # noqa: E402

# Tools that take the helper from _paths instead of keeping a copy, and the name they use for it.
USERS = {
    "anim_index": "same_report_path",
    "awc_manifest": "_same_report_path",
    "bvh_index": "same_output_path",
    "gamedata_index": "same_report_path",
    "gamedata_xml": "same_report_path",
    "navmesh_decode": "same_report_path",
    "navmesh_index": "same_report_path",
    "rpf_probe": "_same_report_path",
    "tile_probe": "same_report_path",
    "world_lights_paths": "same_report_path",
    "world_names": "_same_report_path",
}


class SameReportPathTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
        self.a = self.dir / "a.json"
        self.a.write_text("{}")

    def tearDown(self):
        self._tmp.cleanup()

    def test_one_file_through_different_spellings(self):
        same = _paths.same_report_path
        self.assertTrue(same(str(self.a), str(self.a)))
        self.assertTrue(same(str(self.a), str(self.dir / "sub" / ".." / "a.json")))
        self.assertTrue(same(str(self.a), os.path.relpath(self.a)))

    def test_different_files_and_missing_files(self):
        same = _paths.same_report_path
        self.assertFalse(same(str(self.a), str(self.dir / "b.json")))
        self.assertFalse(same(str(self.dir / "x.json"), str(self.dir / "y.json")))   # neither exists
        self.assertTrue(same(str(self.dir / "x.json"), str(self.dir / "x.json")))    # one missing path

    def test_hard_link_is_the_same_file(self):
        link = self.dir / "link.json"
        try:
            os.link(self.a, link)
        except (OSError, NotImplementedError) as e:
            self.skipTest(f"no hard links here: {e}")
        self.assertTrue(_paths.same_report_path(str(self.a), str(link)))

    def test_case_follows_the_volume(self):
        probe = self.dir / "Probe"
        probe.touch()
        insensitive = (self.dir / "probe").exists()
        self.assertEqual(_paths.same_report_path(str(self.dir / "R.json"), str(self.dir / "r.json")), insensitive)

    def test_tools_use_the_shared_helper(self):
        for tool, name in USERS.items():
            with self.subTest(tool=tool):
                spec = importlib.util.spec_from_file_location(f"paths_{tool}", TOOLS / f"{tool}.py")
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                self.assertIs(getattr(module, name), _paths.same_report_path)


if __name__ == "__main__":
    unittest.main()
