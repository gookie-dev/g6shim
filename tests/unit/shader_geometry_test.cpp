// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "shader_lowering.hpp"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "g6shim/shader/dxbc.hpp"
#include "g6shim/shader/emit/emit.hpp"
#include "g6shim/shader/emit/shader.hpp"

namespace {

using namespace g6shim::shader;
using namespace g6shim::shader::emit;
using g6shim::test::lowering::check_dxil;

constexpr std::uint32_t kGsWords[] = {
    0x9400ff03, 0x00080008, 0xbf068000, 0xbf850032, 0x92fe8000, 0xbf06c000, 0x85fe7ec1, 0x34040082,
    0x360404ff, 0x0003fffc, 0xbe810382, 0x340600f9, 0x05860601, 0x7e0802f9, 0x00040601, 0xd8d80010,
    0x05000002, 0xd8d80010, 0x0a000003, 0xf4000104, 0xfa000000, 0x8f048204, 0x81048404, 0xd543000b,
    0x040a0404, 0x7e0c02f1, 0x7e0e02f1, 0x7e100280, 0x7e1202f2, 0xd8380100, 0x0007060b, 0xd8380302,
    0x0009080b, 0xd8340010, 0x0000050b, 0xbf900022, 0x7e0c02f0, 0xd8380908, 0x0007060b, 0xd8380b0a,
    0x0009080b, 0xd8340030, 0x0000050b, 0xbf900022, 0x7e0c0280, 0x7e0e02f0, 0xd8381110, 0x0007060b,
    0xd8381312, 0x0009080b, 0xd8340050, 0x0000050b, 0xbf900022, 0xbf900012, 0xbf900003, 0xbf810000,
};
constexpr std::uint32_t kTarget = 16;
constexpr std::uint32_t kCentre = 8;
constexpr float kNegOne = -1.0f;
constexpr float kOne = 1.0f;
constexpr std::uint8_t kRed = 200;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    append_u32(out, static_cast<std::uint32_t>(value));
    append_u32(out, static_cast<std::uint32_t>(value >> 32));
}

void set_u32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void set_u64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
    set_u32(out, at, static_cast<std::uint32_t>(value));
    set_u32(out, at + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kModulus = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kModulus;
        b = (b + a) % kModulus;
    }
    return b << 16 | a;
}

std::vector<std::uint8_t> zlib_stored(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(65535, data.size() - at);
        const bool final = at + length == data.size();
        out.push_back(final ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(length));
        out.push_back(static_cast<std::uint8_t>(inverse));
        out.push_back(static_cast<std::uint8_t>(inverse >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at),
                   data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < data.size());
    const std::uint32_t check = adler32(data);
    out.push_back(static_cast<std::uint8_t>(check >> 24));
    out.push_back(static_cast<std::uint8_t>(check >> 16));
    out.push_back(static_cast<std::uint8_t>(check >> 8));
    out.push_back(static_cast<std::uint8_t>(check));
    return out;
}

