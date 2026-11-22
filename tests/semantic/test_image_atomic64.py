"""RDNA2 §8.2 MIMG masks, image types, and wide atomic opcode selection."""
import re
import struct
import unittest
from pathlib import Path

from support import FIXTURE_DIR, rdna2spirv, translate_lines


def translate(lines):
    fssm = bytearray(0x148)
    struct.pack_into("<4I", fssm, 0x138, 0x4, 32, 1, 1)
    return translate_lines("cs_image_atomic64", lines, fssm=fssm)


class ImageAtomic64(unittest.TestCase):
    def test_masks_select_types_and_atomic_operations(self):
        source = Path(FIXTURE_DIR, "cs_image_atomic64.asm").read_text(encoding="utf-8")
        spv = translate(source.splitlines())
        self.assertIn('OpExtension "SPV_EXT_shader_image_int64"', spv)
        self.assertIn("OpCapability Int64ImageEXT", spv)
        self.assertIn("OpCapability Int64Atomics", spv)
        for binding, arrayed, signed in ((26, 0, 0), (27, 0, 1), (28, 1, 0), (29, 1, 1)):
            name = f"atomic64_{arrayed}_{signed}"
            self.assertIn(f"OpDecorate %{name} Binding {binding}", spv)
            image_type = "%long" if signed else "%ulong"
            fmt = "R64i" if signed else "R64ui"
            self.assertIn(f"%img_{name} = OpTypeImage {image_type} 2D 0 {arrayed} 0 2 {fmt}", spv)
        self.assertEqual(re.findall(r"= (OpAtomic\w+) (%\w+)", spv), [
            ("OpAtomicIAdd", "%uint"), ("OpAtomicCompareExchange", "%uint"),
            ("OpAtomicIAdd", "%ulong"), ("OpAtomicISub", "%ulong"), ("OpAtomicAnd", "%ulong"),
            ("OpAtomicOr", "%ulong"), ("OpAtomicXor", "%ulong"), ("OpAtomicUMin", "%ulong"),
            ("OpAtomicUMax", "%ulong"), ("OpAtomicSMin", "%long"), ("OpAtomicSMax", "%long"),
            ("OpAtomicExchange", "%ulong"), ("OpAtomicCompareExchange", "%ulong"),
        ])
        # Only the three wide glc forms unpack the returned pair.
        self.assertEqual(spv.count("OpImageTexelPointer %ptr_image_"), 11)
        self.assertEqual(len(re.findall(r"OpBitcast %atomic64_words ", spv)), 3)

    def test_narrow_cmpswap_uses_32_bits(self):
        spv = translate(["image_atomic_cmpswap v[0:1], v[8:9], s[0:7] dmask:3 dim:SQ_RSRC_IMG_2D",
                         "s_endpgm"])
        self.assertIn("OpAtomicCompareExchange %uint", spv)
        self.assertNotIn("Int64ImageEXT", spv)

    def test_rejects_unsupported_width_operands_and_dimensions(self):
        cases = [
            ("image_atomic_inc v[0:1], v[8:9], s[0:7] dmask:3", "image_atomic_inc 64-bit"),
            ("image_atomic_add v0, v[8:9], s[0:7] dmask:3", "64-bit operands"),
            ("image_atomic_cmpswap v[0:1], v[8:9], s[0:7] dmask:0xf", "64-bit operands"),
            ("image_atomic_add v[0:1], v[8:10], s[0:7] dmask:3 dim:SQ_RSRC_IMG_3D", "SQ_RSRC_IMG_3D"),
            ("image_atomic_add v[0:1], v[8:9], s[0:7] dmask:3 a16", "or modifier"),
            ("image_atomic_add v0, v[8:9], s[0:7] dmask:2", "dmask:0x2"),
        ]
        for ins, message in cases:
            with self.subTest(ins=ins), self.assertRaisesRegex(rdna2spirv.Unsupported, message):
                translate([ins, "s_endpgm"])
