"""rdna2spirv.py --no-vertex-subgroups: a VS that reads a buffer through a run-time V# without a subgroup builtin.

Since SHADER-231 every MUBUF access through a run-time V# adds ADD_TID_ENABLE (word3 bit 23) times the wave lane to
the index, and outside compute and mesh shaders that lane is SubgroupLocalInvocationId (Gen.wave_lane). A Metal vertex
function has no SIMD-group lane, so MoltenVK, whose subgroupSupportedStages has no vertex bit, can't create a pipeline
with such a VS: SPIRV-Cross stops with "Subgroup builtins are not available in this type of function" (vs_m15 on an
Apple M5, MoltenVK 1.4.2). With the flag a VS leaves the lane out instead. That is exact while the run-time bit is
clear, which is the vertex fetch case, and reads lane 0's element when it is set; a literal V# with the bit is
refused. Compute and pixel shaders are unchanged. Here:
* the default VS declares the builtin and adds the lane when the run-time bit is set (lane 5 reads 5 records on);
* with the flag the VS declares neither the builtin nor a GroupNonUniform capability, reads the same record as the
  default when the bit is clear, and the vertex's own record when it is set;
* a literal V# with ADD_TID_ENABLE in a VS is Unsupported under the flag;
* a CS and a PS with the same access translate byte for byte the same with and without the flag.
"""
import unittest

import support
import rdna2spirv
import test_buffer_swizzle as bs         # module only: importing its classes would collect them a second time
import test_value_semantics as tv

STRIDE = 4
RECORDS = 0x40
LANE = 5
VERTEX = 2


def translate(name, lines, flag):
    old = rdna2spirv.NO_VERTEX_SUBGROUPS
    rdna2spirv.NO_VERTEX_SUBGROUPS = flag
    try:
        return support.translate_lines(name, lines)
    finally:
        rdna2spirv.NO_VERTEX_SUBGROUPS = old


def fetch_vs(tid, runtime=True):
    """A VS that loads one dword of record VertexIndex through the V# in s[4:7] and exports it as param0."""
    return [*bs.v_sharp(STRIDE, RECORDS, 0, 0, swizzle=False, tid=tid, runtime=runtime),
            "buffer_load_dword v3, v0, s[4:7], 0 idxen", "s_waitcnt vmcnt(0)",
            "v_mov_b32 v5, 1.0", "exp pos0 v5, v5, v5, v5 done", "exp param0 v3, v3, v3, v3", "s_endpgm"]


def param0(spv, lane=None):
    """Run the VS for vertex VERTEX (and subgroup lane `lane` when it declares one): (param0.x, the reads)."""
    memory = bs.PatternMemory()
    inputs = {"%vertex_index": VERTEX, "%instance_index": 0}
    if lane is not None:
        inputs["%sg_lane"] = lane
    out = support.run_fragment(spv, inputs=inputs, push_constants=[0] * 32, memory=memory)
    return list(out["%out_param0"])[0], [a - bs.BASE for a in memory.reads]


class VertexShader(unittest.TestCase):
    def test_default_reads_the_subgroup_lane(self):
        spv = translate("vs_fetch_tid", fetch_vs(tid=True), flag=False)
        self.assertIn("BuiltIn SubgroupLocalInvocationId", spv)
        self.assertIn("OpCapability GroupNonUniform", spv)
        word, reads = param0(spv, lane=LANE)
        self.assertEqual(reads, [(VERTEX + LANE) * STRIDE])
        self.assertEqual(word, tv._pattern_word((VERTEX + LANE) * STRIDE))

    def test_flag_declares_no_subgroup_builtin(self):
        for tid in (False, True):
            with self.subTest(tid=tid):
                spv = translate("vs_fetch", fetch_vs(tid=tid), flag=True)
                self.assertNotIn("SubgroupLocalInvocationId", spv)
                self.assertNotIn("%sg_lane", spv)
                self.assertNotIn("GroupNonUniform", spv)

    def test_flag_is_exact_while_the_bit_is_clear(self):
        default = param0(translate("vs_fetch", fetch_vs(tid=False), flag=False), lane=LANE)
        flagged = param0(translate("vs_fetch", fetch_vs(tid=False), flag=True))
        self.assertEqual(flagged, default)
        self.assertEqual(flagged, (tv._pattern_word(VERTEX * STRIDE), [VERTEX * STRIDE]))

    def test_flag_with_the_bit_set_reads_lane_zero(self):
        """The documented approximation: no vertex lane, so the record is the vertex's own (lane 0's index)."""
        word, reads = param0(translate("vs_fetch_tid", fetch_vs(tid=True), flag=True))
        self.assertEqual(reads, [VERTEX * STRIDE])
        self.assertEqual(word, tv._pattern_word(VERTEX * STRIDE))

    def test_literal_add_tid_is_refused_under_the_flag(self):
        lines = fetch_vs(tid=True, runtime=False)
        self.assertIn("BuiltIn SubgroupLocalInvocationId", translate("vs_fetch_lit", lines, flag=False))
        with self.assertRaises(rdna2spirv.Unsupported):
            translate("vs_fetch_lit", lines, flag=True)
        # a literal V# without the bit folds the lane away either way
        lines = fetch_vs(tid=False, runtime=False)
        self.assertEqual(translate("vs_fetch_lit", lines, flag=True), translate("vs_fetch_lit", lines, flag=False))


class OtherStages(unittest.TestCase):
    def test_compute_and_pixel_shaders_are_unchanged(self):
        access = [*bs.v_sharp(STRIDE, RECORDS, 0, 0, swizzle=False, tid=True, runtime=True),
                  "v_mov_b32 v1, 1", "buffer_load_dword v3, v1, s[4:7], 0 idxen", "s_waitcnt vmcnt(0)"]
        cs = [*access, "buffer_store_dword v3, off, s[0:3], 0", "s_endpgm"]
        ps = [*access, "exp mrt0 v3, v3, v3, v3 done vm", "s_endpgm"]
        for name, lines, fssm in (("cs_fetch_tid", cs, tv.CS_BOUNDS_FSSM), ("ps_fetch_tid", ps, None)):
            with self.subTest(stage=name[:2]):
                old = rdna2spirv.NO_VERTEX_SUBGROUPS
                try:
                    rdna2spirv.NO_VERTEX_SUBGROUPS = False
                    default = support.translate_lines(name, lines, fssm=fssm)
                    rdna2spirv.NO_VERTEX_SUBGROUPS = True
                    flagged = support.translate_lines(name, lines, fssm=fssm)
                finally:
                    rdna2spirv.NO_VERTEX_SUBGROUPS = old
                self.assertEqual(flagged, default)
                if name.startswith("ps"):
                    self.assertIn("BuiltIn SubgroupLocalInvocationId", flagged)    # Metal fragment functions have one


if __name__ == "__main__":
    unittest.main()
