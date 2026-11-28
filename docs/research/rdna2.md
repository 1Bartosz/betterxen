# RDNA 2 source notes

Sources were retrieved on 2026-11-22. AMD's RDNA 2 programming reference and
machine-readable ISA describe the AMD instruction set used by the synthetic gfx1030
fixture. Treat them as class B within the stated AMD target. Console-specific encoding,
descriptor layout, launch state and applicability remain unknown.

| Source | Version and scope | What it establishes |
| --- | --- | --- |
| [AMD RDNA 2 ISA Reference Guide](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture) | AMD document 70648, 30 November 2020 | §3.6 for register classes, §6.2 for vector sources, §8.1 and §12.14 for indexed buffer addresses and MUBUF, §12.2 and §12.5 for waits and termination, §12.7–12.8 for vector addition and moves. The guide is used as a reference with no text copied. |
| [AMD machine-readable ISA](https://gpuopen.com/machine-readable-isa/) | RDNA 2 XML, release date 2026-02-20, schema 1.1.1, AMD Public Use, MIT | Format bits, instruction widths and opcodes for the selected variants. |
| [AMD XML schema documentation](https://github.com/GPUOpen-Tools/isa_spec_manager/blob/720736542c7177de7e7970dec53d6a4a72cfef52/documentation/spec_documentation.md) | Upstream commit `720736542c7177de7e7970dec53d6a4a72cfef52` | Describes instruction encodings and bit layouts. No code imported. |
| [LLVM AMDGPU backend reference](https://llvm.org/docs/AMDGPUUsage.html) and [LLVM 23.1.6 source](https://github.com/llvm/llvm-project/tree/5b10d2d74929eae78912771222c9855af97496f4) | Homebrew LLVM 23.1.6, upstream release commit `5b10d2d74929eae78912771222c9855af97496f4` | The native assembler/disassembler accepts the exact gfx1030 Wave32 forms listed in the coverage table. This is class E tool behavior. |

AMD §8.1 describes the index, stride, offsets and bounds rules of a hardware
descriptor. Xenora's fixture supplies two bindings in advance under [ADR
0003](../adr/0003-demo-memory-model.md). It does not decode descriptors. AMD
§12.5 defines separate vector-load and vector-store accounting and describes
implicit waits at termination (class B, CLAIM-RDNA2-TRANSFER-WAITS). The accepted wait operands are chosen narrowly
for the fixture. AMD §12.7 specifies a uint32 sum with no carry. The VOP3
encoding for this VOP2 operation is described in §12.7.1 and is identified
separately in AMD's XML.

## Table provenance

The [public AMD archive](https://gpuopen.com/download/machine-readable-isa/latest/)
provided `amdgpu_isa_rdna2.xml` with SHA-256
`d671ecbc36543674ab59e9b2feffd56718fd2137e7ad913c314f2e2508b9f4f7`.
The release date is from the XML metadata. AMD copyright and MIT licensing
cover the generated table, which reduces the XML data to first-word masks,
opcode patterns and base lengths. The Python generator accepts only this
exact XML revision and does no network access. AMD may update the archive at
the same URL, so preserve the accepted XML outside the repository for future
regeneration.

Run from the checkout root after extracting the approved XML.

```sh
python3 crates/isa/generate_rdna2.py path/to/amdgpu_isa_rdna2.xml | diff -u crates/isa/src/rdna2.rs -
```

## Complete instruction metadata

`crates/isa/generate_rdna2_meta.py` reduces the same approved XML to every
gfx1030 base instruction form: 1,465 forms in 21 encoding families, with
identifying bits, field layouts, operands (field, data format, operand type,
width, direction), control-flow flags and allowed extension words. It runs in
two stages so regeneration and CI checks need no network and no XML:

```sh
python3 crates/isa/generate_rdna2_meta.py --xml path/to/amdgpu_isa_rdna2.xml  # XML -> snapshot -> Rust
python3 crates/isa/generate_rdna2_meta.py --check                              # snapshot -> Rust, compare
```

The snapshot `crates/isa/data/rdna2.jsonl` is sorted, normalised MIT data
under the AMD notice in `crates/isa/LICENSE-AMD`. The generator rejects a
different XML hash, another architecture, unclassified encodings, duplicate
variants, identifying bits outside a mask and overlapping encodings. Two
details are not in the identifier masks and are handled explicitly: MIMG
opcode bit 7 sits at bit 0, and MTBUF opcode bit 3 sits at bit 53.

The XML names literal, SDWA, DPP8, DPP16 and NSA variants without a rule for
when they apply. `xenora_isa::meta::identify` implements those length rules by
hand: a source field equal to 255 (`SRC_LITERAL` in the XML) adds a literal;
VOP1, VOP2 and VOPC `SRC0` values 0xF9, 0xFA, 0xE9 and 0xEA add an SDWA, DPP16
or DPP8 control dword (from the RDNA 2 ISA guide's SDWA and DPP sections, not
the XML); MIMG `NSA` adds that many address dwords. An extension the form does
not list is rejected. Every LLVM-assembled fixture identifies with exact
boundaries and matching mnemonics. `crates/isa/tests/llvm/identify.s` holds one
instruction per family and extension word (literal, SDWA, DPP16, DPP8 with and
without fetch-inactive, VOP3 and VOP3P literals, NSA images, MTBUF opcodes
above 7, MSAA loads); its LLVM oracle compares every name and length:

```sh
LLVM_BIN=path/to/llvm/bin cargo test --locked -p xenora-isa --test meta_llvm -- --include-ignored
```

The oracle found five instructions that the XML lists only under a literal
encoding (`S_SETREG_IMM32_B32`, `V_FMAMK_F32`, `V_FMAAK_F32`, `V_FMAMK_F16`,
`V_FMAAK_F16`); the generator now treats them as base forms that always carry a
literal.

Identification is not execution. `xenora shaders coverage` reports forms
discovered in metadata against forms the semantic decoder accepts
(`xenora_decoder::SEMANTIC_FORMS`); reference, Metal and native GPU evidence
per form remains in `docs/status.toml`.

## 64 KiB swizzle (`SW_64KB_S`) — public sources (#505)

`xg_xs.dll` implements `SW_64KB_S` tile addressing for `TEXTURE2D`, one array
slice and one mip, over the element sizes the RB+ tables specify: 32-bpp RGBA8
(4-byte elements) and the BC family — BC1/BC4 with 8-byte blocks and
BC2/3/5/6H/7 with 16-byte blocks, where one "texel" element is a 4x4 block and
`elemLog2 = log2(block_bytes)`. At 32 bpp the two public sources below agree
bit-for-bit, which is why this is the mode implemented first; the 8- and 16-byte
rows come from AddrLib alone.

**Microsoft, `D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE`** (public, no NDA):
<https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_texture_layout>.
The page gives the 32-bpp masks `xBytesMask = 0xAA8F` and `yMask = 0x5570`,
combined with `_pdep_u32`. It states that tail-mip packing is **adapter-specific**,
which is why only the single-mip case is implemented. It has no `D` or `R` variant.

**AMD AddrLib (MIT)** — the authoritative public implementation, in both
[Mesa `src/amd/addrlib`](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/amd/addrlib)
and [AMD PAL](https://github.com/GPUOpen-Drivers/pal/tree/dev/src/core/imported/addrlib)
(`SPDX-License-Identifier: MIT`, AMD copyright; PAL `LICENSE.txt` is MIT):

- `src/amd/addrlib/inc/addrtypes.h` — `AddrSwizzleMode`: `ADDR_SW_64KB_S` = 9,
  `_D` = 10, `_R` = 11 (Reserved on GFX10).
- `src/amd/addrlib/src/gfx10/gfx10SwizzlePattern.h` — `GFX10_SW_64K_S_PATINFO`
  and `GFX10_SW_64K_S_RBPLUS_PATINFO`, plus the `GFX10_SW_PATTERN_NIBBLE01/2/3/4`
  tables. The RB+ variant is used because `gfx1030` is Navi2x.
- `src/amd/addrlib/src/gfx10/gfx10addrlib.h` — `GetSwizzlePatternFromPatternInfo`,
  `IsInMipTail`.
- `src/amd/addrlib/src/gfx10/gfx10addrlib.cpp` — `GetSwizzlePatternInfo`,
  `HwlComputeSurfaceAddrFromCoordTiled`, `ComputeSurfaceAddrFromCoordMacroTiled`.
- `src/amd/addrlib/src/core/addrlib2.cpp` — `ComputeThinBlockDimension`,
  `Block256_2d`.

There is **no** standalone GPUOpen `addrlib` repository (404). The public
[RDNA 2 ISA](https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf)
only names a 5-bit "SW mode" field in the T# descriptor (bits 120:116) and does
not define the patterns.

### S versus D versus R

- **S and D are not interchangeable.** At 32 bpp RB+, D uses nibble01 index 39
  (`[0,0,X0,X1,Y0,Y1,X2,Y2]`) while S uses index 2
  (`[0,0,X0,X1,Y0,Y1,Y2,X2]`) — address bits 6 and 7 swapped.
- **`SW_64KB_R` has no public GFX10 form.** Mesa's `SwizzleModeTable` marks
  `ADDR_SW_64KB_R` (enum 11) as Reserved; the render-target mode is
  `ADDR_SW_64KB_R_X`, whose pipe/packer XOR depends on the console
  `GB_ADDR_CONFIG`, which is not public.

### Xbox `XG` tile modes are NDA

`scarlett-tex-layouts` returns HTTP 200 with only the membership gate; all
`/windows/win32/api/xg/*` paths 404. The numeric `XG_SWIZZLE_MODE` values are
not public, so BetterXen's `BX_XG_LAYOUT_*` values 1/2/3 are **local** constants,
not the console's.

### Element sizes and the derived masks

For a 64 KiB tile the intra-tile byte offset of element (x, y) is
`pdep(x, x_mask) | pdep(y, y_mask)`, where x and y are **block** indices (one
4x4 block for BC, one texel for 32 bpp). The masks were expanded by hand from
`GFX10_SW_PATTERN_NIBBLE01/2/3` at the patinfo indices
`GFX10_SW_64K_S_RBPLUS_PATINFO` selects for each `elemLog2`:

| block bytes | formats | `elemLog2` | x mask | y mask | tile (blocks) |
| --- | --- | --- | --- | --- | --- |
| 4 | R8G8B8A8_* | 2 | `0xAA8C` | `0x5570` | 128 x 128 |
| 8 | BC1, BC4 | 3 | `0xAAC8` | `0x5530` | 128 x 64 |
| 16 | BC2, BC3, BC5, BC6H, BC7 | 4 | `0xAAC0` | `0x5530` | 64 x 64 |

At 32 bpp the x mask `0xAA8C` is Microsoft's public `xBytesMask` `0xAA8F` with
the two intra-texel byte bits cleared (`0xAA8F & ~0x3`), and the y mask matches
`0x5570` exactly. The 8- and 16-byte rows have no D3D12 page to cross-check
against; they are AddrLib-only, which is stated in the PR risks. All three rows
were checked to be bijections over their 64 KiB tile (16384 / 8192 / 4096
distinct offsets, each block-aligned and inside the tile).

### Verification

The three masks above were expanded and their bijectivity checked against an
independently written `pdep` reference, outside the shim. `test_xg_xs.exe`
re-derives its expected offsets from those masks (and, at 32 bpp, from the
D3D12 page) rather than reading them back from the shim: one test per block
size, covering intra-block offsets (the 16 texels of a BC block share one byte
offset), independently derived absolute offsets, and an offset -> texel -> offset
round trip. It keeps asserting `E_NOTIMPL` for `SW_64KB_D`, `SW_64KB_R`, 1D/3D,
MSAA, and every multi-mip or multi-slice case. Synthetic evidence only; no
Xbox-hardware claim.
