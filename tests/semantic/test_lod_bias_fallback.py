"""rdna2spirv.py --lod-bias-fallback: the S#'s LOD_BIAS applied by the shader (SHADER-268).

MoltenVK's samplers have no LOD bias (VkPhysicalDevicePortabilitySubsetFeaturesKHR::samplerMipLodBias false), so
there the runtime creates every VkSampler with mipLodBias 0 and the shader adds the bias where Vulkan adds the
sampler's: to the base LOD, before the min/max clamp. The bias is S# word 2 [13:0], signed 5.8 fixed point. Here,
with the S# held in SGPRs (a run-time S#) or set by s_mov (a literal one):
* ps_ssharp's implicit image_sample takes it as the Bias operand, and its image_sample_lz (all three typed reads)
  as the LOD;
* ps_ssharp_ops' image_sample_l gets it added to its LOD of 0.625, image_sample_b to its bias of -0.25, and
  image_sample_d / _d_cl scale their gradients by 2^bias (exactly, for a whole bias), keeping _d_cl's min-LOD clamp;
* an implicit sample outside the PS, which samples LOD 0, samples LOD = bias;
* ps_ssharp_lod's image_get_lod (OpImageQueryLod, whose lambda' includes the sampler's bias) queries at its coordinate
  times 2^bias, which adds the bias to the LOD the device computes and leaves the clamps to the device;
* gathers are unchanged: Vulkan gathers ignore the sampler's bias (measured on KosmicKrisp and llvmpipe), and
  image_gather4_l fetches at the given LOD on every device;
* a literal S# with no bias, and every shader without the flag, translate as before, with no lod_bias_fallback
  in the metadata.
The operands are worked out by a small evaluator over the emitted SPIR-V (the interpreter models no texture access).
The GPU checks are runtime/test/shaders/ps_ssharp, ps_ssharp_ops and ps_ssharp_lod (check.py's sampler_cases), which
run_gpu_tests.py translates with the flag where tools/vk_subgroup_info.py reports sampler_lod_bias false.
"""
import math
import re
import struct
import unittest

import support
import rdna2spirv

S_WORD2 = 18        # the S# is s[16:19] in every shader here


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def bits_f32(w):
    return struct.unpack("<f", struct.pack("<I", w & 0xFFFFFFFF))[0]


def bias_field(bias):
    """S# word 2 with LOD_BIAS = bias (a multiple of 1/256) and MIP_FILTER point, as check.ssharp_words makes it."""
    return (round(bias * 256) & 0x3FFF) | 1 << 26


