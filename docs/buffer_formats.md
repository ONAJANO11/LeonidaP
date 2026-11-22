# Typed buffer formats: every GFX10 code a V# or MTBUF can name

What `buffer_load_format_*`, `buffer_store_format_*` and `tbuffer_load_format_*` do for each GFX10 buffer data format
(V# word 3 `FORMAT` [18:12], or the `format:` modifier of an MTBUF instruction), measured on a Radeon 880M and held
against the translator. The contract row is "Typed buffer formats" in [design.md](design.md); the code is
`Gen.FORMATS`, `Gen.decode_format` and `Gen.encode_format` in `tools/rdna2spirv.py`; the oracle is
`tools/buffer_format_fuzz.py`.

**Read this first: the GPU is not RDNA2.** The only local AMD part is the Radeon 880M (gfx1150, RDNA3.5, GFX11). GFX11
numbers its formats differently (8_8_8_8_UNORM is 56 on GFX10 and 42 on GFX11), so every format is mapped by name
(`GFX11_CODES` in the tool, taken from what the AMD assembler encodes for `format:[BUF_FMT_<name>]`). A match supports
the model on RDNA3.5. Where this repository measured RDNA2 itself (UINT saturation and UNORM/SNORM rounding on gfx1036,
the xy/xyz stores on a Radeon 680M) the two agree; everything else is "GFX11, believed to carry over".

## Which codes there are

GFX10's `FORMAT` is 7 bits, so a buffer can carry codes 1..77 (the table in `tools/gfx10_formats.json` goes to 292
because images use 9 bits; 128 and up are image-only: sRGB, BC, depth, 5_6_5, 1_5_5_5, 5_5_5_1, 4_4_4_4, 5_9_9_9, 6E4).
There are no 5_6_5-style formats for buffers. Of the 77:

- **65 are in `Gen.FORMATS`**: every f32/u32/i32 code, the 8/16-bit and 8_8/16_16/8_8_8_8/16_16_16_16 families in all of
  UNORM, SNORM, USCALED, SSCALED, UINT, SINT (and FLOAT for 16-bit), 10_11_11_FLOAT (36), 11_11_10_FLOAT (43) and the
  10_10_10_2 (44..49) and 2_10_10_10 (50..55) families in all six numeric formats.
- **12 are not**: 10_11_11 and 11_11_10 with UNORM, SNORM, USCALED, SSCALED, UINT and SINT (codes 30..35 and 37..42).
  GFX11 has none of them (its 10_11_11 and 11_11_10 are FLOAT only), so there is nothing to measure them against, and a
  guess at what a 5-bit-exponent field "normalized" to would be worse than the 0 a shader gets now. They decode to 0
  and store nothing, as every code outside the table does.

Of the 65, the 880M can run 63 (GFX11 has no 10_10_10_2_USCALED / SSCALED, codes 46 and 47; their layout is that of the
measured 10_10_10_2 and 2_10_10_10 siblings, and their SCALED conversion is the measured one).

## Names and bit order

Names list the component widths from the top bit down, so the **last** number is `x` in bit 0: `2_10_10_10` (50) is
R10G10B10A2 with x in bits 9:0 and a 2-bit alpha on top, `10_10_10_2` (44) has a 2-bit x. `10_11_11_FLOAT` (36) is x =
11 bits [10:0], y = 11 bits [21:11], z = 10 bits [31:22]; `11_11_10_FLOAT` (43) is x = 10 bits, y and z 11 bits each.
The 880M confirmed all of these.

## Loads (measured)

Every result word of an `xyzw` load of 63 formats, through a V# (`DST_SEL` = XYZW) and through tbuffer's own format
(with a V# that names 32_FLOAT), equals the model below. Per format: every value of every field up to 12 bits (and all
65,536 half floats), edge values (0, 1, max, signed min and its neighbours), random values above that, and random bytes
past the element.

| Format class | Result |
|---|---|
| UNORM | correctly rounded f32 of x / (2ⁿ−1) |
| SNORM | correctly rounded x / (2ⁿ⁻¹−1); the most negative code is −1.0 |
| USCALED / SSCALED | the integer, as a float, **not** divided (exact: at most 16 bits) |
| UINT / SINT | the zero-extended / sign-extended integer, in the integer register |
| 16_FLOAT | the half as f32; a NaN keeps its sign and payload (`0x7C01` gives `0x7F802000`, **not quieted**) |
| 10_11_11 / 11_11_10 FLOAT | unsigned float, 5 exponent bits, bias 15, 6 (11-bit field) or 5 (10-bit field) mantissa bits, no sign. Exponent 0: m·2⁻²⁰ (m·2⁻¹⁹ for 5 bits), a denormal that is **kept**. Exponent 31: infinity, or a NaN whose payload is the mantissa shifted up (`0x7C1` gives `0x7F820000`), unquieted |
| channels the format lacks | through a V#: component *c* mod *n* repeats (the translator's existing rule, SHADER-86, now also measured on gfx1150); through tbuffer: 0 (the instruction names `X000`, `XY00`, `XYZ0` or `XYZW` by the format's component count, and the V# `DST_SEL` is ignored) |

