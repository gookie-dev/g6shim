# shader

The Series GPU's shader programs: GFX10 machine code, the intermediate forms raised from it, and its containers.

## Scalar register codes

| Code | Register |
|---|---|
| 0-105 | user and system SGPRs |
| 106-107 | VCC |
| 124 | M0 |
| 125 | null, no register |
| 126-127 | EXEC |

A wave64 vector instruction reads and writes both EXEC dwords.

## Mask tests

| Branch | Test |
|---|---|
| `s_cbranch_execz`, `s_cbranch_execnz` | EXEC |
| `s_cbranch_vccz`, `s_cbranch_vccnz` | VCC, over the lanes in EXEC |
| `s_cbranch_scc0`, `s_cbranch_scc1` | SCC |

## Pixel input VGPRs

One bit of `SPI_PS_INPUT_ADDR` per entry, in this order; each enabled entry takes the next VGPRs.

| Bit | VGPRs | Input |
|---|---|---|
| 0 | 2 | perspective sample barycentrics i, j |
| 1 | 2 | perspective center |
| 2 | 2 | perspective centroid |
| 3 | 3 | perspective pull-model barycentric |
| 4 | 2 | linear sample |
| 5 | 2 | linear center |
| 6 | 2 | linear centroid |
| 7 | 1 | line stipple |
| 8-11 | 1 each | position x, y, z, w |
| 12 | 1 | front face |
| 13 | 1 | ancillary |
| 14 | 1 | sample coverage |
| 15 | 1 | fixed-point position |

## Export targets

| Target | Meaning |
|---|---|
| 0-7 | colour render target |
| 8 | depth, stencil and sample mask |
| 9 | discarded |
| 12-16 | position x, y, z, w |
| 20 | primitive, from a merged or mesh stage |
| 32-63 | vertex parameter `n - 32`; bit 7 of the parameter marks a mesh per-primitive output |

## Inline constants

| Code | Value |
|---|---|
| 128-192 | 0 to 64 |
| 193-208 | -1 to -16 |
| 240-247 | ±0.5, ±1, ±2, ±4 |
| 248 | 0x3E22F983 |

## Root parameters

User data dword `i` arrives in SGPR `i` in the vertex, pixel and compute stages, and in SGPR `i + 8` in the merged stages.
A table or heap parameter holds the 32-bit heap address in its first dword, with the high half set to 0; a root descriptor holds a 64-bit buffer address in its first two.

One compute layout reserves four hardware user SGPRs and starts logical root data at word 16. Fixed scalar loads through the pointer copied from `s0` and the low eight bits of `s1` read declared logical words starting at `16 + byte_offset / 4`. The translator binds those words through the ordinary root constants and descriptor paths. Undeclared words and direct reads of a spilled table's high address word remain unsupported. [Public description of user data spilling](https://releases.llvm.org/16.0.0/docs/AMDGPUUsage.html#spill-table)

## Geometry stage

A geometry program is a merged export/geometry stage on local-data-share rings, not a primitive shader. It is wave64. It does not use `exp`.

The live vertex-offset registers on entry are `v0` and `v1`. They hold packed 16-bit input-vertex offsets, unpacked with word selects (`WORD_0` of `v0`, `WORD_1` of `v0`, `WORD_0` of `v1` are vertices 0, 1 and 2).