std::vector<std::uint8_t> geometry_container(std::span<const std::uint32_t> words = kGsWords,
                                             std::uint8_t input_texcoord_columns = 1) {
    constexpr std::size_t kStageData = 0x14C + 8 * (4 + 10 * 10 * 4);
    constexpr std::size_t kRecord = 0x54;
    constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFull;
    std::vector<std::uint8_t> code;
    code.reserve(words.size_bytes());
    for (const std::uint32_t word : words) append_u32(code, word);
    std::vector<std::uint8_t> payload;
    append_u64(payload, 6);
    payload.resize(0x68, 0);
    for (const std::size_t slot : {0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x48, 0x60}) set_u64(payload, slot, kAbsent);
    const std::uint64_t record = payload.size();
    set_u64(payload, 0x20, record);
    const std::uint64_t code_at = record + kRecord + kStageData;
    append_u64(payload, code_at);
    append_u64(payload, code.size());
    payload.resize(static_cast<std::size_t>(record) + 0x50, 0);
    append_u32(payload, 28);
    payload.resize(static_cast<std::size_t>(record) + kRecord + kStageData, 0);
    payload.insert(payload.end(), code.begin(), code.end());
    std::vector<std::uint8_t> fssm{'F', 'S', 'S', 'M'};
    append_u32(fssm, 0x101);
    append_u64(fssm, payload.size());
    fssm.insert(fssm.end(), payload.begin(), payload.end());
    const std::vector<std::uint8_t> stream = zlib_stored(fssm);
    std::vector<std::uint8_t> xbch{'X', 'B', 'C', 'H'};
    append_u32(xbch, 3);
    append_u64(xbch, stream.size());
    append_u64(xbch, fssm.size());
    xbch.insert(xbch.end(), stream.begin(), stream.end());
    std::vector<std::uint8_t> dxil(0x18, 0);
    dxil.insert(dxil.end(), {'X', 'D', 'A', 'T'});
    append_u32(dxil, 1);
    append_u32(dxil, 0x24);
    const std::size_t blob_at = dxil.size() + 12;
    append_u32(dxil, 0x0D);
    append_u32(dxil, static_cast<std::uint32_t>(blob_at));
    append_u32(dxil, static_cast<std::uint32_t>(xbch.size()));
    dxil.insert(dxil.end(), xbch.begin(), xbch.end());
    const std::size_t bitcode_at = dxil.size();
    dxil.resize(bitcode_at + 16, 0);
    while (dxil.size() % 4 != 0) dxil.push_back(0);
    set_u32(dxil, 0, 2u << 16 | 0x66);
    set_u32(dxil, 4, static_cast<std::uint32_t>(dxil.size() / 4));
    dxil[8] = 'D';
    dxil[9] = 'X';
    dxil[10] = 'I';
    dxil[11] = 'L';
    set_u32(dxil, 12, 0x106);
    set_u32(dxil, 16, static_cast<std::uint32_t>(bitcode_at - 8));
    set_u32(dxil, 20, 16);

    SigElement position;
    position.name = "SV_Position";
    position.kind = 3;
    position.comp = 9;
    position.row = 0;
    position.col = 0;
    position.cols = 4;
    SigElement texcoord;
    texcoord.name = "TEXCOORD";
    texcoord.kind = 0;
    texcoord.comp = 9;
    texcoord.row = 1;
    texcoord.col = 0;
    texcoord.cols = 1;
    const std::vector<SigElement> outputs{position, texcoord};
    texcoord.cols = input_texcoord_columns;
    const std::vector<SigElement> inputs{position, texcoord};
    const std::vector<std::uint8_t> isg1 = signature(inputs, true);
    const std::vector<std::uint8_t> osg1 = signature(outputs, false);
    const std::array<std::pair<FourCC, std::span<const std::uint8_t>>, 3> parts{{
        {kDxil, dxil},
        {kIsg1, isg1},
        {kOsg1, osg1},
    }};
    std::vector<std::uint8_t> out(0x20 + 4 * parts.size(), 0);
    out[0] = 'D';
    out[1] = 'X';
    out[2] = 'B';
    out[3] = 'C';
    set_u32(out, 0x14, 1);
    std::size_t at = out.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        set_u32(out, 0x20 + 4 * i, static_cast<std::uint32_t>(at));
        at += 8 + parts[i].second.size();
    }
    set_u32(out, 0x18, static_cast<std::uint32_t>(at));
    set_u32(out, 0x1C, static_cast<std::uint32_t>(parts.size()));
    for (const auto& part : parts) {
        out.insert(out.end(), part.first.bytes.begin(), part.first.bytes.end());
        append_u32(out, static_cast<std::uint32_t>(part.second.size()));
        out.insert(out.end(), part.second.begin(), part.second.end());
    }
    return out;
}