## Stores (measured)

`buffer_store_format_xyzw` and `tbuffer_store_format_xyzw` of 63 formats into a buffer pre-filled with `0xDEAD0000`, the
whole 16 bytes compared, so a byte written outside the element shows up.

| Format class | Result |
|---|---|
| UINT | saturates to the field width (min(x, 2ⁿ−1)) |
| SINT | saturates to the signed range of the field |
| UNORM | clamp to [0, 1], then the **exact** product x·(2ⁿ−1) rounded to nearest even; a NaN stores 0 |
| SNORM | clamp to [−1, 1], then the exact product x·(2ⁿ⁻¹−1) rounded to nearest even, **except the 2-bit field** (10_10_10_2 and 2_10_10_10 SNORM), where a half rounds away from zero (0.5 gives 1, −0.5 gives −1; nearest even would give 0) |
| **USCALED / SSCALED** | **converted as UNORM / SNORM**: storing 200.0 into 8_USCALED writes 255, storing 0.5 into 8_SSCALED writes 64. The SCALED formats are for loads; a store does not undo the "not divided" |
| 16_FLOAT, 10_11_11 / 11_11_10 FLOAT | **round toward zero** (not nearest): 0.49999997 gives `0x37FF`, not `0x3800`. A value too large for the format gives the **largest finite** value, not infinity: 65520.0 and 1e30 give `0x7BFF` for halves; only a real +infinity stores infinity. Denormals are produced. The unsigned floats store 0 for every negative number, −0 and −infinity |
| NaN into a float format | stays a NaN: the sign (halves only) and the top mantissa bits of the payload, or 1 when those are all zero (`0x7F800001` gives `0x7C01`) |
| a format with two components | **`buffer_store_format_*` on the 880M takes channels (x, w), not (x, y)**: `8_8_UINT` of (0x11, 0x22, 0x33, 0x44) writes 0x11, 0x44, and `buffer_store_format_xy` into it writes 0x11 and 0. **`tbuffer_store_format_*` takes (x, y)**, like the translator. 1-, 3- and 4-component formats take x, xyz and xyzw (`_xy` into them: x, y, then 0). The translator writes (x, y) for both. **KNOWN DIFFERENCE, not in the validated count; the target RDNA2 behaviour is unresolved until it is measured** |

`DST_SEL` also reorders a store's channels on the 880M in a way that is not a plain swizzle (`xzwy` stores `zxyw`); only
the identity and the B/R swap (`zyxw`, the 8_8_8_8 and 2_10_10_10 UNORM case the translator already handles) are modelled.

## What the 880M ran

`python -I tools/buffer_format_fuzz.py --oracle hw --budget 40 --seed 7` on a Radeon 880M (gfx1150): **1,330,469 validated
elements, 0 mismatches**, plus **26,523 known-difference elements** that are counted apart (below). Stores run as `_xyzw` and
as `_xy`, with a different value in every register (an earlier version of the probe gave y and w the same value for
two-component formats, which hid the difference). `--negative-control` shows the harness is not vacuous (a kernel whose V# names another format differs on
all but one or two elements of five formats). `--self-test` checks the model without a GPU.

