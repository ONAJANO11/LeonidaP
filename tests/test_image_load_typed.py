"""Typed image_load in tools/rdna2spirv.py reads every format from the sampled array (SHADER-151; standard library only:
no GPU, LLVM or game files; SPIRV-Tools only for the optional validation test).

    python -I tests/test_image_load_typed.py

The image's numeric type lives in the T# (word 1 FORMAT [28:20]), so the translator branches on it at run time.
UINT and SINT formats are fetched through uint and sint aliases of the dimension's sampled array (same set and
binding), everything else and a lookup miss (index 0, the RGBA8_UNORM fallback) through the float array. Nothing
reads a storage image, so a shader that only loads declares no StorageImageReadWithoutFormat (which Intel Xe-LP
lacks, #619) and no StorageImageWriteWithoutFormat. These tests translate small pixel shaders and check the SPIR-V:
the capabilities, which binding and component type each fetch reads, and, with a small evaluator of the code that
picks the branch, the branch taken for all 512 FORMAT values with a hit and with a miss.
runtime/test/shaders/cs_imgload.asm checks the result on a GPU (vk_store_test load).
"""
import os
import re
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rdna2spirv  # noqa: E402

EMPTY_DXBC = b"DXBC" + bytes(16) + struct.pack("<III", 1, 32, 0)
EPILOGUE = ["v_mov_b32 v8, v3", "exp mrt0 v8, v8, v8, v8 done vm", "s_endpgm"]

# dim -> (address registers, sampled binding, SPIR-V Dim, arrayed)
DIMS = {"2D": ("v[4:5]", 0, "2D", "0"), "2D_ARRAY": ("v[4:6]", 3, "2D", "1"),
        "3D": ("v[4:6]", 6, "3D", "0"), "1D": ("v4", 5, "1D", "0")}
STORAGE_IMAGE_CAPS = {"StorageImageReadWithoutFormat", "StorageImageWriteWithoutFormat", "Image1D"}


def translate(lines):
    """SPIR-V assembly text of a pixel shader made of these instruction lines (and an export of v3)."""
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        with open(os.path.join(tmp, "ps_load.dxbc"), "wb") as f:
            f.write(EMPTY_DXBC)
        path = os.path.join(tmp, "disasm", "ps_load.s")
        with open(path, "w", encoding="utf-8") as f:
            f.write("; ps_load code 0x0..0x100\n" + "".join("\t" + ln + "\n" for ln in lines + EPILOGUE))
        return rdna2spirv.translate(path)


def load_shader(dim, dmask=0xF):
    addr = DIMS[dim][0]
    return translate(["v_mov_b32 v4, 1", "v_mov_b32 v5, 2", "v_mov_b32 v6, 0",
                      f"image_load v[0:3], {addr}, s[0:7] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_{dim} unorm",
                      "s_waitcnt vmcnt(0)"])


