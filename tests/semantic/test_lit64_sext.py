"""cs_w32_lit64: a 32-bit literal in a 64-bit integer operand, run here on the CPU.

Standard library only; no GPU, no LLVM, no game files.

RDNA2 widens a 32-bit literal by the operand's type: sign-extended where the operand is i64 (s_ashr_i64 S0,
s_bfe_i64 S0, s_flbit_i32_i64, v_ashrrev_i64 S1, v_mad_i64_i32 S2), zero-extended where it is b64/u64. Measured
natively on gfx1036 (docs/design.md, "64-bit operand literals"). runtime/test/shaders/cs_w32_lit64.asm has lane 0
of a wave32 group store one word of each (derivations in its header). This runs the fixture
(tests/semantic/regen_fixtures.py) through spirv_mini_interp, checks the eight words against values computed here,
and checks that check.py and scripts/run_gpu_tests.py expect the same words on the GPU.
"""
import os
import re
import struct
import unittest

import support
import spirv_mini_interp as si

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
M32 = 0xFFFFFFFF
M64 = (1 << 64) - 1

# 4 user SGPRs (the output V#), workgroup id in s4, 32x1x1 threads.
FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x88, 32, 1, 1)
FSSM = bytes(FSSM)
VSHARP = [0, 0, 0x1000, 0x31016FAC]


def lit64(v: int, signed: bool) -> int:
    """The 64-bit value of a 32-bit literal: sign-extended for an i64 operand, zero-extended for b64/u64."""
    v &= M32
    return (v | 0xFFFFFFFF_00000000) if signed and v >> 31 else v


def to_signed(x: int) -> int:
    return x - (1 << 64) if x >> 63 else x


def flbit_i64(x: int) -> int:
    sign = x >> 63
    for i in range(62, -1, -1):
        if (x >> i) & 1 != sign:
            return 63 - i
    return M32


def flbit_b64(x: int) -> int:
    return 64 - x.bit_length() if x else M32


EXPECTED = [
    (to_signed(lit64(0x80000000, True)) >> 4) & M32,                     # s_ashr_i64, low word
    lit64(0xFFFF0000, True) >> 32,                                        # s_bfe_i64 offset 0 width 64, high word
    flbit_i64(lit64(0xFFFF0000, True)),                                   # s_flbit_i32_i64
    lit64(0x80000000, True) >> 32,                                        # v_ashrrev_i64 by 0, high word
    ((0 * 0 + lit64(0xFFFF0000, True)) & M64) >> 32,                      # v_mad_i64_i32, high word
    ((lit64(0x80000000, False) << 4) & M64) >> 32,                        # s_lshl_b64, high word
    flbit_b64(lit64(0x80000000, False)),                                  # s_flbit_i32_b64
    (~lit64(0x80000000, False) & M64) >> 32,                              # s_not_b64, high word
]


class Lit64Fixture(unittest.TestCase):
    def test_reference(self):
        self.assertEqual(EXPECTED, [0xF8000000, 0xFFFFFFFF, 48, 0xFFFFFFFF, 0xFFFFFFFF, 8, 32, 0xFFFFFFFF])

    def test_translated_shader(self):
        spv = support.translate_fixture("cs_w32_lit64", fssm=FSSM)
        mem = si.run_workgroup(spv, 1, 32, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0),
                                                                  "%sg_lane": i},
                               push_constants=VSHARP + [0] * 28, memory={})
        self.assertEqual(sorted(mem), [4 * k for k in range(8)])
        for k, want in enumerate(EXPECTED):
            with self.subTest(word=k):
                self.assertEqual(mem[4 * k], want)

    def test_gpu_runners_expect_the_same_words(self):
        for rel in (("runtime", "test", "shaders", "check.py"), ("scripts", "run_gpu_tests.py")):
            with open(os.path.join(ROOT, *rel), encoding="utf-8") as f:
                text = f.read()
            m = re.search(r'"cs_w32_lit64"\)?[,:]\s*\[([^\]]*)\]', text)
            with self.subTest(file="/".join(rel)):
                self.assertIsNotNone(m)
                self.assertEqual([int(v, 0) for v in m.group(1).split(",")], EXPECTED)


if __name__ == "__main__":
    unittest.main()
