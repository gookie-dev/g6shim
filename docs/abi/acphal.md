# AcpHal.dll

The audio control processor (ACP) runs the console's audio hardware. Titles reach it through `AcpHal.dll`, either directly or through audio middleware that decodes XMA on the hardware.

## APU memory

The audio hardware addresses memory with 32-bit APU addresses, so APU blocks live below 4 GiB. g6shim maps them one to one: a block's APU address is its virtual address.

| Export | Signature and use |
|---|---|
| `ApuCreateHeap` | `HRESULT (UINT32 cached, UINT32 uncached)`: sizes of the two heaps `ApuAlloc` draws from |
| `ApuAlloc` | `HRESULT (void** va, UINT32* apu, UINT32 size, UINT32 alignment, UINT32 flags)`. `apu` may be null. Observed alignments 4 to 0x800, flags 1 and 2 |
| `ApuFree` | `void (void* va)` |
| `ApuMapVirtualAddress` | `UINT32 (const void* va)`: the APU address of a pointer into a block. Titles write the result into hardware contexts |
| `ApuIsVirtualAddressValid` | `BOOLEAN (const void* va, UINT32 size)`: whether `size` bytes from `va` lie in one block. Titles check buffers with it before mapping them |
| `ApuMapApuAddress` | `void* (UINT32 apu)`: the host address of any byte of a block, null outside every block. Titles hand the result to host code |

## Shape contexts

The hardware keeps one set of context arrays per process, one array per kind of processing block.

| Export | Signature and use |
|---|---|
| `AcpHalAllocateShapeContexts` | `HRESULT (void* config)`. Reads six context counts and writes six array pointers back into `config`. Titles clear the arrays themselves after the call. |
| `AcpHalReleaseShapeContexts` | `HRESULT ()`. Frees the arrays; titles call it after releasing their `IAcpHal` objects. |
| `AcpHalCreate` | `HRESULT (IAcpHal** hal)`: one audio coprocessor object |

`config` is 0x60 bytes; titles zero it before filling the counts.

| Offset | Content |
|---|---|
| +0x00 | six `UINT32` context counts, one per kind |
| +0x18 | six array pointers, written by the call; kinds with a count of 0 get null |
| +0x48 | six `UINT32` APU addresses of the arrays, written by the call |

| Kind | Block | Context size | Array alignment | Maximum count |
|---|---|---|---|---|
| 0 | sample-rate converter (SRC) | 32 | 0x20 | 0x1000 |
| 1 | EQ and compressor | 160 | 0x10 | 0x1000 |
| 2 | filter and volume | 32 | 0x20 | 0x1000 |
| 3 | DMA | 16 | 0x10 | 0x1000 |
| 4 | XMA decoder | 64 | 0x40 | 0x200 |
| 5 | PCM | 32 | shares the XMA block | 0xE00 |

- When the SRC or PCM count is not 0, the SRC count must equal the XMA count plus the PCM count: every source has its converter. Otherwise the call fails with `E_INVALIDARG`, as it does for a count above the maximum.
- The PCM array follows the XMA array in one block.
- A second allocation fails with `0x8AC80001`; a release without an allocation fails with `0x8AC80002`.
- Kinds 1 and 2 are named after the hardware blocks by their context sizes; the names of kinds 0 and 3 to 5 match the shared-context fields the console's audio library uses.

## Audio coprocessor

`AcpHalCreate` returns a pointer to an object whose first field is a pointer to this method table.

| Index | Method | Behaviour |
|---|---|---|
| 0 | `HRESULT connect(UINT32 commands, UINT32 messages)` | `E_INVALIDARG` when `commands` is 0; otherwise records the message queue size |
| 1 | `HRESULT disconnect()` | `E_FAIL` when not connected; otherwise queues message `0x80` if registered |
| 2 | `HRESULT submit(UINT32 command, UINT64 id, UINT32 frame, const void* data, UINT32 notification)` | completes at once, always `S_OK`; `data` starts with the command's `UINT32` words |
| 3 | `bool pop_message(Message* out)` | removes the oldest message; false when the queue is empty |
| 4 | `UINT32 message_count()` | number of queued messages |
| 5 | `void release()` | destroys the object |

`Message` is 28 bytes:

| Offset | Content |
|---|---|
| +0x00 | `UINT32` kind |
| +0x04 | `UINT32` messages dropped since the previous one queued |
| +0x08 | `UINT32` microseconds since the object was created |
| +0x0C | 16 data bytes |

| Kind | Meaning | Data |
|---|---|---|
| `0x02` | flowgraph completed | flowgraph id, `UINT32` |
| `0x10` | command completed | command `UINT32`, id `UINT64`, audio frame `UINT32` |
| `0x80` | disconnected | none |

