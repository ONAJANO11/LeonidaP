"""Semantic interpreter tests for PS `exp mrtz` depth, stencil and sample-mask exports (SHADER-224).

On RDNA2/GFX10, MRTZ exports carry (R=Z, G=stencil, B=sample mask, A=alpha):
  - Uncompressed (SPI_SHADER_32_R / 32_GR / 32_ABGR): x = f32 depth, y[7:0] = stencil ref, z = sample mask.
  - Compressed (`compr` = SPI_SHADER_UINT16_ABGR): never carries Z; y holds (x, y) with stencil in bits [23:16],
    and z holds (z, w) with sample mask in bits [15:0].

Executes translated SPIR-V directly through spirv_mini_interp (no GPU required) and checks exact output words,
inactive-lane EXEC masking, conditional-branch sample-mask preservation (0xFFFFFFFF default), shader_meta flags,
and spirv-val validation when available.
"""
import gc
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
if HERE not in sys.path:
    sys.path.insert(0, HERE)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
HAVE_SPIRV_TOOLS = bool(shutil.which("spirv-as") and shutil.which("spirv-val"))


def _translate_ps(name, body_lines, with_enc=False):
    """Translate a hand-written PS listing and return (spvasm, meta)."""
    suf = " ; encoding: [0x00,0x00,0x00,0x00]" if with_enc else ""
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        s_path = os.path.join(tmp, "disasm", name + ".s")
        with open(s_path, "w", encoding="utf-8") as f:
            f.write(f"; {name}\n")
            for ln in body_lines:
                f.write(f"\t{ln}{suf}\n")
            f.write(f"\ts_endpgm{suf}\n")
        with open(os.path.join(tmp, name + ".dxbc"), "wb") as f:
            f.write(STUB_DXBC)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(s_path)
            gen = rdna2spirv.translate.last_gen
            gc.collect()
        meta = rdna2spirv.shader_meta(gen, spv)
    if HAVE_SPIRV_TOOLS:
        with tempfile.TemporaryDirectory() as tmp:
            spv_bin = os.path.join(tmp, name + ".spv")
            r = subprocess.run(["spirv-as", "--target-env", "vulkan1.2", "-o", spv_bin, "-"],
                               input=spv, capture_output=True, text=True)
            assert r.returncode == 0, f"spirv-as failed for {name}:\n{r.stderr}"
            r = subprocess.run(["spirv-val", "--target-env", "vulkan1.2", spv_bin],
                               capture_output=True, text=True)
            assert r.returncode == 0, f"spirv-val failed for {name}:\n{r.stderr}"
    return spv, meta


