// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "g6shim/shader/emit/shader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace g6shim::shader::emit {

namespace {

constexpr std::string_view kMagic = "DXIL";
constexpr char kEntryName[] = "main";
constexpr std::uint32_t kDxilHeaderSize = 24;
constexpr std::uint32_t kDxilVersion = 0x106;
constexpr std::uint32_t kBitcodeOffset = 16;
constexpr std::uint32_t kProgramKindShift = 16;
constexpr std::uint32_t kProgramKindMask = 0x66;
constexpr std::uint32_t kRootCbvSize = 65536;
constexpr std::uint32_t kRootCbvBlobDwords = 16384;
constexpr std::uint32_t kFeatureStencilRef = 0x200;
constexpr std::uint32_t kFeatureTypedUavLoads = 0x800;
constexpr std::uint32_t kFeatureWaveOps = 0x4000;
constexpr std::uint32_t kFeatureInt64 = 0x8000;
constexpr std::uint32_t kFeatureResourceHeap = 0x200'0000;
constexpr std::uint32_t kFeatureSamplerHeap = 0x400'0000;
constexpr std::uint32_t kFeatureAtomicInt64OnTypedResource = 0x400000;
constexpr std::uint32_t kFeatureAtomicInt64OnHeapResource = 0x10000000;
constexpr std::uint32_t kComparisonSampler = 0x8000;
constexpr std::uint32_t kUnallocatedRow = UINT32_MAX;
constexpr std::uint32_t kSample = 60;
constexpr std::uint32_t kSampleBias = 61;
constexpr std::uint32_t kSampleCmp = 64;
constexpr std::uint32_t kCalculateLod = 81;
constexpr std::uint32_t kGsStateTag = 1;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::uint32_t round_up(std::uint32_t value, std::uint32_t multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

std::uint32_t signature_width(const std::vector<SigElement>& elements) {
    std::uint32_t count = 0;
    for (const SigElement& element : elements)
        if (element.row)
            count = std::max(count, *element.row * 4 + static_cast<std::uint32_t>(element.col + element.cols));
    return count;
}

}  // namespace

Shader::Shader(bool pixel_shader) : pixel(pixel_shader) {
    const Ty void_type = m.void_type();
    const Ty i1 = m.int_type(1);
    const Ty i8 = m.int_type(8);
    const Ty i32 = m.int_type(32);
    const Ty f32 = m.float_type();
    const Ty i8p = m.ptr(i8);
    const Ty handle = m.named_struct("dx.types.Handle", {i8p});
    const Ty props = m.named_struct("dx.types.ResourceProperties", {i32, i32});
    const Ty bind = m.named_struct("dx.types.ResBind", {i32, i32, i32, i8});
    const Ty cbuf_ret = m.named_struct("dx.types.CBufRet.i32", {i32, i32, i32, i32});
    const Ty res_ret = m.named_struct("dx.types.ResRet.f32", {f32, f32, f32, f32, i32});
    const Ty main_ty = m.func_ty(void_type, {});
    m.ptr(main_ty);
    main = m.function(kEntryName, main_ty, std::nullopt, true);
    entry = m.new_block();
    t = Types{void_type, i1, i32, f32, handle, props, bind, cbuf_ret, res_ret};
}

V Shader::lds() {
    if (lds_) return *lds_;
    const Ty array = m.array(lds_dwords, t.i32);
    const V value = m.global("lds", array, 3);
    lds_ = value;
    return value;
}

V Shader::op(std::size_t block, const std::string& name, Attr attr, Ty ret, std::uint32_t opcode,
             const std::vector<V>& args) {
    if (opcode == kSample || opcode == kSampleBias || opcode == kSampleCmp || opcode == kCalculateLod)
        uses_implicit_lod_ = true;
    std::vector<Ty> params = {t.i32};
    params.reserve(params.size() + args.size());
    for (const V arg : args) params.push_back(m.type_of(arg));
    const Ty function_type = m.func_ty(ret, std::move(params));
    m.ptr(function_type);
    const V function = m.function("dx.op." + name, function_type, attr, false);
    std::vector<V> all = {m.i32(opcode)};
    all.insert(all.end(), args.begin(), args.end());
    return m.call(block, function, std::move(all));
}

V Shader::user_data_handle() {
    const std::array<V, 4> bind = {m.i32(0), m.i32(0), m.i32(kUserDataSpace), m.i8(2)};
    const V binding = m.aggregate(t.bind, bind);
    const std::size_t block = entry;
    const V index = m.i32(0);
    const V non_uniform = m.boolean(false);
    const V handle =
        op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {binding, index, non_uniform});
    return annotate(block, handle, 13, kUnallocatedRow);
}

