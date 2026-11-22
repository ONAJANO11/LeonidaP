"""LDS accesses between the lanes of one wave (docs/design.md "LDS between the lanes of one wave"): the subgroup
barrier lds_wave_order emits, where it goes and where it must not.

Standard library only; no GPU, no LLVM, no game files. The GPU tests are runtime/test/shaders/cs_lds_wave_exchange.asm
(wave32) and cs_w64x_lds_wave_exchange.asm (wave64).

Why: on RDNA2 a wave runs in lockstep and its LDS queue is in order, so a lane reads what another lane of the wave wrote
with only an s_waitcnt (or nothing) between ds_write and ds_read. SPIR-V (Vulkan memory model) orders nothing between two
invocations without a barrier. The translator inserts `OpControlBarrier Subgroup Subgroup AcquireRelease|WorkgroupMemory`
(`%c3 %c3 %c108`) between two LDS accesses of a wave that conflict (read after write, write after read, write after
write, atomic against a plain access) when no s_barrier or earlier subgroup barrier has ordered them yet. The expected
placements below are written from that rule, not read back from the translator.
"""
import re
import struct
import unittest

import support

rdna2spirv = support.rdna2spirv

SUBGROUP_BARRIER = "OpControlBarrier %c3 %c3 %c108"
WORKGROUP_BARRIER = "OpControlBarrier %c2 %c2 %c108"


def fssm(threads=64):
    h = bytearray(0x148)
    struct.pack_into("<4I", h, 0x138, 0x8088, threads, 1, 1)
    return bytes(h)


def translate(lines, emulate=False, on=True):
    """`on`: the --lds-wave-order option (the tests below are about what it does; default_off_* is about off)."""
    prev = rdna2spirv.WAVE64_EMULATE, rdna2spirv.LDS_WAVE_ORDER
    rdna2spirv.WAVE64_EMULATE, rdna2spirv.LDS_WAVE_ORDER = emulate, on
    try:
        # Branch targets need the size of every instruction: an encoding comment of 8 bytes for DS and VOP3, else 4.
        def enc(ln):
            wide = ln.startswith(("ds_", "v_mbcnt")) or "_e64" in ln
            return f"\t{ln} ; encoding: [" + ",".join(["0x00"] * (8 if wide else 4)) + "]\n"
        text = "; cs_lds_order_t  hand-written test program\n" + "".join(enc(ln) for ln in lines + ["s_endpgm"])
        return support.translate_listing("cs_lds_order_t", text.encode("utf-8"), fssm=fssm())
    finally:
        rdna2spirv.WAVE64_EMULATE, rdna2spirv.LDS_WAVE_ORDER = prev


def count(spv, barrier=SUBGROUP_BARRIER):
    return sum(1 for ln in spv.splitlines() if ln.strip() == barrier)


def lds_events(spv):
    """The LDS accesses and barriers in program order: 'w' (OpStore through %lds), 'r' (OpLoad through it), 'a'
    (an atomic on it), 'sub' / 'wg' (the two barriers)."""
    ptrs, events = set(), []
    for ln in spv.splitlines():
        s = ln.strip()
        m = re.match(r"(%\w+) = OpAccessChain %_ptr_WG_uint %lds ", s)
        if m:
            ptrs.add(m.group(1))
        elif s == SUBGROUP_BARRIER:
            events.append("sub")
        elif s == WORKGROUP_BARRIER:
            events.append("wg")
        elif (m := re.match(r"OpStore (%\w+) ", s)) and m.group(1) in ptrs:
            events.append("w")
        elif (m := re.match(r"%\w+ = OpLoad %uint (%\w+)$", s)) and m.group(1) in ptrs:
            events.append("r")
        elif (m := re.match(r"%\w+ = OpAtomic\w+ %uint (%\w+) ", s)) and m.group(1) in ptrs:
            events.append("a")
    return events


def uniform_position(spv):
    """True if every subgroup barrier sits outside the 'if EXEC' selection of a DS instruction (labels *_do)."""
    inside = False
    for ln in spv.splitlines():
        s = ln.strip()
        if re.match(r"%\w+_do = OpLabel$", s):
            inside = True
        elif s.startswith("OpBranch %") and s.endswith("_end"):
            inside = False
        elif s == SUBGROUP_BARRIER and inside:
            return False
    return True


