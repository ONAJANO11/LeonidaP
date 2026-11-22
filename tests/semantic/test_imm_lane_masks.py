"""Immediates used as lane masks and as 64-bit data: runtime/test/shaders/cs_imm_mask.asm across 64 lanes.

The GPU check's words (check.py CS_DUMPS, derived in the .asm header from the RDNA2 ISA: an inline constant is
sign-extended to 64 bits) must come back on one 64-wide subgroup and on two 32-wide ones: EXEC is set from each lane's
own bit of the constant, with the lane from LocalInvocationIndex, and the counts read the constant as data, so no
instruction here crosses lanes. Until 2026-11-22 every lane took bit 0 of an immediate mask and the high word of a
64-bit immediate read as 0.
"""
import os
import struct
import sys
import unittest

import support
import spirv_mini_interp as si

sys.path.insert(0, os.path.join(support.ROOT, "runtime", "test", "shaders"))
import check  # noqa: E402
from compute_fixture import compute_header  # noqa: E402

NAME = "cs_imm_mask"
WANT = dict(check.CS_DUMPS)[NAME]
VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]       # as in test_wave64_subgroup_ops.py


def run(subgroup_size):
    header = compute_header(os.path.join(support.ROOT, "runtime", "test", "shaders", NAME + ".asm"))
    threads = struct.unpack_from("<I", header, 0x13C)[0]
    spv = support.translate_fixture(NAME, fssm=bytes(header))
    memory = si.run_workgroup(
        spv, threads, subgroup_size,
        inputs_for=lambda i: {"%wg_id": (0, 0, 0), "%local_id": (i, 0, 0), "%sg_lane": i % subgroup_size},
        push_constants=VSHARP + [0] * 28, memory={})
    return [memory.get(4 * k) for k in range(len(WANT))]


class ImmediateLaneMasks(unittest.TestCase):
    def test_words_equal_the_gpu_check(self):
        self.assertEqual(WANT, [1, 100, 111, 10, 60, 0, 0xFFFFFFFF, 64])
        for size in (64, 32):
            with self.subTest(subgroup_size=size):
                self.assertEqual(run(size), WANT)


if __name__ == "__main__":
    unittest.main()
