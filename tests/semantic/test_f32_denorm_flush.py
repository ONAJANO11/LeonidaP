"""--f32-denorm-flush (SHADER-377): the game's MODE flushes f32 denormals, translated and executed.

The rule, as @Vonsek measured it on gfx1036 with tools/hw_oracle (finding on SHADER-377, docs/design.md "f32
denormals in the game's MODE"): with denorm_mode_32 = 0 RDNA2 reads a denormal f32 operand as a zero of its sign, and
flushes a denormal result of plain arithmetic to a zero of its sign, for v_cmp_*_f32, v_max/min/med3/max3/min3_f32,
v_mul_f32, v_fma_f32, v_cvt_flr_i32_f32 and v_sqrt_f32.

The expected words below are worked out by hand from that rule (a denormal operand is the zero of its sign; the
sign of a flushed result is the sign of the denormal), not taken from a GPU run. Each case runs twice:
  * without the option (the default) the denormal reaches the operation, so the table shows what changes;
  * with ``rdna2spirv.F32_DENORM_FLUSH`` set the rule applies.
Operations the finding does not list (``v_add_f32``) keep their denormals either way; the last test pins that
until someone measures them. No GPU and no game files.
"""
import gc
import os
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import support  # noqa: E402  (puts LEONIDA_TOOLS_DIR or tools/ on the path)

rdna2spirv = support.rdna2spirv

PZ, NZ = 0x00000000, 0x80000000
D_MIN, D_NEG_MAX = 0x00000001, 0x807FFFFF       # the smallest denormal; the largest negative one
D_HALF_MIN = 0x00400000                         # FLT_MIN / 2: a denormal that 0.5 * FLT_MIN produces
FLT_MIN, NEG_FLT_MIN = 0x00800000, 0x80800000
ONE, M1, HALF, TWO = 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000
PUSH = [0x1000, 0, 32, 0] + [0] * 28            # V#: base 0x1000, num_records 32

LOAD = """s_buffer_load_dwordx4 s[4:7], s[0:3], 0
s_waitcnt lgkmcnt(0)
v_mov_b32_e32 v1, s4
v_mov_b32_e32 v2, s5
v_mov_b32_e32 v3, s6
v_mov_b32_e32 v4, s7
v_mov_b32_e32 v0, 0
"""


def translate_text(body):
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "disasm"))
        src = os.path.join(tmp, "disasm", "ps_denorm.s")
        with open(src, "w", encoding="utf-8", newline="\n") as f:
            f.write("; ps_denorm\n" + "".join("\t" + ln.strip() + "\n" for ln in body.strip().splitlines()))
        with open(os.path.join(tmp, "ps_denorm.dxbc"), "wb") as f:
            f.write(support.STUB_DXBC)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", ResourceWarning)
            spv = rdna2spirv.translate(src)
            gc.collect()
        return spv


def run(lines, inputs, flush):
    """Run ``lines`` (writing v0, reading v1..v4 = inputs) with the option on or off; return the v0 word."""
    old = rdna2spirv.F32_DENORM_FLUSH
    rdna2spirv.F32_DENORM_FLUSH = flush
    try:
        spv = translate_text(LOAD + lines + "\nexp mrt0 v0, v0, v0, v0 done vm\ns_endpgm")
    finally:
        rdna2spirv.F32_DENORM_FLUSH = old
    mem = {0x1000 + 4 * i: w for i, w in enumerate((list(inputs) + [0] * 4)[:4])}
    return support.mrt0_words(spv, push_constants=PUSH, memory=mem)[0]


