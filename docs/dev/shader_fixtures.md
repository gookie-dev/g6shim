# GFX10 translator fixtures

`tests/gfx10/*.s` are small hand-written GFX10 compute programs, one translator rule each. `tests/unit/gfx10_fixtures.inc` holds their assembled words. `shader_fixtures_translate_and_validate` lowers every program to DXIL and checks the container with the DXIL validator, so neither CI nor contributors need LLVM.

`tests/gfx10/gs/*.s` are geometry programs, not part of that compute suite. `gs_emit.s` is a wave64 legacy geometry shader: an `s_bfm_b64` / `s_cselect_b64` prologue, word-select unpack of `v0` and `v1`, a ring read, ring writes and `s_sendmsg` emit/cut. Its words are in `tests/unit/shader_geometry_test.cpp`. Regenerate them with the same `llvm-mc` invocation (`-triple=amdgcn-amd-amdpal -mcpu=gfx1030`) and copy the `.text` dwords.

## Program shape

Most programs use the same frame around their rule:

| Part | Instructions |
|---|---|
| inputs | `s[0:3]` root constants; `s4` the descriptor table address, `s5` set to 0 |
| descriptor | `s_load_dwordx4 s[8:11], s[4:5], 0x0`: the output UAV |
| operands | `v1`-`v4` copied from `s0`-`s3` |
| rule | the instructions under test, result in `v0` |
| output | `s_waitcnt lgkmcnt(0)`, `buffer_store_dword v0, off, s[8:11], 0`, `s_endpgm` |

The test lowers each program as a wave32 compute shader with 8 VGPRs, five user SGPRs (root constants at user data 0, the table at user data 4, one UAV in it) and a 1x1x1 group. `lane_mask_data` does not use the shared frame: it reads the lane index, stores a ballot, a bit count, a prefix count, a mask combined from two compares, and the active-lane count, one dword each, at the thread id.

`scalar_buffer_length.s` binds an input resource at the start of the table and an output UAV at byte offset `0x20`. It stores the input length and the result of a scalar buffer load in the UAV.

`scalar_buffer_address.s` uses the same bindings. A branch joins the low address word with a zero value before the store. The test uses both shader-resource and constant-buffer input views.

`scalar_buffer_fields.s` joins two descriptor words with zero and masks out their address or component selector bits. An unused read outside the known bits leaves those results intact. The negative case returns a value that depends on an unavailable stride bit.

`scalar_structured_fields.s` masks and extracts stride bits from a structured descriptor word, compares it with zero and converts the top format byte to float.

`scalar_raw_buffer_high_word.s` compares a byte-addressed buffer's upper address word with a threshold and converts the top format byte after a control-flow join.

`scalar_typed_buffer_format.s` uses a formatted vector load and also reads that buffer's complete high address and format words as data.

`scalar_image_address.s` uses the first slot for an image descriptor. It stores the shifted base address, the image load result, a sampling performance bit, a comparison of reserved bits and a compression metadata address bit. The negative case checks that corner sampling remains unavailable.

## Adding a program

1. Write `tests/gfx10/<rule>.s` in the frame above; the file name is the fixture's name.
2. Regenerate the words with an `llvm-mc` that has the AMDGPU target:

```bat
python tools/gfx10_fixtures.py --llvm-mc <path to llvm-mc.exe>
```

The script assembles each program with `-triple=amdgcn-amd-amdpal -mcpu=gfx1030`, takes the `.text` section of the object and writes the words in file-name order. `--check` exits 1 when the committed words are stale; `LLVM_MC` can name the tool instead of `--llvm-mc`.

3. Commit the `.s` file and the regenerated `.inc` together.

## 64-bit image atomics

`tests/gfx10/image64/*.s` are compute programs whose rule is one 64-bit image atomic. The frame loads an 8-dword image descriptor from the table at user data 4 and uses `s0`-`s3` as coordinates and data. `tests/unit/image64_fixtures.inc` holds the words. The same `python tools/gfx10_fixtures.py` command regenerates that file when the directory is present. `shader_image_atomic64_translates_and_validates` lowers each program with an 8-dword UAV and checks the container.
