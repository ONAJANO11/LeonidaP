"""cs_w32_pk_int16_a / _b: the GPU tests of packed 16-bit integer ops, run here on the CPU too.

Standard library only; no GPU, no LLVM, no game files.

runtime/test/shaders/cs_w32_pk_int16_a.asm and _b.asm have lane 0 of a wave32 group store one word per case of
v_pk_add_i16, v_pk_sub_u16, v_pk_mad_i16, v_pk_mul_lo_u16, v_pk_max_u16, v_pk_min_i16 and v_pk_min_u16, with op_sel,
op_sel_hi, clamp and 16-bit wrap (derivations in their headers). This runs the fixtures (tests/semantic/regen_fixtures.py)
through spirv_mini_interp, checks the words against a plain-Python model of the ISA definition, and checks that
check.py and scripts/run_gpu_tests.py expect the same words on the GPU.
"""
import os
import re
import struct
import unittest

import support
import spirv_mini_interp as si

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# 4 user SGPRs (the output V#), workgroup id in s4, 512 bytes of LDS (LDS_SIZE 1), 32x1x1 threads.
FSSM = bytearray(0x148)
struct.pack_into("<4I", FSSM, 0x138, 0x8088, 32, 1, 1)
FSSM = bytes(FSSM)
VSHARP = [0, 0, 0x1000, 0x31016FAC]


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >> 15 else x


def pk(kind, signed, srcs, sel=(0, 0, 0), selhi=(1, 1, 1), clamp=False):
    """One packed 16-bit op: result half 0 reads source i's half sel[i], half 1 reads selhi[i]."""
    out = 0
    for h, picks in enumerate((sel, selhi)):
        v = [(srcs[i] >> (16 * picks[i])) & 0xFFFF for i in range(len(srcs))]
        if signed:
            v = [s16(x) for x in v]
        r = {"add": lambda: v[0] + v[1], "sub": lambda: v[0] - v[1], "mul": lambda: v[0] * v[1],
             "mad": lambda: v[0] * v[1] + v[2], "max": lambda: max(v[0], v[1]), "min": lambda: min(v[0], v[1])}[kind]()
        if clamp:
            r = max(-32768, min(32767, r)) if signed else max(0, min(0xFFFF, r))
        out |= (r & 0xFFFF) << (16 * h)
    return out


EXPECTED_A = [
    pk("add", True, (0x7FFF8000, 0x00010001)),
    pk("add", True, (0x7FFF8000, 0x0001FFFF), clamp=True),
    pk("sub", False, (0x00100000, 0x00020001)),
    pk("sub", False, (0x00100000, 0x00020001), clamp=True),
    pk("mad", True, (0xFFFE0003, 0x0004FFFD, 0x00010001)),
    pk("mad", True, (0xFFFE0003, 0x0004FFFD, 0x00640032), sel=(1, 0, 1), selhi=(0, 1, 0)),
    pk("mul", False, (0xFFFF8001, 0xFFFF0002)),
    pk("mul", False, (0x00030005, 0x00070002), sel=(0, 1), selhi=(1, 0)),
]
EXPECTED_B = [
    pk("max", False, (0x7FFF8000, 0x8000FFFF)),
    pk("max", False, (0xC0000001, 0x00020003), sel=(1, 0), selhi=(0, 1)),
    pk("min", True, (0x80018000, 0x7FFE7FFF)),
    pk("min", False, (0x80018000, 0x7FFE7FFF)),
    pk("min", True, (0x80018000, 0x7FFE7FFF), sel=(1, 1)),
    pk("min", False, (0x80018000, 0x7FFE7FFF), selhi=(0, 0)),
    pk("add", True, (0x7FFE0003, 0x0003FFFF), sel=(1, 1), selhi=(0, 0)),
    pk("sub", False, (0x00050009, 0x00020003), sel=(1, 0), selhi=(0, 1)),
]
HAND = {
    "cs_w32_pk_int16_a": [0x80008001, 0x7FFF8000, 0x000EFFFF, 0x000E0000, 0xFFF9FFF8, 0x003E006A, 0x00010002, 0x00060023],
    "cs_w32_pk_int16_b": [0x8000FFFF, 0x0002C000, 0x80018000, 0x7FFE7FFF, 0x80018001, 0x7FFF7FFF, 0x00028001, 0x00070002],
}
MODEL = {"cs_w32_pk_int16_a": EXPECTED_A, "cs_w32_pk_int16_b": EXPECTED_B}


class PkInt16Fixture(unittest.TestCase):
    def test_model_matches_hand_derived_words(self):
        for name, want in HAND.items():
            with self.subTest(name=name):
                self.assertEqual(MODEL[name], want)

    def test_translated_shaders(self):
        for name, want in HAND.items():
            spv = support.translate_fixture(name, fssm=FSSM)
            mem = si.run_workgroup(spv, 1, 32, inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0),
                                                                      "%sg_lane": i},
                                   push_constants=VSHARP + [0] * 28, memory={})
            self.assertEqual(sorted(mem), [4 * k for k in range(8)])
            for k, w in enumerate(want):
                with self.subTest(name=name, word=k):
                    self.assertEqual(mem[4 * k], w)

    def test_gpu_runners_expect_the_same_words(self):
        for rel in (("runtime", "test", "shaders", "check.py"), ("scripts", "run_gpu_tests.py")):
            with open(os.path.join(ROOT, *rel), encoding="utf-8") as f:
                text = f.read()
            for name, want in HAND.items():
                m = re.search(r'"%s"\)?[,:]\s*\[([^\]]*)\]' % name, text)
                with self.subTest(file="/".join(rel), name=name):
                    self.assertIsNotNone(m)
                    self.assertEqual([int(v, 0) for v in m.group(1).split(",")], want)


if __name__ == "__main__":
    unittest.main()