| Code | Format | Translator | V# loads | tbuffer loads | V# stores (xyzw and xy) | tbuffer stores (xyzw and xy) | known difference |
|---|---|---|---|---|---|---|---|
| 1 | `8_UNORM` | before | 338 | 338 | 3,024 | 3,024 | 0 |
| 2 | `8_SNORM` | before | 338 | 338 | 3,024 | 3,024 | 0 |
| 3 | `8_USCALED` | added | 338 | 338 | 1,864 | 1,864 | 0 |
| 4 | `8_SSCALED` | added | 338 | 338 | 1,864 | 1,864 | 0 |
| 5 | `8_UINT` | added | 338 | 338 | 1,688 | 1,688 | 0 |
| 6 | `8_SINT` | added | 338 | 338 | 1,688 | 1,688 | 0 |
| 7 | `16_UNORM` | before | 606 | 606 | 3,024 | 3,024 | 0 |
| 8 | `16_SNORM` | before | 606 | 606 | 3,024 | 3,024 | 0 |
| 9 | `16_USCALED` | added | 606 | 606 | 1,864 | 1,864 | 0 |
| 10 | `16_SSCALED` | added | 606 | 606 | 1,864 | 1,864 | 0 |
| 11 | `16_UINT` | added | 606 | 606 | 1,688 | 1,688 | 0 |
| 12 | `16_SINT` | added | 606 | 606 | 1,688 | 1,688 | 0 |
| 13 | `16_FLOAT` | before | 65,618 | 65,618 | 1,864 | 1,864 | 0 |
| 14 | `8_8_UNORM` | before | 594 | 594 | 1,247 | 3,024 | 1,777 |
| 15 | `8_8_SNORM` | before | 594 | 594 | 499 | 3,024 | 2,525 |
| 16 | `8_8_USCALED` | added | 594 | 594 | 834 | 1,864 | 1,030 |
| 17 | `8_8_SSCALED` | added | 594 | 594 | 414 | 1,864 | 1,450 |
| 18 | `8_8_UINT` | added | 594 | 594 | 592 | 1,688 | 1,096 |
| 19 | `8_8_SINT` | added | 594 | 594 | 248 | 1,688 | 1,440 |
| 20 | `32_UINT` | before | 357 | 357 | 1,688 | 1,688 | 0 |
| 21 | `32_SINT` | before | 357 | 357 | 1,688 | 1,688 | 0 |
| 22 | `32_FLOAT` | before | 357 | 357 | 1,864 | 1,864 | 0 |
| 23 | `16_16_UNORM` | before | 1,130 | 1,130 | 941 | 3,024 | 2,083 |
| 24 | `16_16_SNORM` | before | 1,130 | 1,130 | 400 | 3,024 | 2,624 |
| 25 | `16_16_USCALED` | added | 1,130 | 1,130 | 724 | 1,864 | 1,140 |
| 26 | `16_16_SSCALED` | added | 1,130 | 1,130 | 382 | 1,864 | 1,482 |
| 27 | `16_16_UINT` | before | 1,130 | 1,130 | 350 | 1,688 | 1,338 |
| 28 | `16_16_SINT` | added | 1,130 | 1,130 | 88 | 1,688 | 1,600 |
| 29 | `16_16_FLOAT` | before | 131,154 | 131,154 | 74 | 1,864 | 1,790 |
| 36 | `10_11_11_FLOAT` | added | 5,202 | 5,202 | 1,864 | 1,864 | 0 |
| 43 | `11_11_10_FLOAT` | added | 5,202 | 5,202 | 1,864 | 1,864 | 0 |
| 44 | `10_10_10_2_UNORM` | before | 3,158 | 3,158 | 3,224 | 3,224 | 0 |
| 45 | `10_10_10_2_SNORM` | added | 3,158 | 3,158 | 3,224 | 3,224 | 0 |
| 48 | `10_10_10_2_UINT` | added | 3,158 | 3,158 | 1,688 | 1,688 | 0 |
| 49 | `10_10_10_2_SINT` | added | 3,158 | 3,158 | 1,688 | 1,688 | 0 |
| 50 | `2_10_10_10_UNORM` | before | 3,158 | 3,158 | 3,224 | 3,224 | 0 |
| 51 | `2_10_10_10_SNORM` | added | 3,158 | 3,158 | 3,224 | 3,224 | 0 |
| 52 | `2_10_10_10_USCALED` | added | 3,158 | 3,158 | 1,864 | 1,864 | 0 |
| 53 | `2_10_10_10_SSCALED` | added | 3,158 | 3,158 | 1,864 | 1,864 | 0 |
| 54 | `2_10_10_10_UINT` | added | 3,158 | 3,158 | 1,688 | 1,688 | 0 |
| 55 | `2_10_10_10_SINT` | added | 3,158 | 3,158 | 1,688 | 1,688 | 0 |
| 56 | `8_8_8_8_UNORM` | before | 1,106 | 1,106 | 3,024 | 3,024 | 0 |
| 57 | `8_8_8_8_SNORM` | before | 1,106 | 1,106 | 3,024 | 3,024 | 0 |
| 58 | `8_8_8_8_USCALED` | added | 1,106 | 1,106 | 1,864 | 1,864 | 0 |
| 59 | `8_8_8_8_SSCALED` | added | 1,106 | 1,106 | 1,864 | 1,864 | 0 |
| 60 | `8_8_8_8_UINT` | before | 1,106 | 1,106 | 1,688 | 1,688 | 0 |
| 61 | `8_8_8_8_SINT` | added | 1,106 | 1,106 | 1,688 | 1,688 | 0 |
| 62 | `32_32_UINT` | before | 632 | 632 | 36 | 1,688 | 1,652 |
| 63 | `32_32_SINT` | before | 632 | 632 | 50 | 1,688 | 1,638 |
| 64 | `32_32_FLOAT` | before | 632 | 632 | 6 | 1,864 | 1,858 |
| 65 | `16_16_16_16_UNORM` | before | 2,178 | 2,178 | 3,024 | 3,024 | 0 |
| 66 | `16_16_16_16_SNORM` | before | 2,178 | 2,178 | 3,024 | 3,024 | 0 |
| 67 | `16_16_16_16_USCALED` | added | 2,178 | 2,178 | 1,864 | 1,864 | 0 |
| 68 | `16_16_16_16_SSCALED` | added | 2,178 | 2,178 | 1,864 | 1,864 | 0 |
| 69 | `16_16_16_16_UINT` | before | 2,178 | 2,178 | 1,688 | 1,688 | 0 |
| 70 | `16_16_16_16_SINT` | added | 2,178 | 2,178 | 1,688 | 1,688 | 0 |
| 71 | `16_16_16_16_FLOAT` | before | 262,226 | 262,226 | 1,864 | 1,864 | 0 |
| 72 | `32_32_32_UINT` | before | 907 | 907 | 1,688 | 1,688 | 0 |
| 73 | `32_32_32_SINT` | before | 907 | 907 | 1,688 | 1,688 | 0 |
| 74 | `32_32_32_FLOAT` | before | 907 | 907 | 1,864 | 1,864 | 0 |
| 75 | `32_32_32_32_UINT` | before | 1,182 | 1,182 | 1,688 | 1,688 | 0 |
| 76 | `32_32_32_32_SINT` | before | 1,182 | 1,182 | 1,688 | 1,688 | 0 |
| 77 | `32_32_32_32_FLOAT` | before | 1,182 | 1,182 | 1,864 | 1,864 | 0 |

