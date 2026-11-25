// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "d3d12_xs/host.hpp"
#include "d3d12_xs/object.hpp"
#include "d3d12_xs/pipeline.hpp"
#include "d3d12_xs/pipeline_host.hpp"
#include "d3d12_xs/pipeline_library.hpp"
#include "d3d12_xs/pipeline_make.hpp"
#include "d3d12_xs/shader_cache.hpp"

#include "g6shim/shader/emit/shader.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace g6shim::d3d12_xs;

namespace {

namespace emit = g6shim::shader::emit;

struct Environment {
    Environment() {
        ::SetEnvironmentVariableA("G6SHIM_D3D_WARP", "1");
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) == 0) return;
        const std::filesystem::path path = std::filesystem::path(temp) / L"g6shim_pipeline_test";
        ::SetEnvironmentVariableW(L"G6SHIM_CACHE_DIR", path.c_str());
    }
};

const Environment g_environment;

std::span<const std::uint8_t> span_of(std::string_view text) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::vector<std::uint8_t> compute_container() {
    constexpr std::uint32_t kKindNative = 0x0D;
    constexpr std::uint32_t kXbchVersion = 3;
    constexpr std::uint32_t kFssmVersion = 0x101;
    constexpr std::size_t kXdatOffset = 0x18;
    constexpr std::uint64_t kGraphicsSlotCount = 6;
    constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFULL;
    constexpr std::size_t kRecordSize = 0x54;
    constexpr std::size_t kRecordFlags = 0x14;
    constexpr std::size_t kRecordRegs = 0x50;
    constexpr std::size_t kCsSlot = 0x60;
    constexpr std::size_t kStageBytes = 0x38;
    constexpr std::size_t kCsThreads = 0x28;
    constexpr std::uint32_t kEndpgm = 0xBF81'0000;
    constexpr std::array<std::pair<std::size_t, std::uint64_t>, 8> slots{{
        {0x08, 0}, {0x10, 1}, {0x18, 2}, {0x20, 3}, {0x28, 4}, {0x30, 5}, {0x48, 6}, {0x60, 7},
    }};
    const auto append32 = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto append64 = [&append32](std::vector<std::uint8_t>& out, std::uint64_t value) {
        append32(out, static_cast<std::uint32_t>(value));
        append32(out, static_cast<std::uint32_t>(value >> 32));
    };
    const auto set32 = [](std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto set64 = [&set32](std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t value) {
        set32(out, at, static_cast<std::uint32_t>(value));
        set32(out, at + 4, static_cast<std::uint32_t>(value >> 32));
    };
    const auto adler = [](const std::vector<std::uint8_t>& data) {
        constexpr std::uint32_t kMod = 65521;
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (const std::uint8_t byte : data) {
            a = (a + byte) % kMod;
            b = (b + a) % kMod;
        }
        return (b << 16) | a;
    };
    const auto zlib = [&adler](const std::vector<std::uint8_t>& data) {
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
        const std::uint32_t check = adler(data);
        for (const std::uint32_t shift : {24u, 16u, 8u, 0u})
            out.push_back(static_cast<std::uint8_t>(check >> shift));
        return out;
    };
    const auto xbch = [&zlib](const std::vector<std::uint8_t>& unpacked) {
        const std::vector<std::uint8_t> stream = zlib(unpacked);
        std::vector<std::uint8_t> out{'X', 'B', 'C', 'H'};
        for (const std::uint32_t value : {kXbchVersion}) {
            out.push_back(static_cast<std::uint8_t>(value));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
            out.push_back(static_cast<std::uint8_t>(value >> 16));
            out.push_back(static_cast<std::uint8_t>(value >> 24));
        }
        for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(stream.size() >> (8 * i)));
        for (std::size_t i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(unpacked.size() >> (8 * i)));
        out.insert(out.end(), stream.begin(), stream.end());
        return out;
    };
    std::vector<std::uint8_t> native_code;
    append32(native_code, kEndpgm);
    std::vector<std::uint8_t> stage(kStageBytes, 0);
    set32(stage, kCsThreads, 1);
    set32(stage, kCsThreads + 4, 1);
    set32(stage, kCsThreads + 8, 1);
    std::vector<std::uint8_t> payload;
    append64(payload, kGraphicsSlotCount);
    payload.resize(kCsSlot + 8, 0);
    for (const auto& [at, value] : slots) {
        (void)value;
        set64(payload, at, kAbsent);
    }
    const std::uint64_t record = payload.size();
    set64(payload, kCsSlot, record);
    append64(payload, record + kRecordSize + stage.size());
    append64(payload, native_code.size());
    payload.resize(static_cast<std::size_t>(record) + kRecordFlags, 0);
    append32(payload, 0);
    payload.resize(static_cast<std::size_t>(record) + kRecordRegs, 0);
    append32(payload, 28 | 31U << 20);
    payload.resize(static_cast<std::size_t>(record) + kRecordSize, 0);
    payload.insert(payload.end(), stage.begin(), stage.end());
    payload.insert(payload.end(), native_code.begin(), native_code.end());
    std::vector<std::uint8_t> fssm{'F', 'S', 'S', 'M'};
    for (const std::uint32_t value : {kFssmVersion}) {
        fssm.push_back(static_cast<std::uint8_t>(value));
        fssm.push_back(static_cast<std::uint8_t>(value >> 8));
        fssm.push_back(static_cast<std::uint8_t>(value >> 16));
        fssm.push_back(static_cast<std::uint8_t>(value >> 24));
    }
    for (std::size_t i = 0; i < 8; ++i) fssm.push_back(static_cast<std::uint8_t>(payload.size() >> (8 * i)));
    fssm.insert(fssm.end(), payload.begin(), payload.end());
    const std::vector<std::uint8_t> packed = xbch(fssm);
    std::vector<std::uint8_t> xdat(kXdatOffset, 0);
    xdat.insert(xdat.end(), {'X', 'D', 'A', 'T'});
    const auto append = [&xdat](std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) xdat.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    append(1);
    append(0x24);
    const std::uint32_t native_at = static_cast<std::uint32_t>(xdat.size() + 12);
    append(kKindNative);
    append(native_at);
    append(static_cast<std::uint32_t>(packed.size()));
    xdat.insert(xdat.end(), packed.begin(), packed.end());
    const std::array<emit::ContainerPart, 1> parts{{emit::ContainerPart{{'D', 'X', 'I', 'L'}, xdat}}};
    return emit::build(parts);
}