V Shader::root_cbv_handle(std::uint8_t slot) {
    for (const auto& entry_value : root_cbvs)
        if (entry_value.first == slot) return entry_value.second;
    const V register_value = m.i32(slot);
    const std::array<V, 4> bind = {register_value, register_value, m.i32(kRootCbvSpace), m.i8(2)};
    const V binding = m.aggregate(t.bind, bind);
    const V non_uniform = m.boolean(false);
    const std::size_t block = entry;
    const V raw =
        op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {binding, register_value, non_uniform});
    const V handle = annotate(block, raw, 13, kRootCbvSize);
    root_cbvs.emplace_back(slot, handle);
    return handle;
}

V Shader::root_srv_handle(std::uint8_t slot) {
    for (const auto& entry_value : root_srvs)
        if (entry_value.first == slot) return entry_value.second;
    const V register_value = m.i32(slot);
    const std::array<V, 4> bind = {register_value, register_value, m.i32(kRootCbvSpace), m.i8(0)};
    const V binding = m.aggregate(t.bind, bind);
    const V non_uniform = m.boolean(false);
    const std::size_t block = entry;
    const V raw =
        op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {binding, register_value, non_uniform});
    const V handle = annotate(block, raw, 11, 0);
    flags |= feature::kRawBuffers;
    root_srvs.emplace_back(slot, handle);
    return handle;
}

V Shader::sampler_handle(std::size_t block, std::uint32_t n) {
    const std::array<V, 4> bind = {m.i32(n), m.i32(n), m.i32(kSamplerSpace), m.i8(3)};
    const V binding = m.aggregate(t.bind, bind);
    const V index = m.i32(n);
    const V non_uniform = m.boolean(false);
    const V handle =
        op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {binding, index, non_uniform});
    const bool comparison = n < 64 && (cmp_samplers >> n & 1) != 0;
    return annotate(block, handle, 14 | (comparison ? kComparisonSampler : 0), 0);
}

V Shader::annotate(std::size_t block, V handle, std::uint32_t props0, std::uint32_t props1) {
    const std::array<V, 2> values = {m.i32(props0), m.i32(props1)};
    const V props = m.aggregate(t.props, values);
    return op(block, "annotateHandle", Attr::ReadNone, t.handle, 216, {handle, props});
}

V Shader::heap_handle(std::size_t block, V index, bool sampler, bool nonuniform,
                      std::pair<std::uint32_t, std::uint32_t> props) {
    flags |= sampler ? feature::kSamplerHeap : feature::kResourceHeap;
    nonuniform_handles += nonuniform ? 1u : 0u;
    const V is_sampler = m.boolean(sampler);
    const V non_uniform = m.boolean(nonuniform);
    const V handle =
        op(block, "createHandleFromHeap", Attr::ReadNone, t.handle, 218, {index, is_sampler, non_uniform});
    return annotate(block, handle, props.first, props.second);
}

