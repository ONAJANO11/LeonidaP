"""Which scalar registers hold a lane mask?

On hardware a lane mask is one register for the whole wave, a bit per lane. The translator runs one invocation
per lane and keeps only that lane's bit in the register (all ones or zero), which is all that and/or/not, selects
and EXEC need. A few instructions look at the mask as a whole instead: s_bcnt1 (how many lanes), s_ff1 (the first
lane), v_mbcnt (how many lanes below this one). For those the translator has to collect the other lanes' bits.
The same instructions also run on plain 64-bit data, such as a light mask read from a buffer, and nothing in the
instruction says which of the two it has, so this module works it out from what wrote the register.

    masks, writes = analyse(blocks, succ)   # blocks: lists of instructions; succ: successor block numbers per block
    masks[id(ins)]                      # dict mapping each register holding a lane mask on every path to ins
                                        # (SGPR number, "vcc", ("lane", vgpr, lane) parked in a VGPR lane, or
                                        # ("v", vgpr) passing through a VGPR) to its kind (MASK, MASK_LO, MASK_HI)
    id(ins) in writes                   # ins writes a mask to a VGPR

A register counts as a mask only if every path to the instruction wrote a mask to it. Anything else is treated
as data, which is what the translator did for all of them before.

Parked masks: a shader that runs out of SGPRs moves some into single lanes of a spare VGPR (v_writelane_b32) and
fetches them back later (v_readlane_b32). When the SGPR is a saved EXEC, the value that comes back has to be this
lane's bit again, not the bit of the lane it was parked in.

Masks through a VGPR: the compiler does some scalar work in the vector unit, such as an OR of three SGPRs
(v_mov_b32 / v_or3_b32, then v_readfirstlane_b32 to get the scalar back). On hardware every lane computes the same
whole mask and the first lane's copy is read. Here each lane computes its own bit, so it has to read its own
copy, and it has to compute it whether or not it is active.
"""

MASK = "mask"           # full lane mask (64-bit pair, vcc, or wave32 single register)
MASK_LO = "mask_lo"     # low 32-bit half of a 64-bit lane mask (lanes 0-31)
MASK_HI = "mask_hi"     # high 32-bit half of a 64-bit lane mask (lanes 32-63)
MASK_KINDS = (MASK, MASK_LO, MASK_HI)
EITHER = "either"       # a constant 0 or -1: no lanes or all lanes, and the same bits when read as data
# Absent from the state: data.

COPIES = ("s_mov_b", "s_not_b", "s_wqm_b")
BITWISE = ("s_and_b", "s_or_b", "s_xor_b", "s_andn2_b", "s_orn2_b", "s_nand_b", "s_nor_b", "s_xnor_b",
           "s_cselect_b")
NO_DESTINATION = ("s_cmp", "s_bitcmp", "v_cmpx", "s_set", "s_rfe")      # their first operand is a source
VECTOR_BITWISE = ("v_mov_b32", "v_or3_b32", "v_or_b32", "v_and_b32", "v_xor_b32", "v_xor3_b32", "v_not_b32")


def base_op(op):
    for suffix in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suffix):
            return op[:-len(suffix)]
    return op


def kind(state, a):
    if a["k"] == "special":
        t = a.get("t", "")
        if t == "exec_lo":
            return MASK_LO
        if t == "exec_hi":
            return MASK_HI
        if t == "exec":
            return MASK
        if t == "vcc":
            return state.get("vcc")
        if t in ("vcc_lo", "vcc_hi"):
            vk = state.get("vcc")
            if vk in MASK_KINDS:
                return MASK_LO if t == "vcc_lo" else MASK_HI
            return vk if t == "vcc_lo" else None
        return None
    if a["k"] == "sgpr":
        n = a.get("n", 1)
        ks = [state.get(a["r"] + i) for i in range(n)]
        if None in ks:
            return None
        if n == 2 and any(k in MASK_KINDS for k in ks):
            return MASK
        return ks[0] if len(set(ks)) == 1 else None
    if a["k"] == "vgpr" and a.get("n", 1) == 1:
        return state.get(("v", a["r"]))
    if a["k"] == "imm" and a["v"] in (0, -1):
        return EITHER
    return None


def put(state, a, k):
    if a["k"] == "sgpr":
        n = a.get("n", 1)
        if k == MASK and n == 2:
            state[a["r"]] = MASK_LO
            state[a["r"] + 1] = MASK_HI
            return
        regs = [a["r"] + i for i in range(n)]
    elif a["k"] == "special" and a.get("t") in ("vcc", "vcc_lo"):
        regs = ["vcc"]
    elif a["k"] == "special" and a.get("t") == "vcc_hi":
        # A separate write to vcc_hi lands in %vcc_hi, so vcc no longer holds one lane mask.
        state.pop("vcc", None)
        return
    else:
        return
    for r in regs:
        if k is None:
            state.pop(r, None)
        else:
            state[r] = k


