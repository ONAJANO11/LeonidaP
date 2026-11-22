"""Semantic tests for v_cvt_pk_u8_f32 including NaN inputs (docs/design.md)."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import spirv_mini_interp as si  # noqa: E402
import support  # noqa: E402

PUSH = [0x1000, 0, 16, 0] + [0] * 28


class CvtPkU8F32Tests(unittest.TestCase):
    def _run(self, src_word, byte_idx=1, base=0xAABBCCDD, mods=""):
        spv = support.translate_lines("ps_cvt_pk_u8", [
            "s_buffer_load_dwordx4 s[4:7], s[0:3], 0",
            "s_waitcnt lgkmcnt(0)",
            "v_mov_b32_e32 v1, s4",
            "v_mov_b32_e32 v2, s5",
            "v_mov_b32_e32 v3, s6",
            f"v_cvt_pk_u8_f32 v3, {mods}v1, v2, v3",
            "exp mrt0 v3, v3, v3, v3 done vm",
            "s_endpgm",
        ])
        mem = {0x1000: src_word, 0x1004: byte_idx, 0x1008: base}
        return spv, si.run_fragment(spv, push_constants=PUSH, memory=mem)["%out_mrt0"][0]

    def test_nan_infinities_negatives_and_ties(self):
        cases = (
            # (src_word, byte_idx, base, mods, want)
            (0x7FC00000, 1, 0xAABBCCDD, "", 0xAABB00DD),    # +qNaN -> 0
            (0xFFC00000, 2, 0x11223344, "", 0x11003344),    # -qNaN -> 0
            (0x7F800001, 0, 0xAABBCCDD, "", 0xAABBCC00),    # +sNaN -> 0
            (0xFF800001, 3, 0x11223344, "", 0x00223344),    # -sNaN -> 0
            (0xFF800000, 1, 0xAABBCCDD, "", 0xAABB00DD),    # -inf -> 0
            (0x7F800000, 1, 0xAABBCCDD, "", 0xAABBFFDD),    # +inf -> 255
            (0x80000000, 1, 0xAABBCCDD, "", 0xAABB00DD),    # -0.0 -> 0
            (0xBF800000, 1, 0xAABBCCDD, "", 0xAABB00DD),    # -1.0 -> 0
            (0x41480000, 1, 0xAABBCCDD, "", 0xAABB0CDD),    # 12.5 -> tie to even 12
            (0x41580000, 1, 0xAABBCCDD, "", 0xAABB0EDD),    # 13.5 -> tie to even 14
            (0x437F6666, 1, 0xAABBCCDD, "", 0xAABBFFDD),    # 255.4 -> clamped to 255
            (0x43800000, 1, 0xAABBCCDD, "", 0xAABBFFDD),    # 256.0 -> 255
            (0xC1400000, 5, 0x11223344, "-", 0x11220C44),   # -(-12.0), byte 5 & 3 = 1
        )
        for word, byte_idx, base, mods, want in cases:
            with self.subTest(word=f"{word:#010x}", byte=byte_idx, mods=mods):
                _, got = self._run(word, byte_idx, base, mods)
                self.assertEqual(f"{got:#010x}", f"{want:#010x}")

    def test_uses_nmax_nmin_not_fclamp_and_detects_sabotage(self):
        spv, got = self._run(0x7FC00000)
        self.assertEqual(got, 0xAABB00DD)
        self.assertIn(" NMax ", spv)
        self.assertIn(" NMin ", spv)
        self.assertNotIn(" FClamp ", spv)
        # Sabotaging NMax back to FMin / reverting NaN handling fails under the mini-interpreter.
        bad = re.sub(
            r"^\s*(%\w+) = OpExtInst %float %glsl NMax (%\w+) %f0\n"
            r"(\s*%\w+ = OpBitcast %float %c437f0000\n)"
            r"\s*(%\w+) = OpExtInst %float %glsl NMin \1 (%\w+)$",
            r"\3         \4 = OpExtInst %float %glsl FClamp \2 %f0 \5",
            spv,
            count=1,
            flags=re.M,
        )
        self.assertNotEqual(bad, spv)
        with self.assertRaises(si.UnsupportedConstruct):
            si.run_fragment(bad, push_constants=PUSH, memory={0x1000: 0x7FC00000, 0x1004: 1, 0x1008: 0xAABBCCDD})


if __name__ == "__main__":
    unittest.main()
