"""The committed fixtures are exactly what tests/semantic/regen_fixtures.py generates from the .asm sources."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import regen_fixtures as rf  # noqa: E402


class FixtureTests(unittest.TestCase):
    def test_fixtures_match_asm_sources(self):
        for name in rf.SHADERS:
            with self.subTest(name=name):
                path = os.path.join(rf.FIXTURE_DIR, name + ".s")
                self.assertTrue(os.path.isfile(path), "run tests/semantic/regen_fixtures.py")
                with open(path, encoding="utf-8", newline="") as f:
                    have = f.read().replace("\r\n", "\n")
                self.assertEqual(have, rf.render(name))


if __name__ == "__main__":
    unittest.main()
