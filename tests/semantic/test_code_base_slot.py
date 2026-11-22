"""Per-stage code-base slots (SHADER-67, docs/d3d12_vulkan.md section 5.2, open question 14).

s_getpc_b64 becomes code base + next-instruction offset. A pixel shader reads its code base from push-constant dwords
38-39 (a 160-byte block), every other stage from 34-35 (144 bytes), so the VS or MS and the PS of one pipeline can
each point at their own FSSM blob. The pixel shader runs in the interpreter with different words in both slots and
must return the 38-39 one; the vertex shader's block and access chain are checked in its SPIR-V.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from support import mrt0_words, rdna2spirv, translate_lines  # noqa: E402

GETPC = [
    "s_getpc_b64 s[4:5] ; encoding: [0x04,0x1c,0x84,0xbe]",
    "v_mov_b32_e32 v0, s4 ; encoding: [0x04,0x02,0x00,0x7e]",
    "v_mov_b32_e32 v1, s5 ; encoding: [0x05,0x02,0x02,0x7e]",
]
NEXT = 4    # s_getpc_b64 is the first instruction, 4 bytes long


def push_words(vs_base, ps_base):
    """Push constants with s0..s31 and the heap base zero, `vs_base` in dwords 34-35 and `ps_base` in 38-39."""
    words = [0] * 40
    words[34], words[35] = vs_base & 0xFFFFFFFF, vs_base >> 32
    words[36], words[37] = 0xDEAD0036, 0xDEAD0037        # --mesh-compute's output address: never a code base
    words[38], words[39] = ps_base & 0xFFFFFFFF, ps_base >> 32
    return words


class CodeBaseSlot(unittest.TestCase):
    def test_slots(self):
        self.assertEqual(rdna2spirv.code_base_dword("ps"), 38)
        for stage in ("vs", "ms", "cs", "gs"):
            self.assertEqual(rdna2spirv.code_base_dword(stage), 34, stage)

    def test_pixel_shader_reads_dwords_38_39(self):
        asm = translate_lines("ps_code_base_slot", GETPC + [
            "exp mrt0 v0, v1, v0, v1 done vm ; encoding: [0x0f,0x08,0x00,0xf8,0x00,0x01,0x00,0x01]",
            "s_endpgm ; encoding: [0x00,0x00,0x81,0xbf]"])
        self.assertIn("OpMemberDecorate %UserData 8 Offset 156", asm)      # the block ends at dword 39: 160 bytes
        ps_base = 0x00000002_FFFFFFFE                                       # the +4 carries into the high dword
        words = mrt0_words(asm, push_constants=push_words(0x00000007_00001000, ps_base))
        want = ps_base + NEXT
        self.assertEqual(words, [want & 0xFFFFFFFF, want >> 32] * 2)
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, asm)
        self.assertEqual((meta["uses_code_base"], meta["code_base_dword"], meta["push_constant_bytes"]),
                         (True, 38, 160))

    def test_vertex_shader_keeps_dwords_34_35(self):
        asm = translate_lines("vs_code_base_slot", GETPC + [
            "exp pos0 v0, v1, v0, v1 done ; encoding: [0xcf,0x08,0x00,0xf8,0x00,0x01,0x00,0x01]",
            "s_endpgm ; encoding: [0x00,0x00,0x81,0xbf]"])
        self.assertIn("OpMemberDecorate %UserData 3 Offset 136", asm)
        self.assertIn("OpMemberDecorate %UserData 4 Offset 140", asm)
        self.assertNotIn("OpMemberDecorate %UserData 5 ", asm)             # 144 bytes, as before
        self.assertIn("%ud_code_lo_p = OpAccessChain %_ptr_PC_uint %user_data %c3", asm)
        self.assertIn("%ud_code_hi_p = OpAccessChain %_ptr_PC_uint %user_data %c4", asm)
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, asm)
        self.assertEqual((meta["code_base_dword"], meta["push_constant_bytes"]), (34, 144))

    def test_pixel_shader_without_code_base_stays_144_bytes(self):
        asm = translate_lines("ps_no_code_base", [
            "v_mov_b32_e32 v0, s4 ; encoding: [0x04,0x02,0x00,0x7e]",
            "exp mrt0 v0, v0, v0, v0 done vm ; encoding: [0x0f,0x08,0x00,0xf8,0x00,0x00,0x00,0x00]",
            "s_endpgm ; encoding: [0x00,0x00,0x81,0xbf]"])
        self.assertNotIn("OpMemberDecorate %UserData 5 ", asm)
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, asm)
        self.assertEqual(meta["push_constant_bytes"], 144)
        self.assertNotIn("code_base_dword", meta)


if __name__ == "__main__":
    unittest.main()
