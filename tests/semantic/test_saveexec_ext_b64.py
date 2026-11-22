"""The other 64-bit saveexec / wrexec kinds, run across every lane of a wave64.

RDNA2 ISA (SOP1), with EXEC the old EXEC:
  s_xor_saveexec_b64    D = EXEC; EXEC = S0 ^ EXEC.
  s_andn1_saveexec_b64  D = EXEC; EXEC = ~S0 & EXEC.
  s_orn1_saveexec_b64   D = EXEC; EXEC = ~S0 | EXEC.
  s_orn2_saveexec_b64   D = EXEC; EXEC = S0 | ~EXEC.
  s_nand_saveexec_b64   D = EXEC; EXEC = ~(S0 & EXEC).
  s_nor_saveexec_b64    D = EXEC; EXEC = ~(S0 | EXEC).
  s_xnor_saveexec_b64   D = EXEC; EXEC = ~(S0 ^ EXEC).
  s_andn1_wrexec_b64    EXEC = ~S0 & EXEC; D = EXEC (the new one).
  All of them: SCC = (EXEC != 0).

The programs, the S0/EXEC table (plus six pairs) and the interpreter run are the ones test_saveexec_b64.py uses for the
and/or/andn2 kinds; only the ops and their model differ. The expected values come from the ISA lines above, written as integers in
`model`, not from the translator. The b32 forms are in test_saveexec_ext_fixture.py.

The GPU test is runtime/test/shaders/cs_w64x_saveexec_ext.asm; its fixture also runs here. The --wave64-emulate
output needs Workgroup memory, which the interpreter rejects, so it is covered by the GPU test only.
"""
import unittest

import support
from test_saveexec_b64 import ALL64, BCNT_AT, CASES, FF1_AT, SCC_AT, SOURCES, fssm, program, run, s0_mask

# derivation in cs_w64x_saveexec_ext.asm: out[k] = code(new EXEC) << 16 | SCC << 15 | code(D), code = bcnt << 8 | ff1
# test_saveexec_b64.py's table plus pairs that give these kinds a result in one half only, or none:
CASES_EXT = CASES + (
    (1, 32, 32, 0x00000000FFFF0000),          # S0 = the high half: orn1 is low only
    (1, 32, 32, 0x00000000FFFFFFFF),          # S0 = the high half, EXEC = the low half: orn2 is high only
    (1, 0, 32, 0x00000000FFFFFFFF),           # S0 = EXEC = the low half: nand is high only
    (1, 0, 0, 0xFFFFFFFF00000000),            # S0 = every lane, EXEC = the high half: nand is low only
    (1, 0, 32, 0x0000FFFF00000000),           # S0 = the low half, EXEC = lanes 32..47: xnor is lanes 48..63
    (1, 64, 64, ALL64),                       # S0 = no lane, EXEC = every lane: orn2 is empty
)
EXPECTED_FIXTURE = [671133712, 604809224, 536905768, 537964544, 536914948, 605325368, 537960448, 405313576]
KINDS = {"s_xor_saveexec_b64": lambda s, e: s ^ e, "s_andn1_saveexec_b64": lambda s, e: ~s & e,
         "s_orn1_saveexec_b64": lambda s, e: ~s | e, "s_orn2_saveexec_b64": lambda s, e: s | ~e,
         "s_nand_saveexec_b64": lambda s, e: ~(s & e), "s_nor_saveexec_b64": lambda s, e: ~(s | e),
         "s_xnor_saveexec_b64": lambda s, e: ~(s ^ e), "s_andn1_wrexec_b64": lambda s, e: ~s & e}


def model(op, s0, old_exec):
    """(new EXEC, D, SCC) from the ISA's definition."""
    new_exec = KINDS[op](s0, old_exec) & ALL64
    return new_exec, (new_exec if "_wrexec_" in op else old_exec), int(new_exec != 0)


def code(mask):
    return bin(mask).count("1") << 8 | ((mask & -mask).bit_length() - 1 if mask else 0xFF)


