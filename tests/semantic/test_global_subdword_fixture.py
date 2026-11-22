"""cs_global_subdword: global_load_sbyte / _ushort, the d16 loads and the byte / d16_hi global stores, through the
translator and spirv_mini_interp.

runtime/test/shaders/cs_global_subdword.asm seeds two source dwords and thirteen store targets, runs 21 loads and 13
stores, and writes 34 words to out[0..33]. check.py's GLOBAL_SUBDWORD_WORDS holds the words derived by hand from the
RDNA2 ISA (the .asm header). Here the translated fixture runs on the CPU and must give the same words, and a plain
Python byte-memory model of the same instruction list must agree with both, so the GPU test, the hand constants and the
translation are kept in step.
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "runtime", "test", "shaders"))
sys.path.insert(0, os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools"))

import check       # noqa: E402  (runtime/test/shaders/check.py; importing it runs nothing)
import support     # noqa: E402


def fssm(rsrc2, group):
    header = bytearray(0x148)
    struct.pack_into("<4I", header, 0x138, rsrc2, *group)
    return bytes(header)


CS_FSSM = fssm(0x8088, (64, 1, 1))
OUT_VSHARP = [0, 0, 0xFFFFFFFF, 0x31016FAC]

M32 = 0xFFFFFFFF
SEEDS = {0x200: 0x8081F2A3, 0x204: 0x7F01C4E5,
         0x240: 0x44332211, 0x244: 0x88776655, 0x248: 0xCCBBAA99, 0x24C: 0x00FFEEDD,
         0x250: 0x44332211, 0x254: 0x88776655, 0x258: 0xCCBBAA99, 0x25C: 0x00FFEEDD,
         0x260: 0x44332211, 0x264: 0x88776655, 0x268: 0xCCBBAA99, 0x270: 0x44332211, 0x278: 0x13579BDF}
DATA = 0xA1B2C3D4
PRE = 0xDEADBEEF


def model():
    """The 34 words from the ISA pseudocode on a byte-addressed memory: (op, address, destination initial value)."""
    mem = bytearray(0x300)
    for a, w in SEEDS.items():
        mem[a:a + 4] = struct.pack("<I", w)

    def read(addr, n):
        return int.from_bytes(mem[addr:addr + n], "little")

    def sext(v, bits):
        return (v - (1 << bits)) if v >> (bits - 1) else v

    out = []
    # (op, address, vdst initial value)
    for op, addr, init in [
            ("sbyte", 0x200, PRE), ("sbyte", 0x203, PRE), ("sbyte", 0x206, PRE), ("sbyte", 0x207, PRE),
            ("ushort", 0x200, PRE), ("ushort", 0x202, PRE), ("ushort", 0x201, PRE), ("ushort", 0x205, PRE),
            ("ubyte_d16", 0x200, DATA), ("ubyte_d16_hi", 0x202, DATA), ("sbyte_d16", 0x203, DATA),
            ("sbyte_d16", 0x207, DATA), ("ubyte_d16_hi", 0x203, DATA), ("short_d16", 0x200, DATA),
            ("short_d16", 0x201, DATA), ("short_d16_hi", 0x204, DATA), ("short_d16_hi", 0x206, DATA),
            ("ubyte_d16", 0x205, DATA), ("ubyte_d16_hi", 0x200, DATA), ("sbyte", 0x201, PRE),
            ("ushort", 0x202, PRE)]:
        if op == "sbyte":
            out.append(sext(read(addr, 1), 8) & M32)
        elif op == "ushort":
            out.append(read(addr, 2))
        else:
            n = 2 if op.startswith("short") else 1
            v = read(addr, n)
            if op.startswith("sbyte"):
                v = sext(v, 8) & 0xFFFF
            if op.endswith("_hi"):
                out.append((init & 0xFFFF) | v << 16)
            else:
                out.append((init & 0xFFFF0000) | v)
    for op, addr in [("byte", 0x240), ("byte", 0x245), ("byte", 0x24A), ("byte", 0x24F),
                     ("byte_d16_hi", 0x250), ("byte_d16_hi", 0x255), ("byte_d16_hi", 0x25A), ("byte_d16_hi", 0x25F),
                     ("short_d16_hi", 0x260), ("short_d16_hi", 0x266), ("short_d16_hi", 0x269),
                     ("byte", 0x270 + 2 + 1), ("short_d16_hi", 0x27C - 2)]:
        n = 1 if op.startswith("byte") else 2
        v = (DATA >> 16 if op.endswith("_hi") else DATA) & ((1 << 8 * n) - 1)
        mem[addr:addr + n] = v.to_bytes(n, "little")
    out += [read(a, 4) for a in (0x240, 0x244, 0x248, 0x24C, 0x250, 0x254, 0x258, 0x25C, 0x260, 0x264, 0x268, 0x270,
                                 0x278)]
    return out


class GlobalSubdwordFixture(unittest.TestCase):
    def test_hand_constants_match_a_byte_memory_model(self):
        self.assertEqual([f"{w:#010x}" for w in model()], [f"{w:#010x}" for w in check.GLOBAL_SUBDWORD_WORDS])

    def test_translated_fixture_gives_the_hand_derived_words(self):
        spv = support.translate_fixture("cs_global_subdword", fssm=CS_FSSM)
        memory = {}
        outputs = support.run_fragment(spv, inputs={"%wg_id": (0, 0, 0), "%local_id": (0, 0, 0)},
                                       push_constants=OUT_VSHARP + [0] * 28, memory=memory)
        self.assertEqual(outputs, {})
        want = check.GLOBAL_SUBDWORD_WORDS
        for i, w in enumerate(want):
            with self.subTest(out=i):
                self.assertEqual(f"{memory.get(4 * i, 0):#010x}", f"{w:#010x}")
        # nothing else lands between out[] and the seed region
        self.assertEqual(sorted(a for a in memory if len(want) * 4 <= a < 0x200), [])


if __name__ == "__main__":
    unittest.main()
