"""64-bit image atomics (D3D-251): Gen.image_atomic_64 evaluated on the CPU against the RDNA2 ISA.

RDNA2 ISA, MIMG atomics: DMASK sets the data size, 0x1 for 32 bits and 0x3 for 64 (cmpswap carries the comparand
too: 0x3 and 0xf); the opcodes are the same (LLVM SIISelLowering lowerImage; llvm-mc -mcpu=gfx1013 rejects any
other mask). The data is a VGPR pair, low dword first; with glc the texel's previous value comes back in it, without
glc the pair is left alone. cmpswap writes DATA (first pair) where the texel equals CMP (second pair), all 64 bits.
A lane EXEC turns off leaves memory and its registers alone. The texel is a 64-bit element of an R32G32 resource,
R the low dword: an R64ui storage image, set 0 binding 26 (2D) or 27 (3D) at the T#'s index.

spirv_mini_interp rejects images, so the SPIR-V the rule emits for one instruction (integer arithmetic, a branch on
%active for the exchanges and one OpAtomic* on an OpImageTexelPointer) runs through a small evaluator below, on one
lane. The descriptor lookup is not part of the rule and is left out (the index is an opaque id). The GPU fixture
cs_imgatomic64 then runs the same way, its v_mov_b32 and image_store lines modelled here, and must leave the words
runtime/test/shaders/check.py expects of it on the GPU. Standard library only; no GPU, no LLVM, no game files.
"""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)

import support  # noqa: E402  (puts tools/ on sys.path)
import rdna2spirv  # noqa: E402
from shader_ir import parse_line  # noqa: E402

sys.path.insert(0, os.path.join(support.ROOT, "runtime", "test", "shaders"))
import check  # noqa: E402  (the GPU runner's expected words)
from compute_fixture import compute_header  # noqa: E402

M32, M64 = (1 << 32) - 1, (1 << 64) - 1


def s64(x):
    return x - (1 << 64) if x >> 63 else x


MODEL = {
    "add": lambda m, d: (m + d) & M64, "sub": lambda m, d: (m - d) & M64,
    "smin": lambda m, d: m if s64(m) <= s64(d) else d, "smax": lambda m, d: m if s64(m) >= s64(d) else d,
    "umin": min, "umax": max, "and": lambda m, d: m & d, "or": lambda m, d: m | d, "xor": lambda m, d: m ^ d,
    "swap": lambda m, d: d,
}
SPIRV = {"add": "OpAtomicIAdd", "sub": "OpAtomicISub", "smin": "OpAtomicSMin", "smax": "OpAtomicSMax",
         "umin": "OpAtomicUMin", "umax": "OpAtomicUMax", "and": "OpAtomicAnd", "or": "OpAtomicOr",
         "xor": "OpAtomicXor", "swap": "OpAtomicExchange", "cmpswap": "OpAtomicCompareExchange"}
# Carries and borrows across the dwords, both signs, and pairs a per-dword min / max / compare would get wrong.
VALUES = [0, 1, M64, 1 << 63, (1 << 63) - 1, 0x00000001_FFFFFFF0, 0x00000007_FFFFFFFF, 0x00000008_00000000,
          0xFFFFFFFF_00000001, 0x12345678_9ABCDEF0]