WRITE = ["ds_write_b32 v3, v2"]
READ = ["ds_read_b32 v5, v4", "s_waitcnt lgkmcnt(0)"]


class EmittedBarriers(unittest.TestCase):
    def check(self, lines, events, **kw):
        spv = translate(lines, **kw)
        self.assertEqual(lds_events(spv), events, lines)
        self.assertTrue(uniform_position(spv), "a subgroup barrier inside a divergent (EXEC) selection")
        return spv

    def test_read_after_write_gets_one_barrier_between(self):
        spv = self.check(WRITE + ["s_waitcnt lgkmcnt(0)"] + READ, ["w", "sub", "r"])
        # Scope Subgroup for execution and memory, AcquireRelease | WorkgroupMemory: what OpControlBarrier needs for
        # one invocation's write to be visible to the other lanes' read (no s_barrier, no workgroup scope).
        self.assertEqual(count(spv), 1)

    def test_no_waitcnt_in_between_is_the_same(self):
        self.check(WRITE + READ, ["w", "sub", "r"])

    def test_write_after_read(self):
        self.check(READ + WRITE, ["r", "sub", "w"])

    def test_write_after_write(self):
        self.check(WRITE + ["ds_write_b32 v3, v6"], ["w", "sub", "w"])

    def test_s_barrier_orders_it(self):
        spv = self.check(WRITE + ["s_waitcnt lgkmcnt(0)", "s_barrier"] + READ, ["w", "wg", "r"])
        self.assertEqual(count(spv), 0)

    def test_read_after_read_needs_none(self):
        self.check(READ + ["ds_read_b32 v6, v4"], ["r", "r"])

    def test_one_barrier_orders_a_whole_group_of_accesses(self):
        # w w r r: the barrier before the first read orders both writes; the second read needs nothing more.
        # (w w itself conflicts, so the second write gets one too.)
        self.check(WRITE + ["ds_write_b32 v3, v6", "ds_read_b32 v5, v4", "ds_read_b32 v7, v4"],
                   ["w", "sub", "w", "sub", "r", "r"])

    def test_a_chain_of_exchanges(self):
        # write, read, write, read: every access conflicts with the one before it
        spv = self.check(WRITE + READ + WRITE + READ, ["w", "sub", "r", "sub", "w", "sub", "r"])
        self.assertEqual(count(spv), 3)

    def test_atomic_after_atomic_needs_none(self):
        self.check(["ds_add_u32 v3, v2", "ds_add_u32 v4, v2"], ["a", "a"])

    def test_atomic_after_write_and_read_after_atomic(self):
        self.check(WRITE + ["ds_add_u32 v3, v2", "ds_read_b32 v5, v3"], ["w", "sub", "a", "sub", "r"])

    def test_returning_atomic_and_cmpst_count_as_atomics(self):
        self.check(WRITE + ["ds_cmpst_rtn_b32 v5, v3, v2, v6"], ["w", "sub", "a"])
        self.check(WRITE + ["ds_wrxchg_rtn_b32 v5, v3, v2"], ["w", "sub", "a"])

    def test_sub_dword_write_is_a_write(self):
        # a b8 write is an atomic and + or on the dword (so neighbouring lanes don't race), still a write here
        self.check(READ + ["ds_write_b8 v3, v2"], ["r", "sub", "a", "a"])

    def test_no_lds_no_barrier(self):
        spv = translate(["v_add_nc_u32_e32 v1, v0, v0"])
        self.assertEqual(count(spv), 0)
        self.assertNotIn("OpControlBarrier", spv)

    def test_register_permutes_are_not_memory(self):
        spv = translate(WRITE + ["ds_bpermute_b32 v5, v4, v2", "ds_swizzle_b32 v6, v2 offset:swizzle(SWAP,16)"])
        self.assertEqual(count(spv), 0)

    def test_wave64_native_has_it_too(self):
        wave64 = ["v_mbcnt_lo_u32_b32 v1, -1, 0", "v_mbcnt_hi_u32_b32 v1, -1, v1"]
        spv = self.check(wave64 + WRITE + READ, ["w", "sub", "r"])
        self.assertEqual(count(spv), 1)

    def test_emulated_wave64_keeps_its_own_ordering(self):
        wave64 = ["v_mbcnt_lo_u32_b32 v1, -1, 0", "v_mbcnt_hi_u32_b32 v1, -1, v1"]
        spv = translate(wave64 + WRITE + READ, emulate=True)
        self.assertEqual(count(spv), 0)       # x_memory_order starts a round, whose workgroup barrier orders it
        self.assertIn("OpControlBarrier %c2 %c2 %c948", spv)

    def setup_stage(self, stage, ops=("ds_write_b32", "ds_read_b32", "s_endpgm"), on=True):
        prev, rdna2spirv.LDS_WAVE_ORDER = rdna2spirv.LDS_WAVE_ORDER, on
        try:
            g = rdna2spirv.Gen(stage=stage)
            rdna2spirv.lds_order_setup(g, [blk(*ops)], [[]])
        finally:
            rdna2spirv.LDS_WAVE_ORDER = prev
        return g

    def test_mesh_stage_is_covered_by_the_same_rule(self):
        g = self.setup_stage("ms")             # through the stage condition of lds_order_setup
        self.assertTrue(g.lds_order)
        for op in ("ds_write_b32", "ds_read_b32"):
            g.lds_wave_order(op)
        self.assertEqual([ln.strip() for ln in g.body], [SUBGROUP_BARRIER])

    def test_pixel_stage_is_untouched_even_with_lds_in_the_code(self):
        g = self.setup_stage("ps")
        self.assertFalse(g.lds_order)
        self.assertEqual(g.lds_barriers, 0)

    def test_option_off_enables_nothing_for_any_stage(self):
        for stage in ("cs", "ms", "ps"):
            self.assertFalse(self.setup_stage(stage, on=False).lds_order, stage)

    def test_default_off_leaves_a_conflicting_pair_unchanged_and_on_adds_the_barrier(self):
        self.assertFalse(rdna2spirv.LDS_WAVE_ORDER)             # the module default is off
        pair = WRITE + READ
        off, on = translate(pair, on=False), translate(pair)
        self.assertEqual(count(off), 0)
        self.assertNotIn("%c108", off)                          # not even the constant of the barrier
        self.assertEqual(lds_events(off), ["w", "r"])
        self.assertEqual(count(on), 1)
        self.assertEqual(lds_events(on), ["w", "sub", "r"])
        # everything else is the same: the on output minus the barrier line (and its constants) is the off output
        strip = [ln for ln in on.splitlines() if ln.strip() != SUBGROUP_BARRIER and "%c108 =" not in ln]
        self.assertEqual("\n".join(strip).count("OpControlBarrier"), 0)
        self.assertEqual(len([ln for ln in strip if "OpStore" in ln]), len([ln for ln in off.splitlines() if "OpStore" in ln]))

    def test_metadata_records_whether_it_was_applied(self):
        for on, key in ((True, 1), (False, None)):
            spv = translate(WRITE + READ, on=on)
            meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, spv)
            self.assertEqual(meta.get("lds_wave_order"), key, on)
        meta = rdna2spirv.shader_meta(rdna2spirv.translate.last_gen, translate(["v_mov_b32 v1, 1"]))
        self.assertNotIn("lds_wave_order", meta)                # no LDS access: the rule was not in play

    # Control flow, in the translator's listing format: a branch takes a dword count relative to the next pc.
    def test_loop_carried_read_orders_the_write_at_the_loop_head(self):
        # pc 0 ds_write (2 dwords), 2 ds_read (2), 4 s_waitcnt, 5 s_cbranch_scc1 -6 (back to pc 0), 6 s_endpgm
        spv = translate(WRITE + ["ds_read_b32 v5, v4", "s_waitcnt lgkmcnt(0)", "s_cbranch_scc1 -6"])
        # the read of one trip is unordered against the write of the next: a barrier before the write, and the
        # usual one between write and read
        self.assertEqual(lds_events(spv), ["sub", "w", "sub", "r"])
        self.assertTrue(uniform_position(spv))

    def test_join_takes_the_union_of_both_paths(self):
        # 0 s_cmp, 1 s_cbranch_scc1 2 (skips the write, to pc 4), 2 ds_write (2), 4 ds_read (2): the read is the join
        # of a path with the write and a path without it
        spv = translate(["s_cmp_lg_u32 s6, 0", "s_cbranch_scc1 2"] + WRITE[:1] + ["ds_read_b32 v5, v4"])
        self.assertEqual(lds_events(spv), ["w", "sub", "r"])

    def test_s_barrier_in_the_predecessor_orders_the_join(self):
        spv = translate(WRITE + ["s_barrier", "s_cmp_lg_u32 s6, 0", "s_cbranch_scc1 2", "ds_read_b32 v5, v4",
                                 "ds_read_b32 v6, v4"])
        self.assertEqual(count(spv), 0)

    def test_wave64_scc_from_a_half_mask_gets_no_barrier_behind_the_branch(self):
        # the exception of the design.md row: a wave64 s_and_b32 on exec_lo / vcc_lo takes the lane's own bit as SCC,
        # so the lanes of a subgroup may split at s_cbranch_scc1 and a subgroup barrier behind it is not reached by all
        wave64 = ["v_mbcnt_lo_u32_b32 v1, -1, 0", "v_mbcnt_hi_u32_b32 v1, -1, v1"]
        body = ["s_cbranch_scc1 4"] + WRITE + ["ds_read_b32 v5, v4"]
        risky = translate(wave64 + ["s_and_b32 s6, vcc_lo, exec_lo"] + body)
        self.assertEqual(count(risky), 0)
        self.assertEqual(lds_events(risky), ["w", "r"])
        plain = translate(wave64 + ["s_cmp_lg_u32 s6, 0"] + body)       # SCC of a plain scalar compare: uniform
        self.assertEqual(lds_events(plain), ["w", "sub", "r"])
        # a conflicting pair before the branch (a block that runs for the whole wave) still gets its barrier
        before = translate(wave64 + WRITE + ["ds_read_b32 v5, v4", "s_and_b32 s6, vcc_lo, exec_lo", "s_cbranch_scc1 0"])
        self.assertEqual(lds_events(before), ["w", "sub", "r"])