class DenormFlush(unittest.TestCase):
    def check(self, lines, inputs, kept, flushed):
        """``kept``: the word without the option; ``flushed``: the word with it."""
        with self.subTest(lines=lines.splitlines()[-1], inputs=[hex(w) for w in inputs]):
            self.assertEqual(run(lines, inputs, False), kept, "without the option")
            self.assertEqual(run(lines, inputs, True), flushed, "with the option")

    def test_option_is_off_by_default(self):
        self.assertFalse(rdna2spirv.F32_DENORM_FLUSH)

    def test_v_mul_f32_operands(self):
        for form in ("e32", "e64"):
            self.check(f"v_mul_f32_{form} v0, v1, v2", (D_MIN, ONE), D_MIN, PZ)
            self.check(f"v_mul_f32_{form} v0, v1, v2", (D_NEG_MAX, ONE), D_NEG_MAX, NZ)

    def test_v_mul_f32_inline_integer_constant_is_a_denormal(self):
        # The inline integer 1 used as an f32 operand is the word 0x00000001 (measured as such for SHADER-298).
        self.check("v_mul_f32_e32 v0, 1, v1", (ONE,), D_MIN, PZ)

    def test_v_mul_f32_result(self):
        self.check("v_mul_f32_e32 v0, v1, v2", (FLT_MIN, HALF), D_HALF_MIN, PZ)
        self.check("v_mul_f32_e32 v0, v1, v2", (NEG_FLT_MIN, HALF), 0x80400000, NZ)
        self.check("v_mul_f32_e32 v0, v1, v2", (FLT_MIN, TWO), 0x01000000, 0x01000000)   # a normal result stays

    def test_v_fma_f32(self):
        self.check("v_fma_f32 v0, v1, v2, v3", (FLT_MIN, HALF, PZ), D_HALF_MIN, PZ)          # denormal result
        self.check("v_fma_f32 v0, v1, v2, v3", (PZ, PZ, D_MIN), D_MIN, PZ)                   # denormal addend
        self.check("v_fma_f32 v0, v1, v2, v3", (ONE, ONE, ONE), 0x40000000, 0x40000000)    # normal values untouched

    def test_v_max_v_min_f32(self):
        # max(+0, -0) = +0 and min(+0, -0) = -0 once the denormal is the zero of its sign.
        for form in ("e32", "e64"):
            self.check(f"v_max_f32_{form} v0, v1, v2", (D_MIN, NZ), D_MIN, PZ)
            self.check(f"v_min_f32_{form} v0, v1, v2", (D_NEG_MAX, PZ), D_NEG_MAX, NZ)

    def test_v_max3_v_min3_f32(self):
        self.check("v_max3_f32 v0, v1, v2, v3", (D_MIN, NZ, NZ), D_MIN, PZ)
        self.check("v_min3_f32 v0, v1, v2, v3", (D_NEG_MAX, PZ, PZ), D_NEG_MAX, NZ)

    def test_v_med3_f32(self):
        # med3(tiny, -1, 1) is tiny, which reads as +0.
        self.check("v_med3_f32 v0, v1, v2, v3", (D_MIN, M1, ONE), D_MIN, PZ)

    def test_v_cmp_f32(self):
        for form_line, kept, flushed in (
            ("v_cmp_eq_f32_e64 s10, v1, v2", 0, 1),      # tiny == 0 is true once flushed
            ("v_cmp_lt_f32_e64 s10, v2, v1", 1, 0),      # 0 < tiny is false once flushed
            ("v_cmp_gt_f32_e64 s10, v1, v2", 1, 0),
        ):
            self.check(form_line + "\nv_cndmask_b32_e64 v0, 0, 1, s10", (D_MIN, PZ), kept, flushed)

    def test_v_cvt_flr_i32_f32(self):
        # floor(-denormal) is -1; the flushed operand is -0, whose floor is 0.
        self.check("v_cvt_flr_i32_f32_e32 v0, v1", (D_NEG_MAX,), 0xFFFFFFFF, 0)

    def test_v_sqrt_f32(self):
        got_kept = run("v_sqrt_f32_e32 v0, v1", (D_MIN,), False)
        self.assertNotEqual(got_kept, PZ, "sqrt of a denormal is a small normal number without the option")
        self.assertEqual(run("v_sqrt_f32_e32 v0, v1", (D_MIN,), True), PZ)

    def test_operations_the_finding_does_not_list_keep_their_denormals(self):
        # v_add_f32 is not in the measured list: it is left alone, with or without the option.
        self.check("v_add_f32_e32 v0, v1, v2", (D_MIN, PZ), D_MIN, D_MIN)


if __name__ == "__main__":
    unittest.main()