class Module:
    """The instructions of a SPIR-V assembly text by result id, its decorations and capabilities."""

    def __init__(self, text):
        self.text = text
        self.defs, self.binding, self.order = {}, {}, []
        self.caps = set(re.findall(r"^\s*OpCapability (\w+)", text, re.M))
        for line in text.splitlines():
            m = re.match(r"\s*(%\w+) = (Op\w+)\s*(.*)$", line)
            if m:
                self.defs[m.group(1)] = (m.group(2), m.group(3).split())
                self.order.append((m.group(1), m.group(2), m.group(3).split()))
                continue
            m = re.match(r"\s*OpDecorate (%\w+) Binding (\d+)$", line)
            if m:
                self.binding[m.group(1)] = int(m.group(2))
            m = re.match(r"\s*(OpBranchConditional) (.*)$", line)
            if m:
                self.order.append((None, m.group(1), m.group(2).split()))

    def fetches(self):
        """(result id, image variable, OpTypeImage operands) of every OpImageFetch."""
        out = []
        for rid, op, args in self.order:
            if op != "OpImageFetch":
                continue
            load_op, load = self.defs[args[1]]
            assert load_op == "OpLoad", load_op
            chain_op, chain = self.defs[load[1]]
            assert chain_op == "OpAccessChain", chain_op
            out.append((rid, chain[1], self.defs[load[0]][1]))
        return out

    def value(self, x, word1, index, regs=None):
        """Evaluate the format selection or volume guard, optionally supplying a test's SGPR/VGPR words."""
        get = lambda y: self.value(y, word1, index, regs)
        if x in ("%true", "%false"):
            return x == "%true"
        op, a = self.defs[x]
        if op == "OpConstant":
            return int(a[1], 0)
        if op == "OpConstantTrue":
            return True
        if op == "OpFunctionCall" and a[1] == "%desc_lookup_fn":
            return index
        if op == "OpLoad":
            ptr_op, p = self.defs[a[1]]
            assert ptr_op == "OpAccessChain", ptr_op
            if p[1] in ("%sgpr", "%vgpr"):
                r = get(p[2])
                if regs is not None:
                    return regs[p[1], r]
                assert (p[1], r) == ("%sgpr", 1), "only T# word 1 is read"
                return word1
            var_op, v = self.defs[p[1]]
            assert var_op == "OpVariable" and v[1] == "Private", (var_op, v)
            words = self.defs[v[2]][1][1:]            # OpConstantComposite %fmt_mask_arr <words>
            return get(words[get(p[2])])
        if op == "OpCompositeExtract":
            return get(a[1])[int(a[2])]
        if op == "OpCompositeInsert":
            result = list(get(a[2]))
            result[int(a[3])] = get(a[1])
            return tuple(result)
        u = [get(y) for y in a[1:]]
        if op == "OpCompositeConstruct":
            return tuple(u)
        if op == "OpBitcast":
            return u[0]
        if op == "OpBitFieldUExtract":
            return (u[0] >> u[1]) & ((1 << u[2]) - 1)
        if op == "OpShiftRightLogical":
            return u[0] >> u[1]
        if op == "OpBitwiseAnd":
            return u[0] & u[1]
        if op == "OpINotEqual":
            return u[0] != u[1]
        if op == "OpIEqual":
            return u[0] == u[1]
        if op == "OpIAdd":
            return (u[0] + u[1]) & 0xffffffff
        if op == "OpULessThanEqual":
            return u[0] <= u[1]
        if op == "OpSelect":
            return u[1] if u[0] else u[2]
        if op == "OpLogicalAnd":
            return u[0] and u[1]
        if op == "OpLogicalOr":
            return u[0] or u[1]
        if op == "OpLogicalNot":
            return not u[0]
        raise AssertionError(f"unexpected {op} in the branch condition")

    def branch(self, word1, index):
        """'sint', 'uint' or 'float': which fetch the typed load's two selections lead to."""
        conds, last = {}, None
        for rid, op, args in self.order:
            if op == "OpBranchConditional":
                last = args[0]
            elif op == "OpAccessChain" and re.fullmatch(r"%itx[12]_\d", args[1]):
                conds["sint" if args[1].startswith("%itx2") else "uint"] = last
        assert set(conds) == {"sint", "uint"}, conds
        for kind in ("sint", "uint"):     # the sint selection encloses the uint one
            if self.value(conds[kind], word1, index):
                return kind
        return "float"


def format_class(fmt):
    name = rdna2spirv.GFX10_FORMATS.get(fmt, "")
    return "uint" if name.endswith("_UINT") else "sint" if name.endswith("_SINT") else "float"