class MrtzExportSemanticTests(unittest.TestCase):
    def test_uncompressed_depth_stencil_and_sample_mask_values(self):
        cases = [
            (0x3EC00000, 0xDEADBE5A, 0x0000000A, 0x5A, 0x0000000A),   # dirty bits [31:8] stripped
            (0x3F000000, 0xFFFF0000, 0xFFFF000F, 0x00, 0xFFFF000F),   # low byte 0, full 32-bit mask preserved
            (0x3E800000, 0x123456FF, 0x00000000, 0xFF, 0x00000000),   # max stencil 255, zero mask
        ]
        for depth_u, stencil_raw, mask_raw, want_s, want_m in cases:
            with self.subTest(stencil=hex(stencil_raw), mask=hex(mask_raw)):
                spv, meta = _translate_ps("ps_mrtz_u", [
                    f"v_mov_b32 v2, {depth_u:#x}",
                    f"v_mov_b32 v3, {stencil_raw:#x}",
                    f"v_mov_b32 v4, {mask_raw:#x}",
                    "exp mrtz v2, v3, v4, off vm",
                    "v_mov_b32 v5, 1.0",
                    "exp mrt0 v5, v5, v5, v5 done vm",
                ])
                out = si.run_fragment(spv)
                self.assertEqual(out["%out_depth"], (depth_u,))
                self.assertEqual(out["%out_stencil"], (want_s,))
                self.assertEqual(out["%out_sample_mask"], (want_m,))
                self.assertTrue(meta["writes_depth"])
                self.assertTrue(meta["writes_stencil"])
                self.assertTrue(meta["writes_sample_mask"])
                self.assertIn("shaderStencilExport", meta["features"])

    def test_compressed_stencil_and_sample_mask_values(self):
        cases = [
            (0x5AA5BEEF, 0xABCD1234, 0xA5, 0x1234),
            (0xFF00FFFF, 0xFFFF0000, 0x00, 0x0000),
            (0x00FF0000, 0xDEADFFFF, 0xFF, 0xFFFF),
        ]
        for v_xy, v_zw, want_s, want_m in cases:
            with self.subTest(v_xy=hex(v_xy), v_zw=hex(v_zw)):
                spv, meta = _translate_ps("ps_mrtz_c", [
                    f"v_mov_b32 v2, {v_xy:#x}",
                    f"v_mov_b32 v3, {v_zw:#x}",
                    "exp mrtz v2, v2, v3, v3 compr vm",
                    "v_mov_b32 v5, 1.0",
                    "exp mrt0 v5, v5, v5, v5 done vm",
                ])
                out = si.run_fragment(spv)
                self.assertNotIn("%out_depth", out)
                self.assertEqual(out["%out_stencil"], (want_s,))
                self.assertEqual(out["%out_sample_mask"], (want_m,))
                self.assertFalse(meta.get("writes_depth"))
                self.assertTrue(meta["writes_stencil"])
                self.assertTrue(meta["writes_sample_mask"])
                self.assertIn("shaderStencilExport", meta["features"])

    def test_compressed_single_channel_and_disassembly_en_masks(self):
        # en=0x2 (stencil only in slot y), en=0x4 (mask only in slot z), en=0x6 (both), en=0x8 (alpha only).
        spv, meta = _translate_ps("ps_mrtz_en2", [
            "v_mov_b32 v2, 0x77c31122",
            "exp mrtz off, v2, off, off compr vm",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ])
        out = si.run_fragment(spv)
        self.assertEqual(out["%out_stencil"], (0xC3,))
        self.assertNotIn("%out_sample_mask", out)
        self.assertNotIn("%out_depth", out)
        self.assertTrue(meta["writes_stencil"])
        self.assertFalse(meta.get("writes_sample_mask"))

        spv, meta = _translate_ps("ps_mrtz_en4", [
            "v_mov_b32 v3, 0xdead000a",
            "exp mrtz off, off, v3, off compr vm",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ])
        out = si.run_fragment(spv)
        self.assertEqual(out["%out_sample_mask"], (0x000A,))
        self.assertNotIn("%out_stencil", out)
        self.assertNotIn("StencilExportEXT", spv)
        self.assertNotIn("SPV_EXT_shader_stencil_export", spv)
        self.assertNotIn("shaderStencilExport", meta["features"])
        self.assertTrue(meta["writes_sample_mask"])

        spv, meta = _translate_ps("ps_mrtz_en6", [
            "v_mov_b32 v2, 0x11423344",
            "v_mov_b32 v3, 0x99880005",
            "exp mrtz off, v2, v3, off compr vm",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ])
        out = si.run_fragment(spv)
        self.assertEqual(out["%out_stencil"], (0x42,))
        self.assertEqual(out["%out_sample_mask"], (0x0005,))

        spv, meta = _translate_ps("ps_mrtz_en8", [
            "v_mov_b32 v3, 0x3f800000",
            "exp mrtz off, off, off, v3 compr vm",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ])
        out = si.run_fragment(spv)
        self.assertNotIn("%out_depth", out)
        self.assertNotIn("%out_stencil", out)
        self.assertNotIn("%out_sample_mask", out)
        self.assertNotIn("StencilExportEXT", spv)

    def test_inactive_lane_does_not_overwrite_mrtz(self):
        # First mrtz writes (0.25, 0x33, 0x5) while active; second runs with exec_lo = 0 and must not overwrite it.
        spv, _ = _translate_ps("ps_mrtz_exec", [
            "v_mov_b32 v2, 0x3e800000",
            "v_mov_b32 v3, 0x33",
            "v_mov_b32 v4, 0x5",
            "exp mrtz v2, v3, v4, off vm",
            "s_mov_b32 exec_lo, 0",
            "v_mov_b32 v6, 0x3f400000",
            "v_mov_b32 v7, 0x99",
            "v_mov_b32 v8, 0xa",
            "exp mrtz v6, v7, v8, off vm",
            "s_mov_b32 exec_lo, -1",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ])
        out = si.run_fragment(spv)
        self.assertEqual(out["%out_depth"], (0x3E800000,))
        self.assertEqual(out["%out_stencil"], (0x33,))
        self.assertEqual(out["%out_sample_mask"], (0x5,))

    def test_skipped_conditional_sample_mask_keeps_full_coverage(self):
        # When a branch bypasses a conditional mrtz sample-mask export, entry-initialized 0xFFFFFFFF keeps full coverage.
        spv, meta = _translate_ps("ps_mrtz_cond_smask", [
            "s_cmp_eq_u32 s0, 0",
            "s_cbranch_scc1 2",              # skip the 2 synthetic 1-dword instructions below
            "v_mov_b32 v4, 0x5",
            "exp mrtz off, off, v4, off vm",
            "v_mov_b32 v5, 1.0",
            "exp mrt0 v5, v5, v5, v5 done vm",
        ], with_enc=True)
        self.assertNotIn("StencilExportEXT", spv)
        self.assertTrue(meta["writes_sample_mask"])
        out_skip = si.run_fragment(spv, push_constants=[0] * 32)
        out_take = si.run_fragment(spv, push_constants=[1] + [0] * 31)
        self.assertEqual(out_skip["%out_sample_mask"], (0xFFFFFFFF,))
        self.assertEqual(out_take["%out_sample_mask"], (0x5,))


if __name__ == "__main__":
    unittest.main()