def lane_slot(vgpr, lane):
    """State key for a mask parked in one lane of a VGPR, or None unless both operands are plain."""
    if vgpr["k"] != "vgpr" or lane["k"] != "imm":
        return None
    return ("lane", vgpr["r"], lane["v"] & 63)


def step(state, ins):
    """Update `state` (register -> MASK | MASK_LO | MASK_HI | EITHER) for one instruction. True if it writes a mask to a VGPR."""
    op, a = base_op(ins["op"]), ins["args"]
    if not a or op.startswith(NO_DESTINATION):
        return
    if op == "v_readfirstlane_b32" and len(a) == 2:                             # sD = vS, the same in every lane
        put(state, a[0], kind(state, a[1]))
        return
    if op == "v_writelane_b32" and len(a) == 3 and lane_slot(a[0], a[2]):       # vD[lane] = sS
        k = kind(state, a[1])
        if k is None:
            state.pop(lane_slot(a[0], a[2]), None)
        else:
            state[lane_slot(a[0], a[2])] = k
        return
    if op == "v_readlane_b32" and len(a) == 3:                                  # sD = vS[lane]
        put(state, a[0], state.get(lane_slot(a[1], a[2])))
        return
    if op.startswith("v_cmp"):
        # Handle v_cmp before VGPR clobber (defensive for 2-operand forms with implicit vcc).
        put(state, {"k": "special", "t": "vcc"} if len(a) == 2 else a[0], MASK)
        return
    if a[0]["k"] == "vgpr":                             # any other write to a VGPR ends what was parked in it
        # A mask only when every source is one (or a constant 0 / -1) on every path: no EITHER for VGPRs, whose
        # plain writes skip inactive lanes. The sources are looked at first: the destination can be one of them.
        kinds = [kind(state, x) for x in a[1:]] if op in VECTOR_BITWISE and a[0].get("n", 1) == 1 else [None]
        written = range(a[0]["r"], a[0]["r"] + a[0].get("n", 1))
        for key in [k for k in state if isinstance(k, tuple) and k[1] in written]:
            del state[key]
        if None not in kinds and any(k in MASK_KINDS for k in kinds):
            ms = {k for k in kinds if k in MASK_KINDS}
            state[("v", a[0]["r"])] = ms.pop() if len(ms) == 1 else MASK
            return True
    if "_saveexec_b" in op or "_wrexec_b" in op or op == "s_bfm_b64":
        put(state, a[0], MASK)
    elif op.startswith(COPIES) and len(a) > 1:
        put(state, a[0], kind(state, a[1]))
    elif op.startswith(BITWISE):
        kinds = [kind(state, x) for x in a[1:]]
        if None in kinds:
            put(state, a[0], None)
        else:
            ms = {k for k in kinds if k in MASK_KINDS}
            put(state, a[0], (ms.pop() if len(ms) == 1 else MASK) if ms else EITHER)
    else:
        put(state, a[0], None)
        if ((op.startswith("v_") and "_co_" in op) or op in ("v_mad_u64_u32", "v_mad_i64_i32")) and len(a) > 1:
            put(state, a[1], MASK)                      # carry/sign lane mask of v_add_co_u32, v_mad_u64_u32, etc.


# Instructions that read a lane mask as a mask: they look at it lane by lane (cndmask selector, carry-in, EXEC
# save), count or find lanes in it (bcnt, ff1, mbcnt, through the translator's wave ballot), compare it, or carry it
# to another register that is still a mask (copies, parking in a VGPR lane). Their mask operands stay one bit per
# lane. The mask operand of v_cndmask and of the carry-in ops is handled by position below.
MASK_READERS = ("s_bcnt", "s_ff0", "s_ff1", "v_mbcnt", "s_bfm", "v_readfirstlane", "v_readlane", "v_writelane") + \
    COPIES + NO_DESTINATION
CARRY_IN = ("v_addc", "v_subb", "v_subbrev", "_co_ci_")
# First operand is a source: the data a store writes (an atomic's data, which it may also return into).
STORES = ("buffer_store", "tbuffer_store", "global_store", "flat_store", "scratch_store", "ds_write", "ds_store",
          "image_store", "buffer_atomic", "global_atomic", "flat_atomic", "image_atomic", "s_store", "s_buffer_store")


def reg_keys(a):
    """The 32-bit registers operand `a` reads: ("sgpr", n), ("vgpr", n) or ("special", name)."""
    if a["k"] in ("sgpr", "vgpr"):
        return [(a["k"], a["r"] + i) for i in range(a.get("n", 1))]
    if a["k"] == "special" and a.get("t") in ("exec_lo", "exec_hi", "vcc_lo", "vcc_hi"):
        return [("special", a["t"])]
    if a["k"] == "special" and a.get("t") in ("exec", "vcc"):
        return [("special", a["t"] + "_lo"), ("special", a["t"] + "_hi")]
    return []