def run(line, regs, memory, active=True):
    """Gen.image_atomic_64 for `line` on one lane: `regs` (VGPR number -> dword) and `memory` ((descriptor array,
    coordinates) -> 64-bit texel) are updated in place. Returns the OpAtomic* opcodes it ran."""
    ins = parse_line(line)
    g = rdna2spirv.Gen("cs")
    dim = next((m for m in ins["mods"] if m.startswith("dim:")), "dim:SQ_RSRC_IMG_2D")
    if not g.image_atomic_64(ins["op"], ins["args"], ins["mods"], dim, g.addr_regs(ins["args"][1]), "%idx"):
        raise AssertionError(f"not a 64-bit image atomic: {line}")
    consts = {name: v for v, name in g.consts.items()}
    consts.update({name: v for v, name in g.lconsts.items()})
    consts["%c64_32"] = 32
    env = dict(consts)
    ptrs = {}
    body = [ln.split() for ln in g.body]
    labels = {w[0]: i for i, w in enumerate(body) if w[1:] == ["=", "OpLabel"]}
    ran = []
    pc = 0
    while pc < len(body):
        w = body[pc]
        pc += 1
        res, op, args = (w[0], w[2], w[3:]) if len(w) > 1 and w[1] == "=" else (None, w[0], w[1:])
        width = M64 if args and args[0] == "%ulong" else M32
        if op == "OpAccessChain":
            if args[1] == "%vgpr":
                ptrs[res] = consts[args[2]]
            else:
                env[res] = args[1]                            # the descriptor array: %simages64 or %simages64_3d
        elif op == "OpLoad":
            env[res] = active if args[1] == "%active" else regs.get(ptrs[args[1]], 0)
        elif op == "OpStore":
            regs[ptrs[args[0]]] = env[args[1]] & M32
        elif op == "OpCompositeConstruct":
            env[res] = tuple(env[x] for x in args[1:])
        elif op in ("OpBitcast", "OpUConvert"):
            env[res] = env[args[1]] & width
        elif op == "OpShiftLeftLogical":
            env[res] = (env[args[1]] << env[args[2]]) & width
        elif op == "OpShiftRightLogical":
            env[res] = env[args[1]] >> env[args[2]]
        elif op == "OpBitwiseOr":
            env[res] = env[args[1]] | env[args[2]]
        elif op == "OpSelect":
            env[res] = env[args[2]] if env[args[1]] else env[args[3]]
        elif op == "OpImageTexelPointer":
            if args[0] != "%_ptr_Image_ulong" or consts[args[3]] != 0:
                raise AssertionError(f"texel pointer {w}")
            env[res] = (env[args[1]], env[args[2]])
        elif op.startswith("OpAtomic"):
            if args[0] != "%ulong" or consts[args[2]] != 1 or consts[args[3]] != 0:   # 64-bit, Device, Relaxed
                raise AssertionError(f"atomic {w}")
            texel = env[args[1]]
            old = memory[texel]
            if op == "OpAtomicCompareExchange":
                if consts[args[4]] != 0:
                    raise AssertionError(f"atomic {w}")
                value, comparator = env[args[5]], env[args[6]]
                memory[texel] = value if old == comparator else old
            else:
                kind = next(k for k, v in SPIRV.items() if v == op)
                memory[texel] = MODEL[kind](old, env[args[4]])
            env[res] = old
            ran.append(op)
        elif op == "OpBranchConditional":
            pc = labels[args[1] if env[args[0]] else args[2]] + 1
        elif op == "OpBranch":
            pc = labels[args[0]] + 1
        elif op not in ("OpSelectionMerge", "OpLabel"):
            raise AssertionError(f"unexpected instruction {w}")
    return ran


def split(v):
    return v & M32, v >> 32


def case(kind, m, d, c=None, glc=True, three=False, active=True):
    """One atomic on texel (1, 2[, 3]) holding m, data d (and comparand c): (texel after, v4..v7 after, ops run)."""
    cmp = kind == "cmpswap"
    data = "v[4:7]" if cmp else "v[4:5]"
    addr, dim, coords = (("v[1:3]", "3D", (1, 2, 3)) if three else ("v[1:2]", "2D", (1, 2)))
    line = (f"image_atomic_{kind} {data}, {addr}, s[16:23] dmask:{'0xf' if cmp else '0x3'} dim:SQ_RSRC_IMG_{dim} "
            f"unorm{' glc' if glc else ''}")
    regs = {1: 1, 2: 2, 3: 3, 4: d & M32, 5: d >> 32, 6: 0xDEAD, 7: 0xBEEF}
    if cmp:
        regs[6], regs[7] = split(c)
    array = "%simages64_3d" if three else "%simages64"
    memory = {(array, coords): m}
    ran = run(line, regs, memory, active)
    return memory[(array, coords)], [regs[r] for r in (4, 5, 6, 7)], ran


