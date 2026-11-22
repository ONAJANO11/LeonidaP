"""test_top10_betrayals.py: value tests for 11 RDNA2 ops the translator used to refuse."""
import os
import struct
import sys
import tempfile
import unittest
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLS_DIR = os.environ.get("LEONIDA_TOOLS_DIR") or os.path.join(ROOT, "tools")

sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS_DIR)
sys.path.insert(0, HERE)

import rdna2spirv  # noqa: E402
import spirv_mini_interp as si  # noqa: E402

STUB_DXBC = b"DXBC" + bytes(24) + struct.pack("<I", 0) + bytes(4)
EXPORT = ["v_and_b32 v5, 0xffff, v0", "v_lshrrev_b32 v6, 16, v0", "v_or_b32 v5, 0x40000000, v5",
          "v_or_b32 v6, 0x40000000, v6"]
_TMP = None


def setUpModule():
    global _TMP
    _TMP = tempfile.TemporaryDirectory()
    os.mkdir(os.path.join(_TMP.name, "disasm"))
    with open(os.path.join(_TMP.name, "ps_top10.dxbc"), "wb") as f:
        f.write(STUB_DXBC)


def tearDownModule():
    _TMP.cleanup()


def run(lines):
    src = os.path.join(_TMP.name, "disasm", "ps_top10.s")
    body = list(lines) + EXPORT
    with open(src, "w", encoding="utf-8") as f:
        f.write("; top10\n" + "".join(f"\t{ln}\n" for ln in body) + "\texp mrt0 v5, v6, v5, v5 done vm\n\ts_endpgm\n")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", ResourceWarning)
        spv = rdna2spirv.translate(src)
    w = si.run_fragment(spv)["%out_mrt0"]
    return (w[0] & 0xFFFF) | (w[1] & 0xFFFF) << 16


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def bits_f32(b):
    return struct.unpack("<f", struct.pack("<I", b))[0]


def h2f(h):
    return struct.unpack("<e", struct.pack("<H", h & 0xFFFF))[0]


def f2h(x):
    return struct.unpack("<H", struct.pack("<e", x))[0]


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def sat(v, lo, hi):
    return max(lo, min(hi, v))


def norm(f, snorm):
    if f != f:
        return 0
    v = bits_f32(f32(sat(f, -1.0 if snorm else 0.0, 1.0) * (32767.0 if snorm else 65535.0)))
    return round(v) & 0xFFFF


HALVES = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FF0, 0x8010, 0x1234, 0xFFFE, 0x4000]
F16 = [0x0000, 0x8000, 0x3C00, 0xBC00, 0x3800, 0xB800, 0x3555, 0x4000, 0xC000, 0x7C00, 0xFC00, 0x7E00, 0x0001, 0x3BFF]


class Top10Betrayals(unittest.TestCase):
    def test_add_sub_nc_i16(self):
        for a in HALVES:
            for b in HALVES:
                movs = [f"v_mov_b32 v1, 0x{a:x}", f"v_mov_b32 v2, 0x{b:x}"]
                with self.subTest(a=hex(a), b=hex(b)):
                    self.assertEqual(run(movs + ["v_add_nc_i16 v0, v1, v2"]) & 0xFFFF, (a + b) & 0xFFFF)
                    self.assertEqual(run(movs + ["v_add_nc_i16 v0, v1, v2 clamp"]) & 0xFFFF,
                                     sat(s16(a) + s16(b), -32768, 32767) & 0xFFFF)
                    self.assertEqual(run(movs + ["v_sub_nc_i16 v0, v1, v2 clamp"]) & 0xFFFF,
                                     sat(s16(a) - s16(b), -32768, 32767) & 0xFFFF)

    def test_sat_pk_u8_i16(self):
        for a in HALVES:
            for b in HALVES:
                w = a | b << 16
                with self.subTest(w=hex(w)):
                    self.assertEqual(run([f"v_mov_b32 v1, 0x{w:x}", "v_sat_pk_u8_i16 v0, v1"]),
                                     sat(s16(a), 0, 255) | sat(s16(b), 0, 255) << 8)

    def test_norm_f16(self):
        for x in F16:
            for y in F16[:6]:
                movs = [f"v_mov_b32 v1, 0x{x:x}", f"v_mov_b32 v2, 0x{y:x}"]
                with self.subTest(x=hex(x), y=hex(y)):
                    self.assertEqual(run(movs[:1] + ["v_cvt_norm_i16_f16 v0, v1"]) & 0xFFFF, norm(h2f(x), True))
                    self.assertEqual(run(movs[:1] + ["v_cvt_norm_u16_f16 v0, v1"]) & 0xFFFF, norm(h2f(x), False))
                    self.assertEqual(run(movs + ["v_cvt_pknorm_i16_f16 v0, v1, v2"]),
                                     norm(h2f(x), True) | norm(h2f(y), True) << 16)
                    self.assertEqual(run(movs + ["v_cvt_pknorm_u16_f16 v0, v1, v2"]),
                                     norm(h2f(x), False) | norm(h2f(y), False) << 16)

    def test_fma_legacy_f32(self):
        vals = [0.0, -0.0, 1.5, -2.25, 3.0, float("inf"), bits_f32(0x3A83126F), 0.5]
        for x in vals:
            for y in vals[:5]:
                for z in (0.0, 1.0, -4.5):
                    want = f32(0.0 + z) if (x == 0 or y == 0) else f32(bits_f32(f32(x * y + z)))
                    movs = [f"v_mov_b32 v1, 0x{f32(x):x}", f"v_mov_b32 v2, 0x{f32(y):x}", f"v_mov_b32 v3, 0x{f32(z):x}"]
                    with self.subTest(x=x, y=y, z=z):
                        self.assertEqual(run(movs + ["v_fma_legacy_f32 v0, v1, v2, v3"]), want)
                        self.assertEqual(run(movs + ["v_mov_b32 v0, v3", "v_fmac_legacy_f32 v0, v1, v2"]), want)

    def test_fmaak_fmamk_f16(self):
        k = 0x4200
        for x in F16[2:9]:
            for y in F16[2:9]:
                movs = [f"v_mov_b32 v1, 0x{x:x}", f"v_mov_b32 v2, 0x{y:x}"]
                with self.subTest(x=hex(x), y=hex(y)):
                    self.assertEqual(run(movs + [f"v_fmaak_f16 v0, v1, v2, 0x{k:x}"]) & 0xFFFF,
                                     f2h(h2f(x) * h2f(y) + h2f(k)))
                    self.assertEqual(run(movs + [f"v_fmamk_f16 v0, v1, 0x{k:x}, v2"]) & 0xFFFF,
                                     f2h(h2f(x) * h2f(k) + h2f(y)))


if __name__ == "__main__":
    unittest.main()