class LoadOnlyShaders(unittest.TestCase):
    def test_2d_explicit_mip_reaches_every_typed_fetch(self):
        m = Module(translate(["image_load_mip v[0:3], v[4:6], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_2D",
                              "s_waitcnt vmcnt(0)"]))
        fetches = m.fetches()
        self.assertEqual({image[0] for _, _, image in fetches}, {"%float", "%uint", "%int"})
        for rid, var, _ in fetches:
            self.assertEqual(m.binding[var], 0)
            operands = m.defs[rid][1]
            self.assertEqual(operands[-2], "Lod")
            op, cast = m.defs[operands[-1]]
            self.assertEqual(op, "OpBitcast")
            op, load = m.defs[cast[1]]
            self.assertEqual(op, "OpLoad")
            op, source = m.defs[load[1]]
            self.assertEqual((op, source[1]), ("OpAccessChain", "%vgpr"))
            self.assertEqual(m.value(source[2], 0, 0), 6)

    def test_2d_explicit_mip_d16_packs_only_selected_components(self):
        listing = ["image_load_mip v0, v[4:6], s[0:7] dmask:0x9 dim:SQ_RSRC_IMG_2D d16",
                   "s_waitcnt vmcnt(0)"]
        m = Module(translate(listing))
        self.assertEqual({image[0] for _, _, image in m.fetches()}, {"%float", "%uint", "%int"})
        for rid, _, _ in m.fetches():
            bits, = (r for r, op, args in m.order if op == "OpBitcast" and args[1] == rid)
            components = [int(args[2]) for _, op, args in m.order if op == "OpCompositeExtract" and args[1] == bits]
            self.assertEqual(components, [0, 3])
        # All three branches write just v0: R in the low half and A in the high half.
        writes = [m.defs[ptr][1] for ptr in re.findall(r"^\s*OpStore (%\w+) ", m.text, re.MULTILINE)]
        vgprs = [m.value(pointer[2], 0, 0) for pointer in writes if len(pointer) > 2 and pointer[1] == "%vgpr"]
        self.assertEqual(vgprs.count(0), 3)
        self.assertNotIn(1, vgprs)

    def test_no_storage_image_capability_and_no_storage_binding(self):
        for dim in DIMS:
            with self.subTest(dim=dim):
                m = Module(load_shader(dim))
                self.assertFalse(m.caps & STORAGE_IMAGE_CAPS, sorted(m.caps))
                self.assertNotIn("OpImageRead", m.text)
                # Set 0 bindings 4 and 7-19 are storage images (runtime/src/vulkan_bindless.h).
                self.assertEqual(set(m.binding.values()), {0, 1, 2, DIMS[dim][1]})

    def test_each_component_type_is_fetched_from_the_sampled_binding_of_its_dimension(self):
        for dim, (_, binding, sdim, arrayed) in DIMS.items():
            with self.subTest(dim=dim):
                m = Module(load_shader(dim))
                seen = {}
                for _, var, image in m.fetches():
                    # OpTypeImage <sampled type> <Dim> <Depth> <Arrayed> <MS> <Sampled> <Format>
                    self.assertEqual(image[1:], [sdim, "0", arrayed, "0", "1", "Unknown"])
                    self.assertEqual(m.binding[var], binding)
                    seen[image[0]] = var
                self.assertEqual(set(seen), {"%int", "%uint", "%float"})
                self.assertEqual(len(set(seen.values())), 3, "one variable per component type")

    def test_the_tsharp_format_picks_the_fetch_and_a_miss_takes_the_float_one(self):
        """All 512 FORMAT values: the 30 UINT and 15 SINT GFX10 formats read as uint and sint when the T# was found
        (index 5), everything reads float on a miss (index 0). Bits outside FORMAT [28:20] don't matter."""
        m = Module(load_shader("2D"))
        counts = {"uint": 0, "sint": 0, "float": 0}
        for fmt in range(512):
            want = format_class(fmt)
            counts[want] += 1
            for other in (0, 0xE00FFFFF):
                word1 = (fmt << 20) | other
                self.assertEqual(m.branch(word1, 5), want, f"FORMAT {fmt} word1 {word1:#x}")
                self.assertEqual(m.branch(word1, 0), "float", f"FORMAT {fmt} miss")
        self.assertEqual(counts, {"uint": 30, "sint": 15, "float": 512 - 45})

    def test_only_the_dmask_components_are_written(self):
        m = Module(load_shader("2D", dmask=0x9))
        for rid, _, _ in m.fetches():
            uses = [args for _, op, args in m.order if op == "OpBitcast" and rid in args]
            self.assertEqual(len(uses), 1)
            bits = next(r for r, op, args in m.order if op == "OpBitcast" and args[1] == rid)
            comps = sorted(int(args[2]) for _, op, args in m.order if op == "OpCompositeExtract" and args[1] == bits)
            self.assertEqual(comps, [0, 3], "dmask 0x9: R into v0, A into v1")


