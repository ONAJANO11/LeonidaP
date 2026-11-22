"""vk_pipeline_check: a --mesh-compute mesh entry is created as a compute pipeline, not skipped.

    python -I tests/test_vk_pipeline_check_mesh_compute.py

rdna2spirv.py --mesh-compute emits a mesh shader as a GLCompute module and records its buffer layout under
"mesh_compute" in index.json. vk_pipeline_check used to skip every such entry as "no VK_EXT_mesh_shader" on the GPUs
the flag exists for (MoltenVK has no mesh shaders), so the pipelines stage never created a third of the corpus there.
The module here is a hand-built empty compute shader; no game data. Needs vk_pipeline_check and a Vulkan device, and
skips without either. Set LEONIDA_VK_PIPELINE_CHECK to use a binary elsewhere.
"""
import json
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def find_binary():
    env = os.environ.get("LEONIDA_VK_PIPELINE_CHECK")
    if env:
        return Path(env) if Path(env).is_file() else None
    build = ROOT / "runtime" / "build"
    for p in (build / "linux" / "vk_pipeline_check", build / "vk_pipeline_check", build / "Release" / "vk_pipeline_check.exe"):
        if p.is_file():
            return p
    return None


def empty_compute_module(local_x=128):
    """SPIR-V 1.0: OpEntryPoint GLCompute "main", LocalSize local_x 1 1, a function that returns."""
    words = [0x07230203, 0x00010000, 0, 5, 0,
             (2 << 16) | 17, 1,                                   # OpCapability Shader
             (3 << 16) | 14, 0, 1,                                # OpMemoryModel Logical GLSL450
             (5 << 16) | 15, 5, 1, 0x6E69616D, 0,                 # OpEntryPoint GLCompute %1 "main"
             (6 << 16) | 16, 1, 17, local_x, 1, 1,                # OpExecutionMode %1 LocalSize
             (2 << 16) | 19, 2,                                   # %2 = OpTypeVoid
             (3 << 16) | 33, 3, 2,                                # %3 = OpTypeFunction %2
             (5 << 16) | 54, 2, 1, 0, 3,                          # %1 = OpFunction %2 None %3
             (2 << 16) | 248, 4,                                  # %4 = OpLabel
             (1 << 16) | 253,                                     # OpReturn
             (1 << 16) | 56]                                      # OpFunctionEnd
    return struct.pack(f"<{len(words)}I", *words)


# What rdna2spirv.py writes for a --mesh-compute mesh shader: the buffer layout, not a bool.
LAYOUT = {"slot_bytes": 12304, "vertex_stride": 80, "primitive_stride": 16, "max_vertices": 128,
          "max_primitives": 128}


class MeshComputeEntries(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = find_binary()
        if cls.exe is None:
            raise unittest.SkipTest("vk_pipeline_check isn't built (cmake -S runtime -B runtime/build/linux)")

    def run_tool(self, index, *args):
        with tempfile.TemporaryDirectory() as d:
            for name in index:
                Path(d, name + ".spv").write_bytes(empty_compute_module())
            Path(d, "index.json").write_text(json.dumps(index), encoding="utf-8")
            r = subprocess.run([str(self.exe), d, "--jobs", "1", *args], capture_output=True, text=True, timeout=300)
        if "device:" not in r.stderr + r.stdout:
            self.skipTest("no Vulkan device: " + (r.stderr.strip().splitlines() or ["(no output)"])[-1])
        return r

    def test_mesh_compute_entry_is_a_compute_pipeline(self):
        r = self.run_tool({"mc": {"stage": "ms", "mesh_compute": LAYOUT, "subgroup_size": None}})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("ms-compute: 1 modules ok, 0 failed", r.stdout)
        self.assertIn("ms-compute pipelines: 1 ok, 0 failed, 0 skipped", r.stdout)
        self.assertNotIn("no VK_EXT_mesh_shader", r.stdout)

    def test_counted_apart_from_compute_shaders(self):
        r = self.run_tool({"cs": {"stage": "cs", "subgroup_size": None},
                           "mc": {"stage": "ms", "mesh_compute": LAYOUT, "subgroup_size": None}})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("cs pipelines: 1 ok, 0 failed, 0 skipped", r.stdout)
        self.assertIn("ms-compute pipelines: 1 ok, 0 failed, 0 skipped", r.stdout)

    def test_compute_only_includes_it_and_graphics_only_leaves_it_out(self):
        index = {"mc": {"stage": "ms", "mesh_compute": LAYOUT, "subgroup_size": None}}
        r = self.run_tool(index, "--compute-only")
        self.assertIn("ms-compute pipelines: 1 ok", r.stdout)
        r = self.run_tool(index, "--graphics-only")
        self.assertIn("checking 0 shaders", r.stdout + r.stderr)

    def test_bool_and_null_forms(self):
        r = self.run_tool({"mc": {"stage": "ms", "mesh_compute": True, "subgroup_size": None}})
        self.assertIn("ms-compute pipelines: 1 ok", r.stdout)
        # Absent or null: a real mesh shader as before (created with VK_EXT_mesh_shader, or skipped without it),
        # never counted as ms-compute. An empty compute module isn't a valid mesh shader, so only the label is checked.
        for value in (None, False):
            with self.subTest(mesh_compute=value):
                r = self.run_tool({"m": {"stage": "ms", "mesh_compute": value}})
                self.assertNotIn("ms-compute", r.stdout)


if __name__ == "__main__":
    unittest.main()