class Operands:
    """Values of the ids in one translated module, from its constants, its stores to the register arrays and the
    S# words given. Register arrays are Function variables indexed by constants; %active is true (one lane)."""

    def __init__(self, spv, sgprs):
        self.defs, self.consts, self.sgprs, self.memo = {}, {}, sgprs, {}
        self.lines = [ln.strip() for ln in spv.splitlines()]
        regs, stores = {}, {}          # access-chain id -> (array, index); (array, index) -> last value stored
        for ln in self.lines:
            m = re.match(r"(%\S+) = (Op\w+) (.*)", ln)
            if m:
                res, op, rest = m.groups()
                a = rest.split()
                self.defs[res] = (op, a)
                if op == "OpConstant" and a[0] == "%uint":
                    self.consts[res] = int(a[1])
                if op == "OpAccessChain" and len(a) == 3 and a[1] in ("%vgpr", "%sgpr"):
                    regs[res] = (a[1], a[2])
                if op == "OpLoad" and a[1] in regs:
                    # A register read sees the last store to that register before it.
                    self.defs[res] = ("RegLoad", [regs[a[1]], stores.get(regs[a[1]])])
            elif ln.startswith("OpStore "):
                ptr, value = ln.split()[1:3]
                if ptr in regs:
                    stores[regs[ptr]] = value

    def __call__(self, x):
        if x not in self.memo:
            self.memo[x] = self.evaluate(x)
        return self.memo[x]

    def evaluate(self, x):
        if x in self.consts:
            return self.consts[x]
        if x == "%f0":
            return 0.0
        if x == "%f1":
            return 1.0
        op, a = self.defs[x]
        v = self
        if op == "RegLoad":
            (array, index), stored = a
            if array == "%sgpr" and self.consts[index] in self.sgprs:
                return self.sgprs[self.consts[index]]
            if stored is None:
                raise KeyError(f"{array}[{self.consts[index]}] read before any store")
            return v(stored)
        if op == "OpLoad" and a[1] == "%active":
            return True
        if op == "OpSelect":
            return v(a[2]) if v(a[1]) else v(a[3])
        if op == "OpBitcast":
            src = v(a[1])
            if a[0] == "%float":
                return bits_f32(src) if isinstance(src, int) else src
            if isinstance(src, float):
                src = struct.unpack("<I", struct.pack("<f", src))[0]
            return src - (1 << 32) if a[0] == "%int" and src >= 1 << 31 else src & 0xFFFFFFFF
        if op == "OpBitFieldSExtract":
            value, offset, count = v(a[1]) & 0xFFFFFFFF, v(a[2]), v(a[3])
            field = (value >> offset) & ((1 << count) - 1)
            return ((field ^ (1 << (count - 1))) - (1 << (count - 1))) & 0xFFFFFFFF
        if op in ("OpConvertSToF", "OpConvertUToF"):
            n = v(a[1])
            if op == "OpConvertSToF" and n >= 1 << 31:
                n -= 1 << 32
            return f32(n)
        if op == "OpConvertFToS":
            return int(v(a[1]))
        if op in ("OpFMul", "OpFAdd", "OpFSub"):
            p, q = v(a[1]), v(a[2])
            return f32(p * q if op == "OpFMul" else p + q if op == "OpFAdd" else p - q)
        if op == "OpFOrdEqual":
            return v(a[1]) == v(a[2])
        if op == "OpVectorTimesScalar":
            return tuple(f32(c * v(a[2])) for c in v(a[1]))
        if op == "OpCompositeConstruct":
            return tuple(v(c) for c in a[1:])
        if op in ("OpDPdx", "OpDPdy"):
            # A 2D image_sample in the PS samples with Grad from the quad derivatives (the typed path). One lane has
            # none, so the evaluator takes unit gradients, d/dx = (1, 0) and d/dy = (0, 1): a scaled Grad shows as 2^bias.
            return (1.0, 0.0) if op == "OpDPdx" else (0.0, 1.0)
        if op == "OpExtInst" and a[2] == "Floor":
            return float(math.floor(v(a[3])))
        if op == "OpExtInst" and a[2] == "Exp2":
            return f32(2.0 ** v(a[3]))
        if op == "OpExtInst" and a[2] == "Ldexp":
            return f32(math.ldexp(v(a[3]), v(a[4])))
        raise NotImplementedError(f"{op} {' '.join(a)}")

    def samples(self):
        """Each OpImageSample*: (opcode, {image operand name: value, or a list of values for Grad})."""
        out = []
        for ln in self.lines:
            m = re.match(r"%\S+ = (OpImageSample\w*) (.*)", ln)
            if not m:
                continue
            op, rest = m.groups()
            words = rest.split()[3 + op.startswith("OpImageSampleDref"):]  # type, sampled image, coordinate (, dref)
            operands, i = {}, 0
            if words:
                for name in words[0].split("|"):
                    n = 2 if name == "Grad" else 1
                    args = words[1 + i:1 + i + n]
                    operands[name] = [self(w) for w in args] if n > 1 else self(args[0])
                    i += n
            out.append((op, operands))
        return out


def translate(name, flag, lines=None):
    old = rdna2spirv.LOD_BIAS_FALLBACK
    rdna2spirv.LOD_BIAS_FALLBACK = flag
    try:
        spv = support.translate_lines(name, lines) if lines else support.translate_fixture(name)
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, spv)
    finally:
        rdna2spirv.LOD_BIAS_FALLBACK = old
    return spv, meta


BIASES = (1.0, -1.0, 0.375, -0.75, -32.0, 8191 / 256)      # whole, fractional and both ends of the field


