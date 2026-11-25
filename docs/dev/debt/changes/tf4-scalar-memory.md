# shader

## add Functional

- Scalar reads of a constant-buffer descriptor expose all four words at a fixed table offset. Reads through dynamic table offsets still fail.
  Closes when: the host can supply the descriptor fields through a dynamic table offset.
  Title IDs: 7952987A
- Scalar reads of the upper word of a buffer descriptor picked by a shader-computed resource heap index still fail. The stride may vary with the selected view.
  Closes when: the shader can read descriptor word 1 from the selected heap slot.
  Title IDs: 7952987A
- A shader can read the LOD word of an ordinary sampler at a fixed heap slot. Extended sampler descriptors and shader-computed sampler indices are unavailable.
  Closes when: the runtime can supply the required tuning fields and indexed sampler descriptor data.
  Title IDs: 7952987A
- Scalar shader-resource descriptor reads expose the buffer length and address at a fixed table offset. Ordinary linear raw and structured views and the supported typed formats also expose the complete high address and format words. Other view kinds expose only the fields the host can derive.
  Closes when: the translator can recover those fields from the bound resource.
  Title IDs: 7952987A
- Scalar image descriptor reads expose the base-address word and the ordinary two-dimensional view's sampling fields at a fixed table offset. The translator can also read mip-zero dimensions and the view's starting mip without an image operation. Shader-computed view indices and other layout and format fields remain unavailable.
  Closes when: the translator can recover the additional fields from the bound image view.
  Title IDs: 7952987A
