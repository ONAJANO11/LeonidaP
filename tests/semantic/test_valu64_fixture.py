"""cs_w32_valu64_*: the GPU tests of the 64-bit VALU ops that had CPU-only checks, run here on the CPU too.

Standard library only; no GPU, no LLVM, no game files.

runtime/test/shaders/cs_w32_valu64_{shl,shr,ashr,mad_i64,mad_u64}.asm have lane 0 of a wave32 group store eight words
of v_lshlrev_b64, v_lshrrev_b64, v_ashrrev_i64 (amounts 0, 31, 32, 33, 63 and 127, so the amount is taken & 63) and
v_mad_i64_i32 / v_mad_u64_u32 (64-bit add across the dword boundary, sdst bit 64 of the 65-bit sum); the derivations
are in each header. This runs each fixture (tests/semantic/regen_fixtures.py) through spirv_mini_interp, compares
the words with the hand constants below and with a plain-Python reference, and checks that check.py and
scripts/run_gpu_tests.py expect the same words on the GPU.
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

# 4 user SGPRs (the output V#), workgroup id in s4, no LDS, 32x1x1 threads (as test_misc_ext_fixture.py).
FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x8088, 32, 1, 1)
FSSM = bytes(FSSM)
VSHARP = [0, 0, 0x1000, 0x31016FAC]


def s32(x):
    x &= M32
    return x - (1 << 32) if x >> 31 else x


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


def fold(r):
    return (r >> 32) ^ (r & M32)


X1 = 0x9ABCDEF1_F3579BDF
X2 = 0x7ABCDEF1_F3579BDF
SHL = lambda a, x: (x << (a & 63)) & M64
SHR = lambda a, x: x >> (a & 63)
SAR = lambda a, x: (s64(x) >> (a & 63)) & M64

MAD_I = [(0xFFFFFFFD, 7, 0x10), (0x7FFFFFFF, 0x7FFFFFFF, M32), (0xFFFF0000, 0xFFFF0000, M64),
         (0x80000000, 0x7FFFFFFF, 1 << 63), (M32, 0x80000000, 0x80000000)]
MAD_U = [(M32, M32, M64), (3, 5, 0x1FFFFFFFF), (0x80000000, 2, 0xFFFFFFFF80000000), (M32, 1, 0), (M32, M32, 0)]


def mad(cases, signed):
    out = []
    for a, b, c in cases:
        v = s32(a) * s32(b) + s64(c) if signed else a * b + c
        out.append((v & M64, (v >> 64) & 1))
    return out


def shift_words(op, amounts_and_inputs, picks):
    rs = [op(a, x) for a, x in amounts_and_inputs]
    sel = {"lo": lambda r: r & M32, "hi": lambda r: r >> 32, "fold": fold}
    return [sel[k](rs[i]) for i, k in picks]


STD = [(0, X1), (31, X1), (32, X1), (33, X1), (63, X1), (127, X1)]
STD_PICKS = [(0, "fold"), (1, "lo"), (1, "hi"), (2, "fold"), (3, "lo"), (3, "hi"), (4, "fold"), (5, "fold")]
_mi, _mu = mad(MAD_I, True), mad(MAD_U, False)
FLAGS = lambda m: sum(b << k for k, (_, b) in enumerate(m))

# name -> (hand constants from the .asm header, reference computed from the ISA definitions)
CASES = {
    "cs_w32_valu64_shl": ([0x69EB452E, 0x80000000, 0xF9ABCDEF, 0xF3579BDF, 0, 0xE6AF37BE, 0x80000000, 0x80000000],
                          shift_words(SHL, STD, STD_PICKS)),
    "cs_w32_valu64_shr": ([0x69EB452E, 0x3579BDE3, 1, 0x9ABCDEF1, 0x4D5E6F78, 0, 1, 1],
                          shift_words(SHR, STD, STD_PICKS)),
    "cs_w32_valu64_ashr": ([0x69EB452E, 0x3579BDE3, 0xFFFFFFFF, 0x6543210E, 0xCD5E6F78, 0x3D5E6F78, 0xFFFFFFFF, 0xFFFFFFFF],
                           shift_words(SAR, [(0, X1), (31, X1), (32, X1), (33, X1), (33, X2), (63, X1), (127, X1)],
                                       [(0, "fold"), (1, "lo"), (1, "hi"), (2, "fold"), (3, "lo"), (4, "fold"),
                                        (5, "lo"), (6, "hi")])),
    "cs_w32_valu64_mad_i64": ([0xFFFFFFFB, 0xFFFFFFFF, 0x40000000, 0xFFFFFFFF, 0x80000000, 0x40000000, 9, 1],
                              [_mi[0][0] & M32, _mi[0][0] >> 32, fold(_mi[1][0]), fold(_mi[2][0]), _mi[3][0] & M32,
                               _mi[3][0] >> 32, FLAGS(_mi), fold(_mi[4][0])]),
    "cs_w32_valu64_mad_u64": ([0, 0xFFFFFFFE, 0xE, 2, 0x80000000, 0xFFFFFFFF, 5, 0xFFFFFFFF],
                              [_mu[0][0] & M32, _mu[0][0] >> 32, _mu[1][0] & M32, _mu[1][0] >> 32, fold(_mu[2][0]),
                               fold(_mu[3][0]), FLAGS(_mu), fold(_mu[4][0])]),
}


class Valu64Fixture(unittest.TestCase):
    def test_reference(self):
        for name, (hand, ref) in CASES.items():
            with self.subTest(shader=name):
                self.assertEqual(hand, ref)

    def test_translated_shaders(self):
        for name, (hand, _) in CASES.items():
            with self.subTest(shader=name):
                spv = support.translate_fixture(name, fssm=FSSM)
                mem = si.run_workgroup(spv, 1, 32, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0),
                                                                          "%sg_lane": i},
                                       push_constants=VSHARP + [0] * 28, memory={})
                self.assertEqual(sorted(mem), [4 * k for k in range(8)])
                self.assertEqual([mem[4 * k] for k in range(8)], hand)

    def test_gpu_runners_expect_the_same_words(self):
        for rel in (("runtime", "test", "shaders", "check.py"), ("scripts", "run_gpu_tests.py")):
            with open(os.path.join(ROOT, *rel), encoding="utf-8") as f:
                text = f.read()
            for name, (hand, _) in CASES.items():
                m = re.search(r'"%s"\)?[,:]\s*\[([^\]]*)\]' % name, text)
                with self.subTest(file="/".join(rel), shader=name):
                    self.assertIsNotNone(m)
                    self.assertEqual([int(v, 0) for v in m.group(1).split(",")], hand)


if __name__ == "__main__":
    unittest.main()