def reg_kind(state, key):
    """The mask kind of one 32-bit register: MASK_LO (lanes 0-31), MASK_HI (lanes 32-63), MASK (a wave32 mask, or a
    VGPR mixing both halves), or None."""
    k, r = key
    if k == "special":
        if r.startswith("exec"):
            return MASK_LO if r == "exec_lo" else MASK_HI
        vk = state.get("vcc")
        if vk not in MASK_KINDS:
            return None
        return MASK_LO if r == "vcc_lo" else MASK_HI
    k = state.get(r if k == "sgpr" else ("v", r))
    return k if k in MASK_KINDS else None


def data_reads(ins, state):
    """The registers that hold a lane mask but that `ins` reads as plain 32-bit data, as {key: kind} (keys as in
    reg_keys). On hardware such a read sees the wave's bits (0xfafafafa, say); here the register holds this lane's
    bit, so the translator has to put the wave's bits together first (#210). Mask readers (MASK_READERS, and
    bitwise ops whose result is still a mask) are left alone, and so are instructions that write EXEC or VCC, which
    take a value as this lane's bit."""
    op, a = base_op(ins["op"]), ins["args"]
    if not a or op.startswith(MASK_READERS) or "_saveexec_b" in op or "_wrexec_b" in op:
        return {}
    if op.startswith(("s_", "v_")) and not op.startswith("v_cmp") and a[0]["k"] == "special" and \
            a[0].get("t", "").startswith(("exec", "vcc")):
        return {}
    if op.startswith(BITWISE) or (op in VECTOR_BITWISE and a[0].get("n", 1) == 1):
        kinds = [kind(state, x) for x in a[1:]]
        if None not in kinds and any(k in MASK_KINDS for k in kinds):
            return {}                                   # the result is a mask too
    if op.startswith(STORES):
        srcs = list(a)                                  # no destination operand
    elif op.startswith("v_") and "_co_" in op and len(a) > 2:
        srcs = a[2:]                                    # vD, carry-out, then the sources
    elif op.startswith("v_cmp") and len(a) == 2:
        srcs = list(a)                                  # implicit VCC destination
    else:
        srcs = a[1:]
    if op.startswith("v_cndmask") and len(srcs) > 2:
        srcs = srcs[:2]                                 # the selector is a mask
    if any(c in op for c in CARRY_IN) and len(srcs) > 2:
        srcs = srcs[:-1]                                # the carry-in is a mask
    out = {}
    for x in srcs:
        for key in reg_keys(x):
            k = reg_kind(state, key)
            if k is not None:
                out[key] = k
    return out


def parked(blocks, masks):
    """The (vgpr, lane) slots some v_readlane_b32 fetches a mask from, given analyse()'s result."""
    found = set()
    for blk in blocks:
        for ins in blk:
            a = ins["args"]
            if base_op(ins["op"]) == "v_readlane_b32" and len(a) == 3:
                slot = lane_slot(a[1], a[2])
                if slot in masks.get(id(ins), ()):
                    found.add(slot[1:])
    return sorted(found)


def analyse(blocks, succ, reads=None):
    """masks, writes as described at the top. When `reads` is a dict, it also gets id(ins) -> data_reads(ins)
    for every instruction that reads a lane mask as data."""
    entry = [None] * len(blocks)                        # None: not reached yet
    if blocks:
        entry[0] = {}
    work = [0] if blocks else []
    while work:
        n = work.pop()
        state = dict(entry[n])
        for ins in blocks[n]:
            step(state, ins)
        for s in succ[n]:
            if entry[s] is None:
                merged = dict(state)
            else:                                       # a mask on both paths, or a mask on one and 0 / -1 on the other
                merged = {}
                for r in entry[s].keys() & state.keys():
                    a_k, b_k = entry[s][r], state[r]
                    if a_k == b_k:
                        merged[r] = a_k
                    elif a_k == EITHER:
                        merged[r] = b_k
                    elif b_k == EITHER:
                        merged[r] = a_k
                    else:
                        merged[r] = MASK
            if merged != entry[s]:
                entry[s] = merged
                work.append(s)
    out, writes = {}, set()
    for n, blk in enumerate(blocks):
        state = dict(entry[n] or {})
        for ins in blk:
            out[id(ins)] = {r: k for r, k in state.items() if k in MASK_KINDS}
            if reads is not None:
                dr = data_reads(ins, state)
                if dr:
                    reads[id(ins)] = dr
            if step(state, ins):
                writes.add(id(ins))
    return out, writes