std::vector<std::uint8_t> Shader::finish() {
    const std::uint32_t ud_size = round_up(user_data * 4, 16);
    fix_user_data_size(ud_size);
    std::optional<Md> resources;
    std::vector<Binding> bindings;
    const auto i32c = [this](std::uint32_t value) {
        const V constant = m.i32(value);
        return m.md_value(constant);
    };
    if (user_data > 0 || samplers > 0 || !root_cbvs.empty() || !root_srvs.empty()) {
        const Md empty = m.md_str("");
        std::optional<Md> cbvs;
        std::vector<std::optional<Md>> cbv_list;
        if (user_data > 0) {
            const Ty array = m.array(round_up(user_data, 4), t.i32);
            const Ty type = m.named_struct("UserData", {array});
            const Ty pointer = m.ptr(type);
            const V undef = m.undef(pointer);
            const std::vector<std::optional<Md>> ops = {
                i32c(0), m.md_value(undef), empty, i32c(kUserDataSpace), i32c(0),
                i32c(1), i32c(ud_size), std::nullopt,
            };
            cbv_list.push_back(m.md_node(ops));
            bindings.push_back(Binding{2, kUserDataSpace, 0, 13});
        }
        const Ty blob = m.array(kRootCbvBlobDwords, t.i32);
        const Ty root_type = m.named_struct("RootCbv", {blob});
        const Ty root_pointer = m.ptr(root_type);
        const V root_undef = m.undef(root_pointer);
        for (const auto& slot : root_cbvs) {
            const std::vector<std::optional<Md>> ops = {
                i32c(static_cast<std::uint32_t>(cbv_list.size())),
                m.md_value(root_undef),
                empty,
                i32c(kRootCbvSpace),
                i32c(slot.first),
                i32c(1),
                i32c(kRootCbvSize),
                std::nullopt,
            };
            cbv_list.push_back(m.md_node(ops));
            bindings.push_back(Binding{2, kRootCbvSpace, slot.first, 13});
        }
        if (!cbv_list.empty()) cbvs = m.md_node(std::move(cbv_list));

        std::optional<Md> srvs;
        if (!root_srvs.empty()) {
            const Ty type = m.named_struct("struct.ByteAddressBuffer", {t.i32});
            const Ty pointer = m.ptr(type);
            const V undef = m.undef(pointer);
            std::vector<std::optional<Md>> list;
            for (std::size_t n = 0; n < root_srvs.size(); ++n) {
                const auto& slot = root_srvs[n];
                const std::vector<std::optional<Md>> ops = {
                    i32c(static_cast<std::uint32_t>(n)),
                    m.md_value(undef),
                    empty,
                    i32c(kRootCbvSpace),
                    i32c(slot.first),
                    i32c(1),
                    i32c(11),
                    i32c(0),
                    std::nullopt,
                };
                list.push_back(m.md_node(ops));
                bindings.push_back(Binding{4, kRootCbvSpace, slot.first, 11});
            }
            srvs = m.md_node(std::move(list));
        }

        std::optional<Md> sampler_nodes;
        if (samplers > 0) {
            const Ty type = m.named_struct("struct.SamplerState", {t.i32});
            const Ty pointer = m.ptr(type);
            const V undef = m.undef(pointer);
            std::vector<std::optional<Md>> list;
            for (std::uint32_t n = 0; n < samplers; ++n) {
                const std::uint32_t kind = n < 64 ? static_cast<std::uint32_t>(cmp_samplers >> n & 1) : 0;
                const std::vector<std::optional<Md>> ops = {
                    i32c(n), m.md_value(undef), empty, i32c(kSamplerSpace),
                    i32c(n), i32c(1), i32c(kind), std::nullopt,
                };
                list.push_back(m.md_node(ops));
                bindings.push_back(Binding{1, kSamplerSpace, n, 14});
            }
            sampler_nodes = m.md_node(std::move(list));
        }
        resources = m.md_node({srvs, std::nullopt, cbvs, sampler_nodes});
        std::stable_sort(bindings.begin(), bindings.end(), [](const Binding& a, const Binding& b) {
            const auto rank = [](const Binding& binding) {
                if (binding.kind == 2) return 0;
                if (binding.kind == 1) return 1;
                return 2;
            };
            return rank(a) < rank(b);
        });
    }
    if (resources) m.named_md("dx.resources", {*resources});

    const bool graphics = !threads.has_value() || mesh.has_value();
    if (graphics) {
        const std::uint32_t ni = signature_width(inputs);
        const std::uint32_t no = signature_width(outputs);
        std::vector<std::uint32_t> view{ni, no};
        if (mesh) view.push_back(signature_width(prims));
        for (std::uint32_t i = 0; i < ni; ++i) {
            for (std::uint32_t k = 0; k < (no + 31) / 32; ++k) {
                const std::uint32_t bits = std::min(no - 32 * k, 32u);
                view.push_back(bits == 32 ? UINT32_MAX : (1u << bits) - 1u);
            }
        }
        if (geometry) {
            constexpr std::uint32_t kUnusedGeometryStreams = 3;
            view.insert(view.end(), kUnusedGeometryStreams, 0u);
        }
        const Ty array = m.array(static_cast<std::uint32_t>(view.size()), t.i32);
        std::vector<V> elements;
        elements.reserve(view.size());
        for (const std::uint32_t value : view) elements.push_back(m.i32(value));
        const V aggregate = m.aggregate(array, elements);
        const Md value = m.md_value(aggregate);
        const Md node = m.md_node({value});
        m.named_md("dx.viewIdState", {node});
    }

    std::optional<Md> signatures;
    if (graphics) {
        const std::optional<Md> ins = signature_md(inputs);
        const std::optional<Md> outs = signature_md(outputs);
        const std::optional<Md> prim_outputs = signature_md(prims);
        signatures = m.md_node({ins, outs, prim_outputs});
    }

    if (pixel && std::any_of(outputs.begin(), outputs.end(),
                             [](const SigElement& element) { return element.kind == 20; }))
        flags |= feature::kStencilRef;

    std::vector<std::optional<Md>> props;
    if (flags != 0) {
        props.push_back(i32c(0));
        const Ty i64t = m.int_type(64);
        const V value = m.int_const(i64t, static_cast<std::int64_t>(flags));
        props.push_back(m.md_value(value));
    }
    if (threads) {
        std::vector<std::optional<Md>> thread_nodes;
        for (const std::uint32_t n : *threads) thread_nodes.push_back(i32c(n));
        const Md thread_group = m.md_node(std::move(thread_nodes));
        if (mesh) {
            props.push_back(i32c(9));
            std::vector<std::optional<Md>> node = {thread_group};
            node.push_back(i32c(mesh->max_vertices));
            node.push_back(i32c(mesh->max_primitives));
            node.push_back(i32c(mesh->topology));
            node.push_back(i32c(0));
            props.push_back(m.md_node(std::move(node)));
        } else {
            props.push_back(i32c(4));
            props.push_back(thread_group);
        }
    }
    if (geometry) {
        props.push_back(i32c(kGsStateTag));
        props.push_back(m.md_node({i32c(geometry->input_primitive), i32c(geometry->max_vertices),
                                   i32c(geometry->stream_mask), i32c(geometry->output_topology),
                                   i32c(geometry->instances)}));
    }
    if (wave_size) {
        props.push_back(i32c(11));
        props.push_back(m.md_node({i32c(*wave_size)}));
    }
    std::optional<Md> props_node;
    if (!props.empty()) props_node = m.md_node(std::move(props));

    const Md main_md = m.md_value(main);
    const Md name = m.md_str(kEntryName);
    const Md entry_node = m.md_node({main_md, name, signatures, resources, props_node});

    const Md ident_text = m.md_str("g6shim");
    const Md ident = m.md_node({ident_text});
    m.named_md("llvm.ident", {ident});
    const Md one = i32c(1);
    const Md six = i32c(6);
    const Md version = m.md_node({one, six});
    m.named_md("dx.version", {version});
    constexpr std::uint32_t kComputeImplicitLodValidatorMinor = 8;
    const bool compute_implicit_lod = threads && !mesh && uses_implicit_lod_;
    const Md validator_version = compute_implicit_lod
                                     ? m.md_node({one, i32c(kComputeImplicitLodValidatorMinor)})
                                     : version;
    m.named_md("dx.valver", {validator_version});

    const char* program_name = "vs";
    std::uint32_t program_kind = 1;
    if (mesh) {
        program_name = "ms";
        program_kind = 13;
    } else if (geometry) {
        program_name = "gs";
        program_kind = 2;
    } else if (threads) {
        program_name = "cs";
        program_kind = 5;
    } else if (pixel) {
        program_name = "ps";
        program_kind = 0;
    }
    const Md shader_name = m.md_str(program_name);
    const Md shader_model = m.md_node({shader_name, six, six});
    m.named_md("dx.shaderModel", {shader_model});
    m.named_md("dx.entryPoints", {entry_node});

    const std::vector<std::uint8_t> bitcode = m.write();
    std::vector<std::uint8_t> dxil;
    dxil.reserve(kDxilHeaderSize + bitcode.size());
    append_u32(dxil, program_kind << kProgramKindShift | kProgramKindMask);
    append_u32(dxil, static_cast<std::uint32_t>((kDxilHeaderSize + bitcode.size()) / 4));
    dxil.insert(dxil.end(), kMagic.begin(), kMagic.end());
    append_u32(dxil, kDxilVersion);
    append_u32(dxil, kBitcodeOffset);
    append_u32(dxil, static_cast<std::uint32_t>(bitcode.size()));
    dxil.insert(dxil.end(), bitcode.begin(), bitcode.end());

    std::uint64_t features = 0;
    if ((flags & feature::kStencilRef) != 0) features |= kFeatureStencilRef;
    if ((flags & feature::kTypedUavLoads) != 0) features |= kFeatureTypedUavLoads;
    if ((flags & feature::kWaveOps) != 0) features |= kFeatureWaveOps;
    if ((flags & feature::kInt64) != 0) features |= kFeatureInt64;
    if ((flags & feature::kResourceHeap) != 0) features |= kFeatureResourceHeap;
    if ((flags & feature::kSamplerHeap) != 0) features |= kFeatureSamplerHeap;
    if ((flags & feature::kAtomicInt64OnTypedResource) != 0)
        features |= kFeatureAtomicInt64OnTypedResource;
    if ((flags & feature::kAtomicInt64OnHeapResource) != 0)
        features |= kFeatureAtomicInt64OnHeapResource;
    std::array<std::uint8_t, 8> sfi0{};
    for (std::size_t i = 0; i < sfi0.size(); ++i) sfi0[i] = static_cast<std::uint8_t>(features >> (8 * i));
    const std::vector<std::uint8_t> isg1 = signature(inputs, true);
    const std::vector<std::uint8_t> osg1 = signature(outputs, false);

    bool flag = false;
    if (pixel) {
        for (const SigElement& element : outputs)
            if (element.kind >= 17 && element.kind <= 19) flag = true;
    } else {
        for (const SigElement& element : outputs)
            if (element.kind == 3) flag = true;
    }

    Mesh mesh_value{};
    if (mesh) mesh_value = *mesh;
    std::optional<std::pair<Mesh, std::uint32_t>> mesh_psv;
    if (mesh) mesh_psv = std::pair<Mesh, std::uint32_t>{mesh_value, lds_dwords * 4};
    const Psv psv_data{
        static_cast<std::uint8_t>(program_kind),
        flag && !threads.has_value(),
        inputs,
        outputs,
        prims,
        mesh_psv,
        geometry,
        bindings,
        threads.value_or(std::array<std::uint32_t, 3>{0, 0, 0}),
        wave_size,
        compute_implicit_lod ? std::string_view{kEntryName} : std::string_view{},
    };
    const std::vector<std::uint8_t> psv0 = psv(psv_data);
    const std::array<std::uint8_t, 20> container_hash{};
    const std::vector<std::uint8_t> psg1 = signature(prims, false);

    std::vector<ContainerPart> parts = {
        ContainerPart{{'S', 'F', 'I', '0'}, sfi0},
        ContainerPart{{'I', 'S', 'G', '1'}, isg1},
        ContainerPart{{'O', 'S', 'G', '1'}, osg1},
    };
    if (mesh) parts.push_back(ContainerPart{{'P', 'S', 'G', '1'}, psg1});
    parts.push_back(ContainerPart{{'P', 'S', 'V', '0'}, psv0});
    parts.push_back(ContainerPart{{'H', 'A', 'S', 'H'}, container_hash});
    parts.push_back(ContainerPart{{'D', 'X', 'I', 'L'}, dxil});
    return build(parts);
}