class RunTimeSsharp(unittest.TestCase):
    """ps_ssharp and ps_ssharp_ops, whose S# is loaded from memory: the bias is read from s18 at run time."""

    @classmethod
    def setUpClass(cls):
        cls.ssharp, cls.ssharp_meta = translate("ps_ssharp", True)
        cls.ops, cls.ops_meta = translate("ps_ssharp_ops", True)

    def assert_unit_grads_scaled(self, operands, scale, exact):
        (dxs, dxt), (dys, dyt) = operands["Grad"]
        self.assertEqual((dxt, dys), (0.0, 0.0))
        if exact:
            self.assertEqual((dxs, dyt), (scale, scale))
        else:
            self.assertAlmostEqual(dxs / scale, 1.0, places=6)
            self.assertEqual(dxs, dyt)

    def test_implicit_scales_its_gradients_and_lz_takes_a_lod(self):
        # A 2D image_sample and image_sample_lz read through a sint, a uint and a float view (image_read_typed); the
        # implicit one samples with Grad from the quad derivatives, which carries no Bias operand, so the gradients
        # are scaled by 2^bias instead.
        for bias in BIASES:
            with self.subTest(bias=bias):
                got = Operands(self.ssharp, {S_WORD2: bias_field(bias)}).samples()
                self.assertEqual([op for op, _ in got[:3]], ["OpImageSampleExplicitLod"] * 3)
                for _, operands in got[:3]:
                    self.assert_unit_grads_scaled(operands, 2.0 ** bias, bias == int(bias))
                self.assertEqual(got[3:], [("OpImageSampleExplicitLod", {"Lod": bias})] * 3)

    def test_explicit_lod_bias_and_gradients(self):
        for bias in BIASES:
            with self.subTest(bias=bias):
                lsample, bsample, dsample, clsample = Operands(self.ops, {S_WORD2: bias_field(bias)}).samples()
                self.assertEqual(lsample, ("OpImageSampleExplicitLod", {"Lod": f32(0.625 + bias)}))
                self.assertEqual(bsample, ("OpImageSampleImplicitLod", {"Bias": f32(-0.25 + bias)}))
                scale = 2.0 ** bias
                for (op, operands), grad, clamp in ((dsample, 1 / 128, None), (clsample, 1 / 256, 0.75)):
                    self.assertEqual(op, "OpImageSampleExplicitLod")
                    (dxs, dxt), (dys, dyt) = operands["Grad"]
                    self.assertEqual((dxt, dys), (0.0, 0.0))
                    if bias == int(bias):   # 2^bias by Ldexp: exact
                        self.assertEqual((dxs, dyt), (grad * scale, grad * scale))
                    else:
                        self.assertAlmostEqual(dxs / (grad * scale), 1.0, places=6)
                        self.assertEqual(dxs, dyt)
                    self.assertEqual(operands.get("MinLod"), clamp)

    def test_metadata(self):
        self.assertTrue(self.ssharp_meta.get("lod_bias_fallback"))
        self.assertTrue(self.ops_meta.get("lod_bias_fallback"))


class Unchanged(unittest.TestCase):
    """Without the flag the S# bias is the sampler's: no Bias operand, LOD 0 for _lz, the instruction's own values."""

    def test_without_the_flag(self):
        spv, meta = translate("ps_ssharp", False)
        got = Operands(spv, {S_WORD2: bias_field(1.0)}).samples()
        self.assertEqual(got, [("OpImageSampleExplicitLod", {"Grad": [(1.0, 0.0), (0.0, 1.0)]})] * 3
                         + [("OpImageSampleExplicitLod", {"Lod": 0.0})] * 3)
        self.assertNotIn("lod_bias_fallback", meta)
        spv, meta = translate("ps_ssharp_ops", False)
        got = Operands(spv, {S_WORD2: bias_field(1.0)}).samples()
        self.assertEqual(got[0], ("OpImageSampleExplicitLod", {"Lod": 0.625}))
        self.assertEqual(got[1], ("OpImageSampleImplicitLod", {"Bias": -0.25}))
        self.assertEqual(got[2][1]["Grad"], [(1 / 128, 0.0), (0.0, 1 / 128)])
        self.assertNotIn("lod_bias_fallback", meta)


class LodQuery(unittest.TestCase):
    """ps_ssharp_lod: both image_get_lod queries take their coordinate times 2^bias with the flag, as is without."""

    @staticmethod
    def queries(spv):
        return [re.match(r"%\S+ = OpImageQueryLod \S+ \S+ (\S+)", ln.strip()).group(1)
                for ln in spv.splitlines() if "OpImageQueryLod" in ln]

    def test_the_coordinate_is_scaled_by_2_to_the_bias(self):
        spv, meta = translate("ps_ssharp_lod", True)
        coords = self.queries(spv)
        self.assertEqual(len(coords), 2)
        for bias in BIASES:
            with self.subTest(bias=bias):
                ops = Operands(spv, {S_WORD2: bias_field(bias)})
                for coord in coords:
                    op, a = ops.defs[coord]
                    self.assertEqual((op, a[0]), ("OpVectorTimesScalar", "%v2float"))
                    self.assertEqual(ops.defs[a[1]][0], "OpCompositeConstruct")
                    if bias == int(bias):   # Ldexp: exact
                        self.assertEqual(ops(a[2]), 2.0 ** bias)
                    else:
                        self.assertAlmostEqual(ops(a[2]) / 2.0 ** bias, 1.0, places=6)
        self.assertTrue(meta.get("lod_bias_fallback"))

    def test_without_the_flag(self):
        spv, meta = translate("ps_ssharp_lod", False)
        ops = Operands(spv, {})
        self.assertEqual([ops.defs[c][0] for c in self.queries(spv)], ["OpCompositeConstruct"] * 2)
        self.assertNotIn("lod_bias_fallback", meta)


