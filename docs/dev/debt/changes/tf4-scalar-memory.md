# shader

## add Functional

- Scalar reads of a constant-buffer descriptor fail when a shader consumes its address, record count or format word.
  Closes when: the host supplies the descriptor fields and the translator verifies the result with synthetic tests.
  Title IDs: 7952987A
- Scalar shader-resource descriptor reads expose the buffer length. Reads of the address or format still fail.
  Closes when: the translator can recover those fields from the bound resource.
  Title IDs: 7952987A