The merged-wave word in `s3` holds the geometry primitive count in bits 8–15. The host runs one primitive per invocation. A count extracted for an EXEC prefix mask or a comparison against zero can use that host count. A full-wave comparison is also supported when it only selects between the prefix mask and a full EXEC mask. Any use of the native count as shader data remains unsupported. [Public instruction set reference](https://docs.amd.com/api/khub/documents/Et~wpu9g~Ffl7d9q0QZ~Og/content)

Export-shader outputs are read from local data share at those vertex offsets (`ds_read2_b32` and `ds_read_b32`). A packed offset may be multiplied by four with a word-select shift or with a shift and low-word mask. Each signature register is 4 dwords, so register `n` starts 16 bytes after the vertex base. The input primitive is a triangle.

Emitted vertices are written with `ds_write*` into the geometry-to-vertex area. The vector address is the per-lane base. The instruction offset either selects the output register directly or indexes successive vertex records. Each record spans four dwords per declared output row, including row padding. In a single block, the translator can subtract the record base when the writes for each vertex precede that vertex's emit and every remaining word maps to a declared output component. A scalar dword load can be omitted if its result feeds only the address arithmetic for these output writes. A result used as output data, a control condition, or a different memory operand still needs a mapped scalar address. Output is a triangle strip on stream 0.

`s_sendmsg` `MSG_GS` with `GS_OP_EMIT` emits the vertex, `GS_OP_CUT` ends the strip, and `GS_OP_EMIT_CUT` does both. The program finishes with `MSG_GS_DONE` and `GS_OP_NOP`.

Geometry DXIL records its input width, stream 0 output width and a bitmask of dependent outputs for each input, followed by zero widths for the other three streams. The pipeline validation part uses the same input-to-output mask orientation. [DXIL view state serialization](https://github.com/microsoft/DirectXShaderCompiler/blob/main/lib/HLSL/ComputeViewIdState.cpp)

The paired vertex container holds an export-shader payload and a legacy vertex payload. A geometry-bound pipeline is taken to run the export-shader payload on the console. The host runs the legacy vertex payload; its exports are the geometry shader's inputs.

## DXIL bitcode blocks

Unabbreviated records only; each block's abbreviation width is fixed.

| Block | ID | Width |
|---|---|---|
| module | 8 | 3 |
| parameter attributes | 9 | 3 |
| parameter attribute groups | 10 | 3 |
| type | 17 | 4 |
| constants | 11 | 4 |
| metadata | 15 | 3 |
| value symbol table | 14 | 4 |
| function | 12 | 4 |

| Record | Code | Operands |
|---|---|---|
| module version | 1 | 1 |
| attribute group | 3 | group, 0xFFFFFFFF, (0, kind) pairs |
| parameter attributes | 2 | group |
| type count | 1 | count |
| void, float | 2, 3 | none |
| integer | 7 | bits |
| pointer | 8 | pointee, address space |
| array | 11 | count, element |
| struct | 18, 20 | body, or name (19) then body |
| function type | 21 | 0, return, parameters |
| global | 7 | type, `space << 2 \| 2 \| constant`, init, linkage, 3, 0 |
| function | 8 | type, 0, prototype, 0, attribute, then ten zeros |
| constants | 3, 4, 6, 7 | undef, signed integer, float bits, aggregate |
| metadata string, value, node | 1, 2, 3 | text, type and value, operand + 1 |
| named metadata | 4, 10 | name, nodes |
| metadata kind | 6 | index, text |
| value symbol | 1 | index, name |
| binary, cast, compare | 2, 3, 28 | relative operands, type, opcode |
| select, extract | 29, 26 | relative operands, index |
| call | 34 | 0, 0x8000, function type, relative function, arguments |
| phi | 16 | type, signed delta, predecessor |
| getelementptr | 43 | 0, pointee, relative pointer, indices |
| load, store | 20, 44 | relative operands, type, alignment 4, 0 |
| atomicrmw, cmpxchg | 38, 46 | relative operands, operation, 0, 6, 1 |
| return, branch | 10, 11 | none, or targets and condition |

## Container parts

| FourCC | Contents |
|---|---|
| `SFI0` | feature bits, 64-bit |
| `ISG1`, `OSG1`, `PSG1` | signature rows and names |
| `PSV0` | pipeline state validation |
| `HASH` | twenty zero bytes |
| `DXIL` | program |

SFI0 feature bits used here: 0x200 stencil reference, 0x800 typed UAV loads, 0x4000 wave operations, 0x8000 64-bit integers, 0x400000 64-bit atomics on typed resources, 0x2000000 resource heap, 0x4000000 sampler heap, 0x10000000 64-bit atomics on heap resources.

DXIL part header: program kind shifted left 16 and or 0x66, size in dwords, `DXIL`, validator version 1.6, bitcode offset 16, bitcode size.

Container header: `DXBC`, MD5-like digest, version 1.0, total size, part count, part offsets, then each part as FourCC, size and data.

## Signature rows

32 bytes per row, sorted by register then channel, unallocated registers last.

| Offset | Field |
|---|---|
| 0 | stream, 0 |
| 4 | name offset |
| 8 | semantic index |
| 12 | system value |
| 16 | component type: 0 none, 1 u32, 2 i32, 3 f32 |
| 20 | register |
| 24 | write mask |
| 25 | read/write mask |
| 28 | minimum precision, 0 |

Interpolation modes: 0 constant, 1 linear, 2 linear centroid, 3 linear no perspective, 4 perspective, 5 perspective centroid, 6 perspective sample, 7 linear sample.

A pixel shader writing SV_DepthLessEqual (18) or SV_DepthGreaterEqual (19) interpolates its position inputs (kind 3) at the centroid (5), as the DXIL validator requires.

## Image instructions

Sampling offsets are 6-bit signed per coordinate in the console encoding, and DXIL's immediate offsets are -8 to 7, so a computed offset word must be constant and in range.

## 64-bit image atomics

An image atomic whose `dmask` is 0x3, or 0xF for compare-and-swap, is a 64-bit atomic. It lowers to `dx.op.atomicBinOp.i64` or `dx.op.atomicCompareExchange.i64` on a UAV handle annotated as one U32 component (`0x105`), the annotation DXC emits for `RWTexture2D<uint64_t>`. Subtract is an add of the negated value. The return, when `glc` is set, is the previous 64-bit texel, split back into two VGPRs.

The container sets SFI0 bits 0x8000, 0x400000 and 0x10000000, and the module shader flags at bits 20, 27 and 32. Bit 0x10000000 is set because the handle comes from the descriptor heap. The host resource view for the operation is `R32G32_UINT`.

A host that reports false from `D3D12_FEATURE_DATA_D3D12_OPTIONS9::AtomicInt64OnTypedResourceSupported`, or from `D3D12_FEATURE_DATA_D3D12_OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported`, still receives the translated shader. `CreateComputePipelineState` or `CreatePipelineState` then fails, pipeline creation logs `host pipeline refused`, and the title gets no pipeline.

`image_atomic_inc` and `image_atomic_dec` have no shader model 6.6 atomic, at either width, and stop the translation.

## Predicates

`v_cmp_lg_*` is ordered and not equal, so a NaN makes it false: DXIL `olt | ogt`. `fcmp one` means the same but the AMD D3D12 driver treats it as unordered.

## PSV0 runtime info

48-byte block.

| Offset | Field |
|---|---|
| 0 | shared memory bytes (mesh) or output-depth flag |
| 1 | sample frequency: an input interpolated per sample (interpolation 6 or 7) or the sample index (kind 12) |
| 12, 14 | maximum vertices, primitives (mesh) |
| 16, 20 | minimum, maximum wave size |
| 24 | stage: 0 pixel, 1 vertex, 5 compute, 13 mesh |
| 26 | per-primitive vectors |
| 27 | topology |
| 28, 29 | input, output elements |
| 30 | per-primitive elements |
| 31, 32 | input, output vectors |
| 36, 40, 44 | thread-group x, y, z |

## Static sampler words

| Word | Bits | Field |
|---|---|---|
| 0 | 0-8 | address modes U, V, W |
| 0 | 9-11 | log2 of maximum anisotropy |
| 0 | 12-14 | comparison function minus one |
| 0 | 29-30 | reduction: 0 filtering, 1 minimum, 2 maximum |
| 1 | 0-11, 12-23 | minimum, maximum LOD, unsigned 4.8 |
| 2 | 0-13 | LOD bias, signed 5.8 |
| 2 | 20-21, 22-23 | magnification, minification filter |
| 2 | 26-27 | mip filter: none, point, linear |
| 3 | 30-31 | border colour |

Address modes 0-7 map to 1 wrap, 2 mirror, 3 clamp, 5 mirror once, 3, 5, 4 border, 5.

## Whole-wave sections

| Instruction | Inline constant | Meaning |
|---|---|---|
| `s_or_saveexec_b32`, `s_or_saveexec_b64` | 193 (-1) | EXEC becomes every lane |
| `s_nand_saveexec_b32`, `s_nand_saveexec_b64` | 128 (0) | EXEC becomes every lane |

The section ends at the first instruction that writes EXEC and reads the saved mask.

## DXIL emission bindings

| Space | Contents |
|---|---|
| 0 | user data `b0`, static samplers `s<n>` |
| 1 | root CBVs `b<i>`, raw root SRVs `t<i>`, `i` the root parameter |

| Handle properties (first word) | Resource |
|---|---|
| 10 | typed buffer, 4 float channels |
| 11 | raw buffer |
| 12 | structured buffer, second word the stride |
| 13 | constant buffer, second word the size |
| 14 | sampler; bit 15 marks a comparison sampler |
| + 0x1000 | UAV |

Handle annotation row is `0xFFFFFFFF`; a root CBV is annotated 65536 bytes.
Resource heap indices shift the byte address right by 5, sampler heap indices by 4.
Root CBVs and root SRVs are annotated with row 0xFFFFFFFF.

## Descriptor fields read as data

| Field | Word, bits |
|---|---|
| image width - 1 (low bits) | 2, 0-13 |
| image width - 1 (high bits) | 1, 30-31 |
| image height - 1 | 2, 14-29 |
| image last level | 3, 16-19 |
| image type | 3, 28-31 (0, 8, 9, 10, 12, 13 for 1D, 2D, 3D, 2D array, cube, 2D MSAA) |
| image depth or layers - 1 | 4 |
| buffer record count | 2 |

Word 3's low bits are `0xFAC`.

## GFX10 buffer formats

Families in number order; each family lists its numeric formats in the order
unorm, snorm, uscaled, sscaled, uint, sint, float. Channel widths run from the
lowest bits up.

| Family | Widths | Numeric formats |
|---|---|---|
| 1-6 | 8 | unorm, snorm, uscaled, sscaled, uint, sint |
| 7-13 | 16 | all seven |
| 14-19 | 8, 8 | six without float |
| 20-22 | 32 | uint, sint, float |
| 23-29 | 16, 16 | all seven |
| 30-36 | 11, 11, 10 | all seven |
| 37-43 | 10, 11, 11 | all seven |
| 44-49 | 2, 10, 10, 10 | six without float |
| 50-55 | 10, 10, 10, 2 | six without float |
| 56-61 | 8, 8, 8, 8 | six without float |
| 62-64 | 32, 32 | uint, sint, float |
| 65-71 | 16, 16, 16, 16 | all seven |
| 72-74 | 32, 32, 32 | uint, sint, float |
| 75-77 | 32, 32, 32, 32 | uint, sint, float |

## DXBC container

| Offset | Content |
|---|---|
| 0x00 | `DXBC` |
| 0x04 | digest[16] |
| 0x14 | version major u16, minor u16 |
| 0x18 | total size u32 |
| 0x1C | part count u32 |
| 0x20 | part offsets u32[count] |

Each part is a FourCC[4], a size u32 and the part body. A container is its first `total size` bytes.

## DXIL program

| Offset | Content |
|---|---|
| 0x00 | version u32: shader kind << 16, major << 4, minor |
| 0x04 | program size in dwords |
| 0x08 | `DXIL` |
| 0x0C | DXIL version u32 |
| 0x10 | bitcode offset from 0x08 |
| 0x14 | bitcode size u32 |

Shader kinds: 0 ps, 1 vs, 2 gs, 3 hs, 4 ds, 5 cs, 13 ms, 14 as.

The emitter uses validator version 1.8 for compute shaders that sample with implicit LOD. Their `PSV0` runtime info adds the entry point's string table offset after the thread group dimensions. The shader model remains 6.6.

A console container's `DXIL` part can hold a placeholder instead of LLVM bitcode. The kind always comes from the version word; the code is the native program the `XDAT` index lists.

## Native program index

| Offset | Content |
|---|---|
| 0x18 | `XDAT` |
| 0x1C | entry count u32, at most 64 |
| 0x20 | 0x24 u32 |
| 0x24 | entries: kind u32, offset u32, size u32 (part-relative) |

Kind 0x0D entries hold `XBCH`: version 3 u32, packed size u64, unpacked size u64, then a zlib stream of an `FSSM` blob.

## Packed shader record

Found by its marker. Offsets relative to the marker:

| Offset | Content |
|---|---|
| -6 | NUL after the name |
| -5 | kind |
| -4 | n u32 |
| 0 | marker `0x000DDCE7` |
| +4 | n u32 |
| +8 | unpacked size u32, at most 64 MiB |
| +12 | raw deflate stream, n - 12 bytes |

Before the NUL: a length byte (with the NUL) and the name. The two `n` fields must agree and be at least 12.