emit::HostShader sample_shader() {
    emit::HostShader shader;
    shader.stage = g6shim::shader::HwStage::Ps;
    shader.dxil = {'D', 'X', 'B', 'C', ' ', 'a', 'n', 'd', ' ', 'm', 'o', 'r', 'e'};
    g6shim::shader::RootType root;
    root.kind = g6shim::shader::RootType::Kind::Table;
    shader.user_data.push_back(emit::UserData{8, 1, root, 0});
    shader.user_data.push_back(emit::UserData{9, 1, root, 8, emit::UserData::Source::ConstantBufferAddressLow});
    shader.user_data.push_back(emit::UserData{10, 1, root, 16, emit::UserData::Source::ShaderBufferAddressLow});
    shader.user_data.push_back(emit::UserData{11, 1, root, 24, emit::UserData::Source::ImageBaseAddressLow});
    shader.user_data.push_back(emit::UserData{12, 1, root, 16, emit::UserData::Source::ShaderBufferAddressHigh});
    shader.user_data.push_back(emit::UserData{13, 1, root, 16, emit::UserData::Source::ShaderBufferComponentMapping});
    shader.user_data.push_back(emit::UserData{14, 1, root, 16, emit::UserData::Source::StructuredBufferHighWord});
    shader.user_data.push_back(emit::UserData{15, 1, root, 16, emit::UserData::Source::LinearRawBufferHighWord});
    shader.user_data.push_back(emit::UserData{16, 1, root, 16, emit::UserData::Source::LinearRawBufferFormatWord});
    shader.user_data.push_back(emit::UserData{17, 1, root, 16, emit::UserData::Source::StructuredBufferFormatWord});
    shader.samplers.push_back({0x400, 0xC0'0000, 0x18F0'0000, 0});
    shader.static_samplers.push_back(emit::StaticSampler::from_words({0x400, 0xC0'0000, 0x18F0'0000, 0}));
    shader.root_cbvs = {2};
    shader.root_srvs = {0, 3};
    return shader;
}