void Shader::fix_user_data_size(std::uint32_t size) {
    const std::array<V, 2> old_values = {m.i32(13), m.i32(kUnallocatedRow)};
    const V old_value = m.aggregate(t.props, old_values);
    const std::array<V, 2> new_values = {m.i32(13), m.i32(size)};
    const V new_value = m.aggregate(t.props, new_values);
    m.replace_const(old_value, new_value);
}

std::optional<Md> Shader::signature_md(std::vector<SigElement> elements) {
    if (elements.empty()) return std::nullopt;
    std::vector<std::optional<Md>> list;
    for (std::size_t id = 0; id < elements.size(); ++id) {
        const SigElement& element = elements[id];
        const V index_value = m.i32(element.index);
        const std::optional<Md> index = m.md_value(index_value);
        const Md indices = m.md_node({index});
        std::optional<Md> usage;
        if (element.usage != 0) {
            const V tag = m.i32(3);
            const std::optional<Md> tag_md = m.md_value(tag);
            const V mask = m.i32(element.usage);
            const std::optional<Md> mask_md = m.md_value(mask);
            usage = m.md_node({tag_md, mask_md});
        }
        std::vector<std::optional<Md>> ops;
        const V id_value = m.i32(static_cast<std::uint32_t>(id));
        ops.push_back(m.md_value(id_value));
        ops.push_back(m.md_str(element.name));
        const V comp = m.i8(element.comp);
        ops.push_back(m.md_value(comp));
        const V kind = m.i8(element.kind);
        ops.push_back(m.md_value(kind));
        ops.push_back(indices);
        const V interp = m.i8(element.interp);
        ops.push_back(m.md_value(interp));
        const V one = m.i32(1);
        ops.push_back(m.md_value(one));
        const V cols = m.i8(element.cols);
        ops.push_back(m.md_value(cols));
        const V row = m.i32(element.row.value_or(kUnallocatedRow));
        ops.push_back(m.md_value(row));
        const V col = m.i8(element.row ? element.col : static_cast<std::uint8_t>(255));
        ops.push_back(m.md_value(col));
        ops.push_back(usage);
        list.push_back(m.md_node(std::move(ops)));
    }
    return m.md_node(std::move(list));
}

}  // namespace g6shim::shader::emit