class SaveexecExtB64(unittest.TestCase):
    def test_model_examples(self):
        # The ISA lines on the masks of cs_w64x_saveexec_ext.asm, by hand.
        lanes = lambda lo, hi: sum(1 << lane for lane in range(lo, hi))
        self.assertEqual(model("s_xor_saveexec_b64", lanes(0, 40), lanes(16, 64)),
                         (lanes(0, 16) | lanes(40, 64), lanes(16, 64), 1))
        self.assertEqual(model("s_andn1_saveexec_b64", lanes(0, 12), lanes(8, 48)), (lanes(12, 48), lanes(8, 48), 1))
        self.assertEqual(model("s_orn1_saveexec_b64", lanes(24, 64), lanes(40, 48)),
                         (lanes(0, 24) | lanes(40, 48), lanes(40, 48), 1))
        self.assertEqual(model("s_orn2_saveexec_b64", lanes(48, 64), lanes(0, 16) | lanes(32, 64)),
                         (lanes(16, 32) | lanes(48, 64), lanes(0, 16) | lanes(32, 64), 1))
        self.assertEqual(model("s_nand_saveexec_b64", lanes(0, 36), lanes(4, 48)),
                         (lanes(0, 4) | lanes(36, 64), lanes(4, 48), 1))
        self.assertEqual(model("s_nor_saveexec_b64", lanes(0, 20), lanes(56, 64)), (lanes(20, 56), lanes(56, 64), 1))
        self.assertEqual(model("s_xnor_saveexec_b64", lanes(32, 64), lanes(0, 16) | lanes(48, 64)),
                         (lanes(16, 32) | lanes(48, 64), lanes(0, 16) | lanes(48, 64), 1))
        self.assertEqual(model("s_andn1_wrexec_b64", lanes(0, 40), lanes(24, 64)), (lanes(40, 64), lanes(40, 64), 1))
        self.assertEqual(model("s_nor_saveexec_b64", ALL64, 0), (0, 0, 0))
        self.assertEqual(model("s_xnor_saveexec_b64", 0, ALL64), (0, ALL64, 0))

    def test_fixture_words_from_model(self):
        # The header's words, rebuilt from the model and the masks the fixture sets up.
        lanes = lambda lo, hi: sum(1 << lane for lane in range(lo, hi))
        setup = [("s_xor_saveexec_b64", lanes(0, 40), lanes(16, 64)),
                 ("s_andn1_saveexec_b64", lanes(0, 12), lanes(8, 48)),
                 ("s_orn1_saveexec_b64", lanes(24, 64), lanes(40, 48)),
                 ("s_orn2_saveexec_b64", lanes(48, 64), lanes(0, 16) | lanes(32, 64)),
                 ("s_nand_saveexec_b64", lanes(0, 36), lanes(4, 48)),
                 ("s_nor_saveexec_b64", lanes(0, 20), lanes(56, 64)),
                 ("s_xnor_saveexec_b64", lanes(32, 64), lanes(0, 16) | lanes(48, 64)),
                 ("s_andn1_wrexec_b64", lanes(0, 40), lanes(24, 64))]
        words = []
        for op, s0, old_exec in setup:
            new_exec, d, scc = model(op, s0, old_exec)
            words.append(code(new_exec) << 16 | scc << 15 | code(d))
        self.assertEqual(words, EXPECTED_FIXTURE)

    def test_every_lane(self):
        # S0 alternates between VCC and an SGPR pair from one pair to the next (the source is read by the same code
        # for every kind; test_saveexec_b64.py runs each pair with both).
        for op in KINDS:
            for k, (mul, add, mask, old_exec) in enumerate(CASES_EXT):
                src = SOURCES[k % 2]
                with self.subTest(op=op, src=src, s0=(mul, add, mask), exec=hex(old_exec)):
                    new_exec, d, scc = model(op, s0_mask(mul, add, mask), old_exec)
                    spv = support.translate_lines("cs_saveexec_ext_b64", program(op, src, mul, add, mask, old_exec),
                                                  fssm=fssm(64))
                    memory = run(spv)
                    got_exec = sum(memory[4 * lane] << lane for lane in range(64))
                    self.assertEqual(got_exec, new_exec, f"EXEC {got_exec:#018x}, ISA {new_exec:#018x}")
                    self.assertEqual(memory[SCC_AT], scc, "SCC")
                    self.assertEqual(memory[BCNT_AT], bin(d).count("1"), "population count of D")
                    lowest = (d & -d).bit_length() - 1 if d else 0xFFFFFFFF   # s_ff1 of 0 is -1
                    self.assertEqual(memory[FF1_AT], lowest, "lowest lane of D")

    def test_cases_cover_both_halves(self):
        # For each op the table has results in the low half only, the high half only, both and neither.
        for op in KINDS:
            seen = set()
            for mul, add, mask, old_exec in CASES_EXT:
                new_exec = model(op, s0_mask(mul, add, mask), old_exec)[0]
                seen.add((bool(new_exec & 0xFFFFFFFF), bool(new_exec >> 32)))
            self.assertEqual(seen, {(False, False), (True, False), (False, True), (True, True)}, op)

    def test_gpu_test_fixture(self):
        """cs_w64x_saveexec_ext.asm: the words check.py expects (lane 0 stores them)."""
        spv = support.translate_fixture("cs_w64x_saveexec_ext", fssm=fssm(64))
        memory = run(spv)
        self.assertEqual(sorted(memory), list(range(0, 32, 4)), "lane 0 stores out[0..7] only")
        self.assertEqual([memory[4 * k] for k in range(8)], EXPECTED_FIXTURE)


if __name__ == "__main__":
    unittest.main()
