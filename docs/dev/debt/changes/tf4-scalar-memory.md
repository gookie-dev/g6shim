# shader

## add Functional

- Scalar reads of a constant-buffer descriptor expose all four words at a fixed table offset. Reads through dynamic table offsets still fail.
  Closes when: the host can supply the descriptor fields through a dynamic table offset.
  Title IDs: 7952987A
- Scalar reads of the upper word of a buffer descriptor picked by a shader-computed resource heap index still fail. The stride may vary with the selected view.
  Closes when: the shader can read descriptor word 1 from the selected heap slot.
  Title IDs: 7952987A
- Indexed dword loads can use the stride of the selected structured buffer. Indexed stores and atomics still require a declared stride. An indexed load with a view that has no supported raw alias returns zero.
  Closes when: indexed stores and atomics use the selected view's stride and the runtime can supply raw aliases for the remaining buffer views.
  Title IDs: 7952987A
- A shader can read the LOD word of an ordinary sampler at a fixed heap slot. Extended sampler descriptors and shader-computed sampler indices are unavailable.
  Closes when: the runtime can supply the required tuning fields and indexed sampler descriptor data.
  Title IDs: 7952987A
- Scalar shader-resource descriptor reads expose the buffer length and address at a fixed table offset. Ordinary linear raw and structured views and the supported typed formats also expose the complete high address and format words. Other view kinds expose only the fields the host can derive.
  Closes when: the translator can recover those fields from the bound resource.
  Title IDs: 7952987A
- Scalar image descriptor reads expose the base-address word and the ordinary two-dimensional view's sampling fields at a fixed table offset. The translator can also read mip-zero dimensions and the view's starting mip without an image operation, including through computed heap indices for ordinary two-dimensional shader-resource views. Other layout fields, view types, and indexed reads in compute shaders with group barriers remain unavailable.
  Closes when: the translator can recover the additional fields from the bound image view.
  Title IDs: 7952987A