class TypedGatherShaders(unittest.TestCase):
    def test_2d_gather_picks_the_sampled_type_and_selected_component(self):
        for dst, modifier in (("v[0:3]", ""), ("v[0:1]", " d16")):
            with self.subTest(modifier=modifier):
                m = Module(translate([f"image_gather4_lz {dst}, v[4:5], s[0:7], s[8:11] "
                                      f"dmask:0x2 dim:SQ_RSRC_IMG_2D{modifier}", "s_waitcnt vmcnt(0)"]))
                types = set()
                for rid, op, args in m.order:
                    if op != "OpImageGather":
                        continue
                    types.add(args[0])
                    self.assertEqual(m.value(args[3], 0, 0), 1)  # green
                    sampled_op, sampled = m.defs[args[1]]
                    self.assertEqual(sampled_op, "OpSampledImage")
                    image_load, image = m.defs[sampled[1]]
                    self.assertEqual(image_load, "OpLoad")
                    _, chain = m.defs[image[1]]
                    self.assertEqual(m.binding[chain[1]], 0)
                    if modifier and args[0] != "%v4float":
                        def uses(opcode, operand):
                            return [(r, a) for r, op, a in m.order if op == opcode and operand in a[1:]]
                        ((raw, _),) = uses("OpBitcast", rid)
                        taps = {int(a[2]): r for r, a in uses("OpCompositeExtract", raw)}
                        self.assertEqual(set(taps), {0, 1, 2, 3})
                        for tap in (0, 2):
                            ((lo, mask),) = uses("OpBitwiseAnd", taps[tap])
                            ((hi, shift),) = uses("OpShiftLeftLogical", taps[tap + 1])
                            self.assertEqual(m.value(mask[2], 0, 0), 0xffff)
                            self.assertEqual(m.value(shift[2], 0, 0), 16)
                            self.assertTrue(any(hi in a for _, a in uses("OpBitwiseOr", lo)))

                self.assertEqual(types, {"%v4uint_st", "%v4int_st", "%v4float"})


class StoreAndLoad(unittest.TestCase):
    def test_a_shader_that_also_stores_needs_only_the_write_feature(self):
        m = Module(translate(["v_mov_b32 v4, 1", "v_mov_b32 v5, 2",
                              "image_store v[0:3], v[4:5], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                              "image_load v[0:3], v[4:5], s[8:15] dmask:0xf dim:SQ_RSRC_IMG_2D unorm",
                              "s_waitcnt vmcnt(0)"]))
        self.assertIn("StorageImageWriteWithoutFormat", m.caps)
        self.assertNotIn("StorageImageReadWithoutFormat", m.caps)
        self.assertNotIn("OpImageRead", m.text)
        self.assertEqual(set(m.binding.values()), {0, 1, 2, 8, 9, 10})


def uav_array_load():
    return translate(["v_mov_b32 v4, 1", "v_mov_b32 v5, 2",
                      "image_load v[0:3], v[4:5], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_1D_ARRAY unorm",
                      "s_waitcnt vmcnt(0)"])


def volume_mip_load():
    return translate(["v_mov_b32 v4, 1", "v_mov_b32 v5, 2", "v_mov_b32 v6, 3", "v_mov_b32 v7, 1",
                      "image_load_mip v[0:3], v[4:7], s[0:7] dmask:0xf dim:SQ_RSRC_IMG_3D unorm",
                      "s_waitcnt vmcnt(0)"])


def mixed_d16_mip_loads():
    return translate(["image_load_mip v0, v[4:6], s[8:15] dmask:0x9 dim:SQ_RSRC_IMG_2D unorm d16",
                      "image_load_mip v3, v[8:11], s[16:23] dmask:0x9 dim:SQ_RSRC_IMG_3D unorm d16",
                      "s_waitcnt vmcnt(0)"])