def literal_program(word2):
    """A literal S#: word 2 by s_mov_b32, the T# loaded as in ps_ssharp, one implicit sample."""
    return ["s_load_dwordx8 s[8:15], s[0:1], 0x0", "s_mov_b32 s16, 0x92", "s_mov_b32 s17, 0xf00000",
            f"s_mov_b32 s18, 0x{word2:x}", "s_mov_b32 s19, 0", "s_waitcnt lgkmcnt(0)",
            "v_interp_p1_f32_e32 v2, v0, attr0.x", "v_interp_p2_f32_e32 v2, v1, attr0.x",
            "v_interp_p1_f32_e32 v3, v0, attr0.y", "v_interp_p2_f32_e32 v3, v1, attr0.y",
            "image_sample v[4:7], v[2:3], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
            "s_waitcnt vmcnt(0)", "exp mrt0 v4, v5, v6, v7 done vm", "s_endpgm"]


class LiteralSsharp(unittest.TestCase):
    def test_a_literal_bias_is_a_constant(self):
        spv, meta = translate("ps_literal_bias", True, literal_program(bias_field(1.5)))
        self.assertNotIn("OpBitFieldSExtract", spv)
        got = Operands(spv, {}).samples()
        self.assertEqual(len(got), 3)              # the sint, uint and float views of a 2D image
        for op, operands in got:
            self.assertEqual(op, "OpImageSampleExplicitLod")
            (dxs, dxt), (dys, dyt) = operands["Grad"]
            self.assertEqual((dxt, dys), (0.0, 0.0))
            self.assertAlmostEqual(dxs / 2.0 ** 1.5, 1.0, places=6)
            self.assertEqual(dxs, dyt)
        self.assertTrue(meta.get("lod_bias_fallback"))

    def test_a_literal_s_without_bias_translates_as_before(self):
        on, meta = translate("ps_literal_bias", True, literal_program(bias_field(0)))
        off, _ = translate("ps_literal_bias", False, literal_program(bias_field(0)))
        self.assertEqual(on, off)
        self.assertNotIn("lod_bias_fallback", meta)


class OtherStagesAndGathers(unittest.TestCase):
    def test_vertex_shader_samples_lod_bias(self):
        """An implicit sample outside the PS samples LOD 0 (there are no derivatives), so with the bias LOD = bias:
        what Vulkan gives the native path, whose sampler adds its mipLodBias to the explicit LOD 0."""
        program = ["s_load_dwordx8 s[8:15], s[0:1], 0x0", "s_load_dwordx4 s[16:19], s[0:1], 0x20",
                   "s_waitcnt lgkmcnt(0)", "v_mov_b32_e32 v2, 0.5", "v_mov_b32_e32 v3, 0.5",
                   "image_sample v[4:7], v[2:3], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D",
                   "s_waitcnt vmcnt(0)", "exp pos0 v4, v5, v6, v7 done", "s_endpgm"]
        spv, _ = translate("vs_sample_bias", True, program)
        self.assertEqual(Operands(spv, {S_WORD2: bias_field(-2.0)}).samples(),
                         [("OpImageSampleExplicitLod", {"Lod": -2.0})])
        spv, _ = translate("vs_sample_bias", False, program)
        self.assertEqual(Operands(spv, {S_WORD2: bias_field(-2.0)}).samples(),
                         [("OpImageSampleExplicitLod", {"Lod": 0.0})])

    def test_gathers_are_unchanged(self):
        """image_gather4_lz (OpImageGather, which ignores a sampler bias) and image_gather4_l (fetches at the
        rounded, clamped LOD) translate the same with and without the flag."""
        for op, addr in (("image_gather4_lz", "v[2:3]"), ("image_gather4_l", "v[2:4]")):
            program = ["s_load_dwordx8 s[8:15], s[0:1], 0x0", "s_load_dwordx4 s[16:19], s[0:1], 0x20",
                       "s_waitcnt lgkmcnt(0)", "v_mov_b32_e32 v2, 0.5", "v_mov_b32_e32 v3, 0.5",
                       "v_mov_b32_e32 v4, 1.0",
                       f"{op} v[4:7], {addr}, s[8:15], s[16:19] dmask:0x1 dim:SQ_RSRC_IMG_2D",
                       "s_waitcnt vmcnt(0)", "exp mrt0 v4, v5, v6, v7 done vm", "s_endpgm"]
            with self.subTest(op=op):
                on, meta = translate("ps_gather_bias", True, program)
                off, _ = translate("ps_gather_bias", False, program)
                self.assertEqual(on, off)
                self.assertNotIn("lod_bias_fallback", meta)


if __name__ == "__main__":
    unittest.main()