std::vector<std::uint8_t> pass_vertex() {
    Shader shader(false);
    SigElement id;
    id.name = "SV_VertexID";
    id.kind = 1;
    id.comp = 5;
    id.row = 2;
    id.col = 0;
    id.cols = 1;
    shader.inputs.push_back(id);
    SigElement position;
    position.name = "SV_Position";
    position.kind = 3;
    position.comp = 9;
    position.interp = 4;
    position.row = 0;
    position.col = 0;
    position.cols = 4;
    position.usage = 15;
    shader.outputs.push_back(position);
    SigElement texcoord;
    texcoord.name = "TEXCOORD";
    texcoord.kind = 0;
    texcoord.comp = 9;
    texcoord.interp = 2;
    texcoord.row = 1;
    texcoord.col = 0;
    texcoord.cols = 1;
    texcoord.usage = 1;
    shader.outputs.push_back(texcoord);
    const std::size_t block = shader.entry;
    const V vertex = shader.op(block, "loadInput.i32", Attr::ReadNone, shader.t.i32, 4,
                               {shader.m.i32(0), shader.m.i32(0), shader.m.i8(0), shader.m.undef(shader.t.i32)});
    const V first = shader.m.cmp(block, pred::kEq, vertex, shader.m.i32(0));
    const V colour = shader.m.select(block, first, shader.m.f32(std::bit_cast<std::uint32_t>(kOne)),
                                     shader.m.f32(0));
    const float position_words[4] = {kNegOne, kNegOne, 0.0f, kOne};
    for (std::uint8_t channel = 0; channel < 4; ++channel) {
        shader.op(block, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5,
                  {shader.m.i32(0), shader.m.i32(0), shader.m.i8(channel),
                   shader.m.f32(std::bit_cast<std::uint32_t>(position_words[channel]))});
    }
    shader.op(block, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5,
              {shader.m.i32(1), shader.m.i32(0), shader.m.i8(0), colour});
    shader.m.terminate(block, Term::ret());
    return shader.finish();
}

std::vector<std::uint8_t> colour_pixel() {
    Shader shader(true);
    SigElement texcoord;
    texcoord.name = "TEXCOORD";
    texcoord.kind = 0;
    texcoord.comp = 9;
    texcoord.interp = 2;
    texcoord.row = 1;
    texcoord.col = 0;
    texcoord.cols = 1;
    texcoord.usage = 1;
    shader.inputs.push_back(texcoord);
    SigElement target;
    target.name = "SV_Target";
    target.kind = 16;
    target.comp = 9;
    target.row = 0;
    target.col = 0;
    target.cols = 4;
    target.usage = 15;
    shader.outputs.push_back(target);
    const std::size_t block = shader.entry;
    const V red = shader.op(block, "loadInput.f32", Attr::ReadNone, shader.t.f32, 4,
                            {shader.m.i32(0), shader.m.i32(0), shader.m.i8(0), shader.m.undef(shader.t.i32)});
    const float rest[3] = {0.0f, 0.0f, kOne};
    shader.op(block, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5,
              {shader.m.i32(0), shader.m.i32(0), shader.m.i8(0), red});
    for (std::uint8_t channel = 1; channel < 4; ++channel) {
        shader.op(block, "storeOutput.f32", Attr::NoUnwind, shader.t.void_type, 5,
                  {shader.m.i32(0), shader.m.i32(0), shader.m.i8(channel),
                   shader.m.f32(std::bit_cast<std::uint32_t>(rest[channel - 1]))});
    }
    shader.m.terminate(block, Term::ret());
    return shader.finish();
}

struct Release {
    template <class T>
    void operator()(T* object) const {
        if (object != nullptr) object->Release();
    }
};

bool warp_pixel(const std::vector<std::uint8_t>& vertex, const std::vector<std::uint8_t>& geometry,
                const std::vector<std::uint8_t>& pixel, std::uint8_t* red) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    const HRESULT adapter_result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(adapter_result)) return false;
    ID3D12Device* device = nullptr;
    const HRESULT device_result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    adapter->Release();
    if (FAILED(device_result)) return false;
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
        device->Release();
        return false;
    }
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    const auto close = [&]() {
        Release{}(heap);
        Release{}(readback);
        Release{}(target);
        Release{}(pso);
        Release{}(root);
        Release{}(fence);
        Release{}(list);
        Release{}(allocator);
        Release{}(queue);
        Release{}(device);
    };
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        close();
        return false;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)))) {
        if (error != nullptr) error->Release();
        if (blob != nullptr) blob->Release();
        close();
        return false;
    }
    blob->Release();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {vertex.data(), vertex.size()};
    desc.GS = {geometry.data(), geometry.size()};
    desc.PS = {pixel.data(), pixel.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
        close();
        return false;
    }
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = kTarget;
    resource.Height = kTarget;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource.SampleDesc.Count = 1;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)))) {
        close();
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heaps{};
    heaps.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heaps.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heaps, IID_PPV_ARGS(&heap)))) {
        close();
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target, nullptr, rtv);
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = kTarget * kTarget * 4;
    resource.Height = 1;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        close();
        return false;
    }
    const float black[4] = {};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kTarget), static_cast<float>(kTarget), 0, 1};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kTarget), static_cast<LONG>(kTarget)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root);
    list->SetPipelineState(pso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = readback;
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dest.PlacedFootprint.Footprint.Width = kTarget;
    dest.PlacedFootprint.Footprint.Height = kTarget;
    dest.PlacedFootprint.Footprint.Depth = 1;
    dest.PlacedFootprint.Footprint.RowPitch = kTarget * 4;
    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close())) {
        close();
        return false;
    }
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(fence, 1))) {
        close();
        return false;
    }
    if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr || FAILED(fence->SetEventOnCompletion(1, event)) ||
            WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
            if (event != nullptr) CloseHandle(event);
            close();
            return false;
        }
        CloseHandle(event);
    }
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) {
        close();
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    *red = bytes[(kCentre * kTarget + kCentre) * 4];
    readback->Unmap(0, nullptr);
    close();
    return true;
}

}  // namespace