bool same(const emit::HostShader& a, const emit::HostShader& b) {
    return a.stage == b.stage && a.dxil == b.dxil && a.user_data == b.user_data && a.samplers == b.samplers &&
           a.static_samplers == b.static_samplers && a.root_cbvs == b.root_cbvs && a.root_srvs == b.root_srvs;
}

void walk(const std::filesystem::path& dir, std::vector<std::filesystem::path>& files) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_directory())
            walk(entry.path(), files);
        else
            files.push_back(entry.path());
    }
}

std::string name_of(std::string_view kind, std::string_view root, std::string_view first, std::string_view second,
                    std::string_view state) {
    const std::array<std::span<const std::uint8_t>, 2> code{span_of(first), span_of(second)};
    return pipeline_library::name(kind, span_of(root), code, state);
}

}  // namespace

TEST(d3d12_xs_pipeline_keeps_its_shaders) {
    const std::array<std::uint8_t, 3> vertex = {1, 2, 3};
    std::array<std::uint8_t, 5> pixel{};
    pixel.fill(4);
    pipeline::GraphicsHead desc{};
    desc.stages[0] = pipeline::Bytecode{vertex.data(), vertex.size()};
    desc.stages[1] = pipeline::Bytecode{pixel.data(), pixel.size()};
    Object* first = nullptr;
    CHECK(pipeline::create_graphics(&desc, &pipeline::kIids[0], &first) == S_OK);
    CHECK(first != nullptr);
    if (first == nullptr) return;
    Pipeline* object = pipeline::as_pipeline(first);
    CHECK(object->shaders.size() == 2);
    if (object->shaders.size() == 2) {
        CHECK(object->shaders[0]->stage == "vertex");
        CHECK(object->shaders[0]->container == std::vector<std::uint8_t>(vertex.begin(), vertex.end()));
        CHECK(object->shaders[1]->stage == "pixel");
        CHECK(object->shaders[1]->container == std::vector<std::uint8_t>(pixel.begin(), pixel.end()));
    }
    Object* second = nullptr;
    CHECK(pipeline::create_graphics(&desc, &pipeline::kIids[0], &second) == S_OK);
    CHECK(second != nullptr);
    if (second != nullptr && object->shaders.size() == 2) {
        CHECK(object->shaders[1].get() == pipeline::as_pipeline(second)->shaders[1].get());
        CHECK(slot<std::uint32_t (*)(Object*)>(second, 0x10)(second) == 0);
    }
    CHECK(slot<std::uint32_t (*)(Object*)>(first, 0x10)(first) == 0);
}

TEST(d3d12_xs_pipeline_streams_yield_their_shaders) {
    const std::array<std::uint8_t, 7> mesh{9, 9, 9, 9, 9, 9, 9};
    std::array<std::uint8_t, 5> pixel{};
    pixel.fill(4);
    std::vector<std::uint8_t> stream(0x2B0, 0);
    const auto put = [&stream](std::size_t at, std::uint64_t value) {
        std::memcpy(stream.data() + at, &value, sizeof value);
    };
    const std::array<std::pair<std::size_t, std::uint64_t>, 14> subobjects{{
        {0x10, 24},
        {0x28, 2},
        {0x40, 3},
        {0x58, 4},
        {0x70, 5},
        {0x88, 8},
        {0x1D8, 9},
        {0x1E0, 10},
        {0x210, 21},
        {0x250, 17},
        {0x260, 12},
        {0x278, 14},
        {0x280, 15},
        {0x2A8, 16},
    }};
    for (const auto& [at, kind] : subobjects) put(at, kind);
    put(0x18, reinterpret_cast<std::uint64_t>(mesh.data()));
    put(0x20, mesh.size());
    put(0x30, reinterpret_cast<std::uint64_t>(pixel.data()));
    put(0x38, pixel.size());
    put(0x278, 14 | 3ull << 32);
    put(0x280, 15 | 28ull << 32);
    put(0x2A0, 1ull << 32);
    pipeline::StreamDesc desc{};
    desc.size = stream.size();
    desc.stream = stream.data();
    Object* made = nullptr;
    CHECK(pipeline::create_stream(&desc, &pipeline::kIids[0], &made) == S_OK);
    CHECK(made != nullptr);
    if (made == nullptr) return;
    Pipeline* object = pipeline::as_pipeline(made);
    CHECK(object->shaders.size() == 2);
    if (object->shaders.size() == 2) {
        CHECK(object->shaders[0]->stage == "mesh");
        CHECK(object->shaders[0]->container == std::vector<std::uint8_t>(mesh.begin(), mesh.end()));
        CHECK(object->shaders[1]->stage == "pixel");
    }
    CHECK(object->host->graphics != nullptr);
    if (object->host->graphics != nullptr) {
        const pipeline_host::GraphicsState& graphics = *object->host->graphics;
        CHECK(static_cast<std::uint32_t>(graphics.topology) == 3);
        CHECK(graphics.targets.NumRenderTargets == 1);
        CHECK(static_cast<std::uint32_t>(graphics.targets.RTFormats[0]) == 28);
    }
    CHECK(slot<std::uint32_t (*)(Object*)>(made, 0x10)(made) == 0);
}

