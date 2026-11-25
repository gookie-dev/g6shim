# shader

## add Functional

- Scalar reads of a constant-buffer descriptor expose the low address word at a fixed table offset. Reads of the other address bits, record count or format still fail.
  Closes when: the host supplies the descriptor fields and the translator verifies the result with synthetic tests.
  Title IDs: 7952987A
- Scalar shader-resource descriptor reads expose the buffer length and address at a fixed table offset. Ordinary linear raw and structured views also expose the complete high address and format words. Other view kinds expose only the fields the host can derive.
  Closes when: the translator can recover those fields from the bound resource.
  Title IDs: 7952987A
- Scalar image descriptor reads expose the base-address word at a fixed table offset. The translator still cannot supply all layout and format words.
  Closes when: the translator can recover the additional fields from the bound image view.
  Title IDs: 7952987A