"before" means the format was in `Gen.FORMATS` before this change. (Codes 5, 6, 11, 12, 18, 19, 28, 61 and 70, the narrow UINT and SINT ones, were added to main's table by a parallel change, measured on gfx1036, while this branch was open; they are marked "added" here because the 880M run covers them too and the table entries are identical.) Codes 30..35, 37..42 (no GFX11 form), 46 and 47 (GFX11
has no SCALED 10_10_10_2) are not in the table.

## Where the translator stood, and what changed

Decode of every format that was already in `Gen.FORMATS` matches the 880M word for word (the 32 old codes are in the
table above). **Differences found in the older stores, left as they are** (an existing format stays byte-identical in this
change; the three are pinned by an `expectedFailure` in `tests/semantic/test_buffer_formats_all.py`, which starts to
fail when someone fixes them):

1. **16_FLOAT, 16_16_FLOAT and 16_16_16_16_FLOAT stores round to nearest even and overflow to infinity** (`Gen.f16_bits`
   without `rtz`); the 880M rounds toward zero and saturates to ±65504. In a 200-row sample of edge-heavy inputs, 89 of
   200 (16_FLOAT), 132 of 200 and 174 of 200 differed (mostly by 1 ulp). This is the f16 *format* store, not the d16 data
   rounding of BUILD-161 (#1759).
2. **UNORM and SNORM stores multiply in f32 and round that**, which rounds twice. 0.3·65535 is 19660.5008 exactly and
   19660.5 in f32, a tie that goes to 19660; the hardware stores 19661. In the same sample 0 of 200 differed for the 8-bit
   one-component formats and between 1 and 13 of 200 for the others (8_8_8_8_UNORM 9, 16_16_UNORM 7, 16_16_16_16_UNORM 12,
   10_10_10_2_UNORM 13, 2_10_10_10_UNORM 11). The new formats store with an exact integer product
   (`Gen.norm_encode_exact`).
3. **A two-component format stored with `buffer_store_format_xyzw` or `_xy`** takes (x, y) in the translator and (x, w) on the 880M
   (26,523 elements in the run above, the last column of the table; `tbuffer_store_format_*` agrees with the translator).
   The RDNA2 xy/xyz measurements in `cs_fmt_xy_xyz.asm` do not cover it. **Unresolved: the translator is unchanged until an RDNA2
   card shows which is right.** It is pinned as an `expectedFailure` too, and excluded from the validated count.

## Not measured

RDNA2 itself (gfx1013 is the target; no format has been run on any RDNA2 part in this change); the twelve codes above;
codes 46 and 47; `d16` loads and stores of the new kinds (they use the same d16 path as the existing formats: SINT
saturates signed, UINT unsigned; BUILD-161 owns d16 rounding); formats on unaligned addresses beyond what the existing
tests cover; swizzled V#s.