TEST(geometry_shader_lowers_to_valid_dxil) {
    std::vector<std::uint8_t> dxil;
    bool translated = true;
    try {
        dxil = translate(geometry_container()).dxil;
    } catch (const std::exception& error) {
        std::printf("geometry shader did not translate: %s\n", error.what());
        translated = false;
    }
    CHECK(translated);
    if (!translated) return;
    CHECK(dxil.size() > 4);
    check_dxil(dxil, "geometry");
}

TEST(geometry_shader_emits_a_triangle_on_warp) {
    std::vector<std::uint8_t> geometry;
    try {
        geometry = translate(geometry_container()).dxil;
    } catch (const std::exception& error) {
        std::printf("geometry shader did not translate: %s\n", error.what());
        CHECK(false);
        return;
    }
    const std::vector<std::uint8_t> vertex = pass_vertex();
    const std::vector<std::uint8_t> pixel = colour_pixel();
    std::uint8_t red = 0;
    if (!warp_pixel(vertex, geometry, pixel, &red)) {
        std::printf("skipped: no WARP device\n");
        return;
    }
    CHECK(red >= kRed);
}

TEST(geometry_shader_validates_with_different_signature_widths) {
    constexpr std::uint8_t kInputTexcoordColumns = 3;
    check_dxil(translate(geometry_container(kGsWords, kInputTexcoordColumns)).dxil,
               "geometry with different signature widths");
}

TEST(geometry_shader_rejects_a_write_to_a_later_vertex_before_emit) {
    constexpr std::uint32_t kStoreFirstPositionPair = 0xd8380100;
    constexpr std::uint32_t kStoreSecondPositionPair = 0xd8380908;
    std::vector<std::uint32_t> words(std::begin(kGsWords), std::end(kGsWords));
    const auto store = std::find(words.begin(), words.end(), kStoreFirstPositionPair);
    CHECK(store != words.end());
    if (store == words.end()) return;
    *store = kStoreSecondPositionPair;
    bool rejected = false;
    try {
        (void)translate(geometry_container(words));
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a geometry output the signature lacks") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(geometry_shader_rejects_an_untracked_load_used_as_output_data) {
    constexpr std::uint32_t kStorePositionPair = 0x0007060b;
    constexpr std::uint32_t kStoreAddressAsPosition = 0x00070b0b;
    std::vector<std::uint32_t> words(std::begin(kGsWords), std::end(kGsWords));
    const auto store = std::find(words.begin(), words.end(), kStorePositionPair);
    CHECK(store != words.end());
    if (store == words.end()) return;
    *store = kStoreAddressAsPosition;
    bool rejected = false;
    try {
        (void)translate(geometry_container(words));
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("an address in an SGPR that is no root parameter") !=
                   std::string_view::npos;
    }
    CHECK(rejected);
}

TEST(geometry_shader_rejects_the_merged_wave_count_used_as_output_data) {
    constexpr std::uint32_t kMovePosition = 0x7e0c02f1;
    constexpr std::uint32_t kMovePrimitiveCountAsPosition = 0x7e0c0200;
    std::vector<std::uint32_t> words(std::begin(kGsWords), std::end(kGsWords));
    const auto move = std::find(words.begin(), words.end(), kMovePosition);
    CHECK(move != words.end());
    if (move == words.end()) return;
    *move = kMovePrimitiveCountAsPosition;
    bool rejected = false;
    try {
        (void)translate(geometry_container(words));
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("a hardware SGPR input") != std::string_view::npos;
    }
    CHECK(rejected);
}