class ImageAtomics64(unittest.TestCase):
    def test_every_op_on_2d_and_3d(self):
        for kind, fn in MODEL.items():
            for three in (False, True):
                for m in VALUES:
                    for d in VALUES[::3]:
                        with self.subTest(op=kind, three=three, m=hex(m), d=hex(d)):
                            texel, regs, ran = case(kind, m, d, three=three)
                            self.assertEqual(texel, fn(m, d))
                            self.assertEqual(regs[:2], list(split(m)))      # glc: the previous texel, low first
                            self.assertEqual(ran, [SPIRV[kind]])

    def test_cmpswap_compares_all_64_bits(self):
        new = 0xDEADBEEF_CAFEF00D
        for m, c in ((0x0000000E_FFFFFFFD, 0x0000000E_FFFFFFFD), (0x0000000F_FFFFFFFE, 0x00000000_FFFFFFFE),
                     (0x00000001_00000000, 0x00000001_00000001), (M64, M64)):
            for three in (False, True):
                with self.subTest(m=hex(m), c=hex(c), three=three):
                    texel, regs, ran = case("cmpswap", m, new, c, three=three)
                    self.assertEqual(texel, new if m == c else m)
                    self.assertEqual(regs, [*split(m), *split(c)])          # the comparand pair is left alone
                    self.assertEqual(ran, ["OpAtomicCompareExchange"])

    def test_seeded(self):
        rng = random.Random(0xD3D251)
        for _ in range(40):
            kind, m, d = rng.choice(list(MODEL)), rng.getrandbits(64), rng.getrandbits(64)
            with self.subTest(op=kind, m=hex(m), d=hex(d)):
                self.assertEqual(case(kind, m, d)[:2], (MODEL[kind](m, d), list(split(m)) + [0xDEAD, 0xBEEF]))

    def test_without_glc_the_data_stays(self):
        m, d = 0x0000000A_FFFFFFF9, 0x0000FFFF_0000FFFF
        for kind, fn in MODEL.items():
            with self.subTest(op=kind):
                self.assertEqual(case(kind, m, d, glc=False)[:2], (fn(m, d), [*split(d), 0xDEAD, 0xBEEF]))
        self.assertEqual(case("cmpswap", m, d, m, glc=False)[:2], (d, [*split(d), *split(m)]))

    def test_inactive_lane_changes_nothing(self):
        m, d = 0x00000003_FFFFFFF2, 0xFFFFFFFF_FFFFFFF0
        for kind in [*MODEL, "cmpswap"]:
            with self.subTest(op=kind):
                texel, regs, ran = case(kind, m, d, m, active=False)
                self.assertEqual(texel, m)
                self.assertEqual(regs[:2], list(split(d)))
                # The exchanges are skipped; the others run with the operand that leaves the texel as it is.
                self.assertEqual(ran, [] if kind in ("swap", "cmpswap") else [SPIRV[kind]])

    def test_32_bit_masks_are_not_this_rule(self):
        g = rdna2spirv.Gen("cs")
        for line in ("image_atomic_add v4, v[1:2], s[16:23] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc",
                     "image_atomic_cmpswap v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc",
                     "image_get_resinfo v[4:7], v1, s[16:23] dmask:0xf dim:SQ_RSRC_IMG_2D unorm"):
            ins = parse_line(line)
            with self.subTest(line=line):
                self.assertFalse(g.image_atomic_64(ins["op"], ins["args"], ins["mods"], "dim:SQ_RSRC_IMG_2D",
                                                   g.addr_regs(ins["args"][1]), "%idx"))
        self.assertEqual(g.body, [])

    def test_unsupported_forms(self):
        for line in ("image_atomic_inc v[4:5], v[1:2], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D unorm glc",
                     "image_atomic_add v[4:5], v[1:3], s[16:23] dmask:0x3 dim:SQ_RSRC_IMG_2D_ARRAY unorm glc",
                     "image_atomic_swap v4, v[1:2], s[16:23] dmask:0x1 dim:SQ_RSRC_IMG_2D unorm glc"):
            with self.subTest(line=line):
                with self.assertRaises(rdna2spirv.Unsupported):
                    support.translate_lines("cs_img64_bad", ["v_mov_b32_e32 v1, 0", line], fssm=bytes(0x148))

    def test_module_declares_the_r64ui_bindings(self):
        spv = support.translate_fixture("cs_imgatomic64", fssm=self.fssm())
        for text in ("OpCapability Int64Atomics", "OpCapability Int64ImageEXT", 'OpExtension "SPV_EXT_shader_image_int64"',
                     "%simg2d_u64 = OpTypeImage %ulong 2D 0 0 0 2 R64ui", "%simg3d_u64 = OpTypeImage %ulong 3D 0 0 0 2 R64ui",
                     "OpDecorate %simages64 Binding 26", "OpDecorate %simages64_3d Binding 27"):
            with self.subTest(text=text):
                self.assertIn(text, spv)
        self.assertNotIn("R32ui", spv)              # no 32-bit atomic in the fixture: binding 4 / 7 stay out

    @staticmethod
    def fssm():
        return bytes(compute_header(os.path.join(support.ROOT, "runtime", "test", "shaders", "cs_imgatomic64.asm")))

    def test_gpu_fixture_words(self):
        """cs_imgatomic64 on lane 0 (its first atomic on all 64 lanes, before v_cmpx) with vk_store_test's fills."""
        images, fills = check.STORE64_IMAGES, check.STORE64_FILL
        words = {name: [fill(i) for i in range(64)] for name, fill in zip(images, fills)}
        by_tsharp = {0x2000: "uint", 0x3000: "sint", 0x6000: "wide", 0x7000: "wide3d"}
        arrays = {"%simages64": "wide", "%simages64_3d": "wide3d"}
        texel = {2: lambda c: c[1] * 8 + c[0], 3: lambda c: c[2] * 16 + c[1] * 4 + c[0]}
        sgprs, regs, lanes = {}, {}, 64
        with open(os.path.join(support.FIXTURE_DIR, "cs_imgatomic64.s"), encoding="utf-8") as f:
            lines = [ln.strip() for ln in f if ln.startswith("\t")]
        for line in lines:
            ins = parse_line(line)
            op, a = ins["op"], ins["args"]
            if op == "s_mov_b32":
                sgprs[a[0]["r"]] = a[1]["v"] & M32
            elif op == "v_mov_b32_e32":
                regs[a[0]["r"]] = a[1]["v"] & M32
            elif op == "v_cmpx_eq_u32_e32":
                lanes = 1                               # 0 == v0: thread 0 only from here on
            elif op == "image_store":
                image = by_tsharp[sgprs[a[2]["r"]]]
                words[image][regs[a[1]["r"] + 1] * 8 + regs[a[1]["r"]]] = regs[a[0]["r"]]
            elif op.startswith("image_atomic_"):
                name = by_tsharp[sgprs[a[2]["r"]]]
                memory = {}
                for _ in range(lanes):                  # identical lanes: each runs the atomic in turn
                    ran = run(line, regs, TexelView(memory, words[name], texel), True)
                    self.assertEqual(len(ran), 1)
                self.assertEqual({arrays[k[0]] for k in memory}, {name})   # the binding of the T#'s dimension
            elif op not in ("s_waitcnt", "s_endpgm"):
                self.fail(f"the fixture model has no rule for {line}")
        for name, want in zip(images, check.store_words("cs_imgatomic64")):
            with self.subTest(image=name):
                self.assertEqual(words[name], want)


class TexelView(dict):
    """The (array, coordinates) -> texel view run() takes, backed by one image's 64 words."""

    def __init__(self, seen, image, texel):
        super().__init__()
        self.seen, self.image, self.texel = seen, image, texel

    def _index(self, key):
        self.seen[key] = True
        return self.texel[len(key[1])](key[1])

    def __getitem__(self, key):
        return self.image[self._index(key)]

    def __setitem__(self, key, value):
        self.image[self._index(key)] = value


if __name__ == "__main__":
    unittest.main()