A message is only queued when its kind bit is set by a register command (`0x02`, `0x10`, `0x80` may be combined). A full queue drops the message and counts it in the next queued message.

| Command | Effect |
|---|---|
| 1 | load flowgraph: word 1 is the flowgraph id; queues `0x02` |
| 2 | register messages: word 0 is the kind mask |
| 3 | unregister messages: word 0 is the kind mask |
| 5 | enable one XMA context: word 0 is the index |
| 6 | enable a set of XMA contexts: word *n* holds the bits of contexts 32*n* to 32*n*+31 |
| 7 | disable one XMA context |
| 8 | disable a set of XMA contexts |
| 18 | advance the read block: word 0 is the index, word 1 the number of blocks |

Any other command is acknowledged without effect. When `id` is not 0, command `0x10` is queued regardless of the message mask. When `notification` is not 0, it is an APU address whose `UINT32` word is set to 1 at once; the title reads it as volatile.

## XMA decoder

Kind 4 contexts are 64 bytes: 16 `UINT32` words each, written by the title and by the hardware. Enabled contexts are decoded on a background thread every 2 ms into their output rings.

| Word | Bits | Content |
|---|---|---|
| 0 | 0-11 | input 0 size in 2048-byte packets |
| 0 | 12-19 | loop count; 255 loops forever |
| 0 | 20-21 | input validity bits |
| 0 | 22-26 | output ring size in 256-byte blocks |
| 0 | 27-31 | write block, written by the hardware |
| 1 | 0-11 | input 1 size in packets |
| 1 | 12-13 | loop end subframe |
| 1 | 17-19 | loop start subframes to skip |
| 1 | 24-26 | subframes to skip at the start |
| 1 | 27-28 | sample rate: 0 = 24 kHz, 1 = 32 kHz, 2 = 44.1 kHz, 3 = 48 kHz |
| 1 | 29 | stereo |
| 1 | 31 | output writable |
| 2 | 0-25 | read position in bits |
| 2 | 26-30 | error status; 4 = input ran out |
| 2 | 31 | error |
| 3 | 0-25 | loop start position in bits |
| 4 | 0-25 | loop end position in bits |
| 4 | 31 | current input |
| 5 | 0-31 | input 0 APU address |
| 6 | 0-31 | input 1 APU address |
| 7 | 0-31 | output ring APU address |
| 8 | 0-31 | overlap buffer APU address |
| 9 | 0-4 | read block, written by the title through command 18 |

Command 18 moves the read block by its word 1 count, reduced modulo the ring size, so any count wraps.

Each output block is 256 bytes of interleaved 16-bit samples, so 128 samples per channel. Titles read back the write block, the writable bit and the read block, and consume whatever lies between the read and write blocks.

## Diagnostics

| Variable | Output |
|---|---|
| `G6SHIM_AUDIO_CAPTURE=<file.wav>` | The whole process tree's WASAPI render streams, recorded by a helper process through process loopback until the title exits |
| `G6SHIM_XMA_DUMP=<folder>` | `voice_<index>_<sequence>.wav`, one file per enabled voice cycle, and `voices.txt` |

The capture WAV has a canonical 44-byte header: format 1, 2 channels, 48000 Hz, 16 bits, block alignment 4. Packet time stamps place each packet; a gap longer than 96 sample frames (2 ms) becomes silence, so the file keeps real time. Header sizes are valid at all times.

A voice WAV has the same header with the context's channel count (word 1 bit 29) and sample rate (word 1 bits 27-28), and holds exactly the samples written into the voice's output ring from enable to disable, in block order, including the zero padding written when the decoder had no samples. Its header is rewritten every 64 blocks, so a voice that is never disabled still leaves a usable file. Sequence numbers count the voice's dumps in the process, starting at 0.

| `voices.txt` field | Content |
|---|---|
| `index` | XMA context index |
| `rate`, `channels` | Context sample rate in Hz and channel count |
| `produced` | Sample frames per channel written to the ring |
| `consumed` | Advance-read blocks times 128, divided by the channel count |
| `advance_reads`, `advance_blocks` | Number of advance-read commands and the total blocks they moved |
| `buffer_switches` | Times the write block wrapped from the last ring block to 0 |
| `input_switches` | Times decoding moved from input buffer 0 to 1 or back |
| `underrun_reads`, `underrun_blocks` | Advance-read calls that took more blocks than the ring held, and the extra blocks |
| `frames` | Frames read from the input |
| `invalid_input` | Reads where the input packets lay outside one APU block |
| `unreadable` | Reads with no frame at the read bit position |
| `rejected` | Frames the decoder rejected |
| `first_failure`, `first_failure_bit` | The first failure's kind and bit position; `none` when no read failed |