def blk(*ops):
    return [{"op": o} for o in ops]


class EntryStates(unittest.TestCase):
    """lds_entry_states: what may still be unordered at the start of each block (forward data flow)."""

    def test_no_lds_is_none(self):
        self.assertIsNone(rdna2spirv.lds_entry_states([blk("v_mov_b32", "s_endpgm")], [[]]))

    def test_write_flows_into_the_next_block(self):
        e = rdna2spirv.lds_entry_states([blk("ds_write_b32", "s_branch"), blk("ds_read_b32", "s_endpgm")], [[1], []])
        self.assertEqual(e[0], frozenset())
        self.assertEqual(e[1], frozenset("w"))

    def test_s_barrier_stops_it(self):
        e = rdna2spirv.lds_entry_states([blk("ds_write_b32", "s_barrier", "s_branch"), blk("ds_read_b32", "s_endpgm")],
                                        [[1], []])
        self.assertEqual(e[1], frozenset())

    def test_a_barrier_emitted_for_a_conflict_stops_it(self):
        # w then r: the barrier before the read orders the write, only the read stays unordered
        e = rdna2spirv.lds_entry_states([blk("ds_write_b32", "ds_read_b32", "s_branch"), blk("s_endpgm")], [[1], []])
        self.assertEqual(e[1], frozenset("r"))

    def test_loop_back_edge_carries_the_read_to_the_loop_head(self):
        # b0 -> b1 (loop: write, read) -> b1 | b2: the read of one trip is unordered against the write of the next
        blocks = [blk("s_branch"), blk("ds_write_b32", "ds_read_b32", "s_cbranch_scc1"), blk("s_endpgm")]
        e = rdna2spirv.lds_entry_states(blocks, [[1], [1, 2], []])
        self.assertEqual(e[1], frozenset("r"))
        self.assertEqual(e[2], frozenset("r"))

    def test_join_is_the_union(self):
        blocks = [blk("s_cbranch_scc1"), blk("ds_write_b32", "s_branch"), blk("ds_read_b32", "s_branch"), blk("s_endpgm")]
        e = rdna2spirv.lds_entry_states(blocks, [[1, 2], [3], [3], []])
        self.assertEqual(e[3], frozenset("wr"))


if __name__ == "__main__":
    unittest.main()