TEST(d3d12_xs_pipeline_library_names_change_with_what_goes_into_the_pipeline) {
    const std::string first = name_of("graphics", "root", "vs", "ps", "state");
    CHECK(first == name_of("graphics", "root", "vs", "ps", "state"));
    CHECK(first != name_of("graphics", "root", "vs", "ps2", "state"));
    CHECK(first != name_of("graphics", "root", "vs", "ps", "state2"));
    CHECK(first != name_of("graphics", "root2", "vs", "ps", "state"));
    CHECK(first != name_of("mesh", "root", "vs", "ps", "state"));
    CHECK(first != name_of("graphics", "root", "vsp", "s", "state"));
}

TEST(d3d12_xs_pipeline_library_starts_empty_from_a_rejected_blob) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    Microsoft::WRL::ComPtr<ID3D12Device1> device;
    if (FAILED(gpu->device()->QueryInterface(IID_PPV_ARGS(&device)))) return;
    std::vector<std::uint8_t> blob;
    const std::string text = "not a pipeline library";
    for (int i = 0; i < 16; ++i) blob.insert(blob.end(), text.begin(), text.end());
    const std::optional<pipeline_library::Library> library = pipeline_library::open(device.Get(), std::move(blob));
    CHECK(library.has_value());
    if (library) {
        CHECK(library->serialized_size() > 0);
        CHECK(library->blob.empty());
    }
}

TEST(d3d12_xs_pipeline_shader_entries_round_trip_and_reject_damage) {
    const emit::HostShader original = sample_shader();
    const std::vector<std::uint8_t> encoded = shader_cache::encode_entry(original);
    const std::optional<emit::HostShader> decoded = shader_cache::decode_entry(encoded);
    CHECK(decoded.has_value());
    if (decoded) CHECK(same(original, *decoded));
    std::vector<std::uint8_t> damaged(encoded.begin(), encoded.end() - 1);
    CHECK(!shader_cache::decode_entry(damaged).has_value());
    damaged = encoded;
    damaged.push_back(0);
    CHECK(!shader_cache::decode_entry(damaged).has_value());
    const std::array<std::uint8_t, 4> wrong{'V', 'S', 'H', '1'};
    CHECK(!shader_cache::decode_entry(wrong).has_value());
    emit::HostShader bad_source = original;
    bad_source.user_data.back().source = static_cast<emit::UserData::Source>(UINT32_MAX);
    CHECK(!shader_cache::decode_entry(shader_cache::encode_entry(bad_source)).has_value());
}

TEST(d3d12_xs_shader_cache_root_prefers_the_override_folder) {
    const std::filesystem::path executable(L"C:\\work\\Game\\Game.exe");
    const std::filesystem::path configured(L"D:\\caches\\g6shim");
    const std::optional<std::filesystem::path> beside = shader_cache::cache_root(executable, configured);
    CHECK(beside.has_value());
    if (beside) CHECK(*beside == configured);
    const std::optional<std::filesystem::path> without_executable =
        shader_cache::cache_root(std::filesystem::path(), configured);
    CHECK(without_executable.has_value());
    if (without_executable) CHECK(*without_executable == configured);
}

TEST(d3d12_xs_shader_cache_root_sits_beside_the_executable) {
    const std::filesystem::path executable(L"C:\\work\\Game\\Game.exe");
    const std::optional<std::filesystem::path> root = shader_cache::cache_root(executable, std::nullopt);
    CHECK(root.has_value());
    if (root) CHECK(*root == std::filesystem::path(L"C:\\work\\Game") / L"g6shim_cache");
}

