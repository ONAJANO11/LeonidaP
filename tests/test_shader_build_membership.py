"""A refreshed shader build must publish the same shader set as a clean build."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / "tools"), str(ROOT / "tests")]
import make_game
from mc_command import mc_command
from test_fssm_unpack import make_dxbc, make_fssm, make_xbch, make_xdat
from test_shader_carve import make_rpf_entry


def compute_container(marker, terminator=0xbf810000):
    # A single-thread compute program with a distinct s_nop marker and, normally, s_endpgm.
    program = make_fssm(instructions=[
        (0xc0, struct.pack("<I", 0x100)),
        (0x128, struct.pack("<II", 0xbf800000 | marker, terminator)),
        (0x138, struct.pack("<4I", 0, 1, 1, 1)),
    ])
    dxil = make_xdat({0x0d: make_xbch(program)}, pre_padding=struct.pack("<HH", 0x66, 5))
    return make_dxbc([(b"DXIL", dxil)])


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


class ShaderBuildMembershipTest(unittest.TestCase):
    def test_loose_unpack_can_still_process_files_outside_the_carve_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for name in ("cs_current", "cs_loose"):
                (folder / (name + ".dxbc")).write_bytes(compute_container(1))
            (folder / "index.json").write_text('[{"file":"cs_current.dxbc","stage":"cs"}]', encoding="utf-8")
            cmd = [sys.executable, "-I", ROOT / "tools/fssm_unpack.py", folder]
            for args, expected in ((["--indexed"], {"cs_current"}), ([], {"cs_current", "cs_loose"})):
                with self.subTest(args=args):
                    result = subprocess.run(cmd + args, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual({row["name"] for row in load(folder / "fssm_index.json")}, expected)

    def test_missing_successful_listing_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            disasm = root / "disasm"
            disasm.mkdir()
            names = ["cs_keep", "cs_missing"]
            (root / "index.json").write_text(json.dumps([{"file": n + ".dxbc"} for n in names]), encoding="utf-8")
            success = json.dumps([{"name": n} for n in names])
            (root / "fssm_index.json").write_text(success, encoding="utf-8")
            (disasm / "selected.json").write_text(success, encoding="utf-8")
            (disasm / "cs_keep.s").touch()
            out = root / "spirv"
            result = subprocess.run([sys.executable, "-I", ROOT / "tools/rdna2spirv.py", disasm,
                                     "--out", out, "--indexed"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cs_missing.s", result.stderr)
            self.assertFalse((out / "index.json").exists())

    def test_recarve_publishes_only_current_shaders_and_preserves_loose_files(self):
        mc = mc_command()
        if not all(shutil.which(exe) for exe in (mc[0], "spirv-as", "spirv-val")):
            self.skipTest("requires llvm-mc with AMDGPU, spirv-as and spirv-val")
        if "amdgcn" not in subprocess.run(mc + ["--version"], capture_output=True, text=True).stdout:
            self.skipTest("llvm-mc lacks the AMDGPU target")

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game = root / "game"
            game.mkdir()
            keep = make_rpf_entry("cs_keep", 1, compute_container(1))
            archive = game / "shaders.rpf"
            archive.write_bytes(keep + b"\0" + make_rpf_entry("cs_retired", 1, compute_container(2)))
            work = root / "reused"
            make_game.shaders(str(game), str(work), jobs=2)
            dxbc = work / "shaders/dxbc"
            spirv = work / "shaders/spirv"
            self.assertEqual(set(load(spirv / "index.json")), {"cs_keep", "cs_retired"})

            # All three tools also support user-maintained inputs in these folders.
            loose = {}
            for folder, suffix in ((dxbc, ".dxbc"), (dxbc / "fssm", ".fssm"), (dxbc / "disasm", ".s")):
                content = (folder / ("cs_keep" + suffix)).read_bytes()
                if suffix == ".s":
                    content = b"; annotated loose shader\n" + content
                path = folder / ("cs_aaa_loose" + suffix)
                path.write_bytes(content)
                loose[path] = content

            arrived = make_rpf_entry("cs_arrived", 1, compute_container(3))
            archive.write_bytes(keep + b"\0" + arrived)
            (dxbc / "index.json").unlink()  # Request a new carve in the same work folder.
            make_game.shaders(str(game), str(work), jobs=2)
            fresh = root / "fresh"
            make_game.shaders(str(game), str(fresh), jobs=2)
            clean = load(fresh / "shaders/spirv/index.json")
            self.assertEqual(set(clean), {"cs_arrived", "cs_keep"})
            self.assertEqual(load(spirv / "index.json"), clean)
            for part, name in enumerate(sorted(clean)):
                self.assertEqual(set(load(spirv / f"index.part{part}.json")), {name})
            for path, content in loose.items():
                self.assertEqual(path.read_bytes(), content)

            index = spirv / "index.json"
            os.utime(index, (1_000_000_000, 1_000_000_000))
            mtime = index.stat().st_mtime_ns
            make_game.shaders(str(game), str(work), jobs=2)
            self.assertEqual(index.stat().st_mtime_ns, mtime)

            # The carved and unpacked shader still exists, but its new program has
            # no s_endpgm. Its previous translation must leave the runtime index.
            no_endpgm = make_rpf_entry("cs_keep", 1, compute_container(1, terminator=0xbf800000))
            archive.write_bytes(no_endpgm + b"\0" + arrived)
            (dxbc / "index.json").unlink()
            make_game.shaders(str(game), str(work), jobs=2)
            self.assertEqual(set(load(index)), {"cs_arrived"})


if __name__ == "__main__":
    unittest.main()