class TextureUavLoads(unittest.TestCase):
    def test_1d_array_load_uses_only_storage_bindings_and_the_read_feature(self):
        m = Module(uav_array_load())
        self.assertEqual(m.caps & STORAGE_IMAGE_CAPS, {"StorageImageReadWithoutFormat", "Image1D"})
        self.assertEqual(set(m.binding.values()), {0, 1, 2, 26, 27, 28})
        self.assertFalse(m.fetches())
        reads = {}
        for _, op, args in m.order:
            if op == "OpImageRead":
                _, load = m.defs[args[1]]
                _, chain = m.defs[load[1]]
                reads[m.binding[chain[1]]] = m.defs[load[0]][1]
        self.assertEqual(reads, {26: ["%float", "1D", "0", "1", "0", "2", "Unknown"],
                                 27: ["%uint", "1D", "0", "1", "0", "2", "Unknown"],
                                 28: ["%int", "1D", "0", "1", "0", "2", "Unknown"]})

    def test_volume_mip_load_rebases_each_sampled_alias_without_storage(self):
        m = Module(volume_mip_load())
        self.assertFalse(m.caps & STORAGE_IMAGE_CAPS)
        fetches = m.fetches()
        self.assertEqual({image[0] for _, _, image in fetches}, {"%int", "%uint", "%float"})
        for rid, var, _ in fetches:
            self.assertEqual(m.binding[var], 6)
            self.assertEqual(m.defs[m.defs[rid][1][2]][0], "OpCompositeInsert")
            self.assertTrue(any(op == "OpPhi" and rid in args for _, op, args in m.order))

    def test_mixed_d16_mips_keep_2d_lod_and_only_rebase_tagged_3d(self):
        m = Module(mixed_d16_mip_loads())
        self.assertFalse(m.caps & STORAGE_IMAGE_CAPS)
        self.assertNotIn("OpImageRead", m.text)
        self.assertEqual(set(m.binding.values()), {0, 1, 2, 6})
        guards, last = {}, None
        for rid, op, args in m.order:
            if op == "OpBranchConditional":
                last = args[0]
            elif op == "OpImageFetch":
                guards[rid] = last
        regs = {("%vgpr", r): value for r, value in {4: 3, 5: 4, 6: 2, 8: 5, 9: 6, 10: 0, 11: 0}.items()}
        types = {0: set(), 6: set()}
        flag, tag = 0x80000000, 0xd7800000 | (1 << 11) | 3    # WSize=2, FirstWSlice=3
        for rid, var, image in m.fetches():
            binding = m.binding[var]
            types[binding].add(image[0])
            _, (_, _, coords, operand, lod) = m.defs[rid]
            self.assertEqual(operand, "Lod")
            if binding == 0:
                self.assertEqual(m.value(coords, 0, 1, regs), (3, 4))
                self.assertEqual(m.value(lod, 0, 1, regs), 2)
                continue
            for t4, t7, z, level, expected in (
                (flag, tag, 1, 0, 4), (flag, tag, 2, 0, None), (flag, tag, 0xffffffff, 0, None),
                (flag, tag, 0, 1, None), (0, tag, 2, 1, 2), (flag, 0, 2, 1, 2), (0, 0, 2, 1, 2),
                (flag, tag | 0x00400000, 1, 0, 4), (flag, tag | 0x00400000, 2, 0, None),
            ):
                with self.subTest(ty=image[0], t4=t4, t7=t7, z=z, level=level):
                    regs.update({("%sgpr", 20): t4, ("%sgpr", 23): t7, ("%vgpr", 10): z, ("%vgpr", 11): level})
                    self.assertEqual(m.value(guards[rid], 0, 1, regs), expected is not None)
                    if expected is not None:
                        self.assertEqual(m.value(coords, 0, 1, regs), (5, 6, expected))
                        self.assertEqual(m.value(lod, 0, 1, regs), level)
        self.assertEqual(types, {0: {"%float", "%uint", "%int"}, 6: {"%float", "%uint", "%int"}})


@unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"), "needs spirv-as and spirv-val")
class Validation(unittest.TestCase):
    def test_spirv_val_accepts_every_dimension(self):
        shaders = {dim: load_shader(dim) for dim in DIMS}
        shaders.update({"1D_ARRAY": uav_array_load(), "3D_MIP": volume_mip_load(), "MIXED_D16_MIP": mixed_d16_mip_loads()})
        shaders["GATHER_D16"] = translate([
            "image_gather4_lz v[0:1], v[4:5], s[0:7], s[8:11] dmask:0x2 dim:SQ_RSRC_IMG_2D d16",
            "s_waitcnt vmcnt(0)",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            for dim, text in shaders.items():
                with self.subTest(dim=dim):
                    sa, sv = os.path.join(tmp, f"{dim}.spvasm"), os.path.join(tmp, f"{dim}.spv")
                    with open(sa, "w", encoding="utf-8") as f:
                        f.write(text)
                    result = rdna2spirv.assemble_spirv(sa, sv)
                    self.assertEqual(result.returncode, 0, result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