TEST(d3d12_xs_shader_cache_root_needs_an_executable_path) {
    CHECK(!shader_cache::cache_root(std::filesystem::path(), std::nullopt).has_value());
}

TEST(d3d12_xs_pipeline_translations_come_back_from_the_cache) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "g6shim_shader_cache_test";
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    const std::optional<shader_cache::Cache> cache = shader_cache::Cache::open(dir);
    CHECK(cache.has_value());
    if (!cache) return;
    std::array<std::uint8_t, 16> build{};
    build.fill(7);
    const std::string container = "a console container";
    std::atomic<int> runs{0};
    const shader_cache::Translator translate = [&runs](std::span<const std::uint8_t>) {
        runs.fetch_add(1);
        return std::optional<emit::HostShader>(sample_shader());
    };
    const auto first = shader_cache::cached(*cache, build, span_of(container), translate);
    CHECK(first.has_value());
    if (first) CHECK(same(*first, sample_shader()));
    const auto second = shader_cache::cached(*cache, build, span_of(container), translate);
    CHECK(second.has_value());
    if (second) CHECK(same(*second, sample_shader()));
    CHECK(runs.load() == 1);
    std::array<std::uint8_t, 16> other_build{};
    other_build.fill(8);
    CHECK(shader_cache::cached(*cache, other_build, span_of(container), translate).has_value());
    CHECK(runs.load() == 2);
    std::vector<std::filesystem::path> files;
    walk(dir, files);
    CHECK(!files.empty());
    const std::string broken = "VSH1 broken";
    for (const auto& file : files) std::filesystem::resize_file(file, 0);
    for (const auto& file : files) {
        std::vector<std::uint8_t> bytes(broken.begin(), broken.end());
        HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        CHECK(handle != INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            ::CloseHandle(handle);
        }
    }
    CHECK(shader_cache::cached(*cache, build, span_of(container), translate).has_value());
    CHECK(runs.load() == 3);
    CHECK(shader_cache::cached(*cache, build, span_of(container), translate).has_value());
    CHECK(runs.load() == 3);
    const shader_cache::Translator fail = [](std::span<const std::uint8_t>) {
        return std::optional<emit::HostShader>();
    };
    const std::string other = "other";
    CHECK(!shader_cache::cached(*cache, build, span_of(other), fail).has_value());
    std::filesystem::remove_all(dir, ignored);
}

TEST(d3d12_xs_pipeline_first_use_is_made_by_a_worker) {
    const auto pending = std::make_shared<make::Pending>(std::vector<std::shared_ptr<Shader>>{}, nullptr);
    CHECK(make::ready(pending) == nullptr);
    CHECK(pending->queued());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pending->done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(pending->done());
    CHECK(pending->result() == nullptr);
}

TEST(d3d12_xs_pipeline_compiles_a_synthetic_compute_pipeline) {
    host::Host* gpu = host::host();
    if (gpu == nullptr) return;
    const std::vector<std::uint8_t> container = compute_container();
    pipeline::ComputeHead desc{};
    desc.stage = pipeline::Bytecode{container.data(), container.size()};
    Object* made = nullptr;
    CHECK(pipeline::create_compute(&desc, &pipeline::kIids[0], &made) == S_OK);
    CHECK(made != nullptr);
    if (made == nullptr) return;
    Pipeline* object = pipeline::as_pipeline(made);
    CHECK(object->shaders.size() == 1);
    if (object->shaders.size() == 1) CHECK(object->shaders[0]->stage == "compute");
    pipeline_host::HostPipeline* pipeline = nullptr;
    for (int attempt = 0; attempt < 2000 && pipeline == nullptr; ++attempt) {
        pipeline = pipeline::host_pipeline(made);
        if (pipeline != nullptr || object->host->done()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(pipeline != nullptr);
    if (pipeline != nullptr) {
        CHECK(pipeline->compute);
        CHECK(!pipeline->mesh);
        CHECK(pipeline->pso != nullptr);
        CHECK(pipeline->root != nullptr);
        CHECK(pipeline->stages.size() == 1);
        CHECK(pipeline->name.find("compute") != std::string::npos);
    }
    CHECK(gpu->device()->GetDeviceRemovedReason() == S_OK);
    CHECK(slot<std::uint32_t (*)(Object*)>(made, 0x10)(made) == 0);
}
