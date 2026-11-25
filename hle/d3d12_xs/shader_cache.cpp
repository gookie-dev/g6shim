// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_cache.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>
#include <g6shim/shader/emit/container.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <fstream>
#include <mutex>
#include <string>

namespace g6shim::d3d12_xs::shader_cache {

namespace {

using g6shim::shader::HwStage;
using g6shim::shader::RootType;
using g6shim::shader::emit::HostShader;
using g6shim::shader::emit::StaticSampler;

constexpr std::array<std::uint8_t, 4> kMagic{'V', 'S', 'H', '2'};
constexpr std::array<HwStage, 8> kStages{HwStage::Ls, HwStage::Hs, HwStage::Es, HwStage::Gs,
                                         HwStage::Vs, HwStage::Ps, HwStage::Ms, HwStage::Cs};
constexpr std::size_t kMaxEntryBytes = 256u << 20;
constexpr std::size_t kMaxUserData = 256;
constexpr std::size_t kMaxModuleChars = 0x8000;
constexpr wchar_t kCacheFolder[] = L"g6shim_cache";

std::atomic<std::uint64_t> g_hits{0};
std::atomic<std::uint64_t> g_misses{0};

std::once_flag g_root_once;
std::optional<std::filesystem::path> g_root;

std::optional<std::wstring> executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::nullopt;
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= kMaxModuleChars) return std::nullopt;
        path.resize(std::min(path.size() * 2, kMaxModuleChars));
    }
}

std::optional<std::filesystem::path> default_root() {
    const std::optional<std::wstring> configured = settings::get_wide("G6SHIM_CACHE_DIR");
    if (configured) return cache_root(std::filesystem::path(), std::filesystem::path(*configured));
    const std::optional<std::wstring> module = executable_path();
    if (!module) {
        G6SHIM_WARN("d3d12_xs: cannot read the executable path and no G6SHIM_CACHE_DIR; translated shaders and "
                    "host pipelines are not kept");
        return std::nullopt;
    }
    return cache_root(std::filesystem::path(*module), std::nullopt);
}

bool write_atomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxEntryBytes) return std::nullopt;
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !in.read(bytes.data(), size)) return std::nullopt;
    return bytes;
}

std::string hex_digest(const std::array<std::uint8_t, 16>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

struct Reader {
    std::span<const std::uint8_t> bytes;

    std::optional<std::span<const std::uint8_t>> take(std::size_t count) {
        if (count > bytes.size()) return std::nullopt;
        const std::span<const std::uint8_t> head = bytes.first(count);
        bytes = bytes.subspan(count);
        return head;
    }

    std::optional<std::uint32_t> word() {
        const auto head = take(4);
        if (!head) return std::nullopt;
        return static_cast<std::uint32_t>((*head)[0]) | static_cast<std::uint32_t>((*head)[1]) << 8 |
               static_cast<std::uint32_t>((*head)[2]) << 16 | static_cast<std::uint32_t>((*head)[3]) << 24;
    }

    std::optional<std::vector<std::uint8_t>> list() {
        const auto length = word();
        if (!length) return std::nullopt;
        const auto head = take(*length);
        if (!head) return std::nullopt;
        return std::vector<std::uint8_t>(head->begin(), head->end());
    }
};

std::uint32_t root_code(const RootType& root) {
    switch (root.kind) {
    case RootType::Kind::Constants: return 0;
    case RootType::Kind::Descriptor: return 1;
    case RootType::Kind::Table: return 2;
    case RootType::Kind::ImmediateSampler: return 3;
    case RootType::Kind::ResourceHeap: return 4;
    case RootType::Kind::SamplerHeap: return 5;
    case RootType::Kind::Other: return 6 + root.raw;
    }
    return 6;
}

RootType root_type(std::uint32_t code) {
    RootType root;
    switch (code) {
    case 0: root.kind = RootType::Kind::Constants; break;
    case 1: root.kind = RootType::Kind::Descriptor; break;
    case 2: root.kind = RootType::Kind::Table; break;
    case 3: root.kind = RootType::Kind::ImmediateSampler; break;
    case 4: root.kind = RootType::Kind::ResourceHeap; break;
    case 5: root.kind = RootType::Kind::SamplerHeap; break;
    default:
        root.kind = RootType::Kind::Other;
        root.raw = code - 6;
        break;
    }
    return root;
}

std::array<std::uint8_t, 16> entry_digest(const std::array<std::uint8_t, 16>& build,
                                          std::span<const std::uint8_t> container) {
    std::vector<std::uint8_t> input(build.begin(), build.end());
    input.insert(input.end(), container.begin(), container.end());
    return g6shim::shader::emit::hash(input);
}

void log_damaged(const std::filesystem::path& path) {
    G6SHIM_TRACE("d3d12_xs: unreadable shader cache entry %s; translating again", path.string().c_str());
}

}  // namespace

std::optional<std::filesystem::path> cache_root(const std::filesystem::path& executable,
                                                const std::optional<std::filesystem::path>& override_dir) {
    if (override_dir) return *override_dir;
    if (executable.empty()) return std::nullopt;
    return executable.parent_path() / kCacheFolder;
}

std::optional<Cache> Cache::open(const std::filesystem::path& dir) {
    if (dir.empty()) return std::nullopt;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        G6SHIM_WARN("d3d12_xs: no shader cache folder %s: %s", dir.string().c_str(), error.message().c_str());
        return std::nullopt;
    }
    return Cache(dir);
}

std::filesystem::path Cache::entry_path(const std::array<std::uint8_t, 16>& build,
                                        std::span<const std::uint8_t> container) const {
    return dir_ / (hex_digest(entry_digest(build, container)) + ".bin");
}

std::optional<std::filesystem::path> root_directory() {
    std::call_once(g_root_once, [] { g_root = default_root(); });
    return g_root;
}

void use_directory(const std::filesystem::path& dir) {
    std::call_once(g_root_once, [&dir] {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        g_root = dir;
    });
}

const std::array<std::uint8_t, 16>& build_fingerprint() {
    static const std::array<std::uint8_t, 16> fingerprint = [] {
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&g_root_once), &self);
        wchar_t module[MAX_PATH] = {};
        const DWORD length =
            self == nullptr ? 0 : ::GetModuleFileNameW(self, module, MAX_PATH);
        std::vector<std::uint8_t> bytes;
        if (length > 0 && length < MAX_PATH) {
            if (const auto file = read_file(std::filesystem::path(module)); file) {
                bytes.reserve(file->size());
                for (const char byte : *file) bytes.push_back(static_cast<std::uint8_t>(byte));
            }
        }
        if (bytes.empty()) {
            G6SHIM_WARN("d3d12_xs: cannot read this build; the shader cache is not reused between runs");
            const std::uint64_t now = ::GetTickCount64();
            for (std::size_t i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(now >> (8 * i)));
        }
        const std::uint32_t limit = g6shim::shader::emit::loop_limit();
        for (std::size_t i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(limit >> (8 * i)));
        return g6shim::shader::emit::hash(bytes);
    }();
    return fingerprint;
}

std::vector<std::uint8_t> encode_entry(const HostShader& shader) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    const auto word = [&out](std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto stage = std::find(kStages.begin(), kStages.end(), shader.stage);
    word(static_cast<std::uint32_t>(stage == kStages.end() ? 0 : stage - kStages.begin()));
    word(static_cast<std::uint32_t>(shader.dxil.size()));
    out.insert(out.end(), shader.dxil.begin(), shader.dxil.end());
    word(static_cast<std::uint32_t>(shader.user_data.size()));
    for (const auto& entry : shader.user_data) {
        for (const std::uint32_t value : {entry.index, static_cast<std::uint32_t>(entry.slot),
                                          root_code(entry.root), entry.dword,
                                          static_cast<std::uint32_t>(entry.source)})
            word(value);
    }
    word(static_cast<std::uint32_t>(shader.samplers.size()));
    for (const auto& sampler : shader.samplers)
        for (const std::uint32_t value : sampler) word(value);
    word(static_cast<std::uint32_t>(shader.static_samplers.size()));
    for (const StaticSampler& sampler : shader.static_samplers) {
        const auto& [u, v, w] = sampler.address;
        for (const std::uint32_t value : {sampler.filter, u, v, w, std::bit_cast<std::uint32_t>(sampler.mip_lod_bias),
                                          sampler.max_anisotropy, sampler.comparison, sampler.border,
                                          std::bit_cast<std::uint32_t>(sampler.min_lod),
                                          std::bit_cast<std::uint32_t>(sampler.max_lod)})
            word(value);
    }
    word(static_cast<std::uint32_t>(shader.root_cbvs.size()));
    out.insert(out.end(), shader.root_cbvs.begin(), shader.root_cbvs.end());
    word(static_cast<std::uint32_t>(shader.root_srvs.size()));
    out.insert(out.end(), shader.root_srvs.begin(), shader.root_srvs.end());
    return out;
}

std::optional<HostShader> decode_entry(std::span<const std::uint8_t> bytes) {
    Reader reader{bytes};
    const auto magic = reader.take(kMagic.size());
    if (!magic || !std::equal(kMagic.begin(), kMagic.end(), magic->begin())) return std::nullopt;
    const auto stage = reader.word();
    if (!stage || *stage >= kStages.size()) return std::nullopt;
    HostShader shader;
    shader.stage = kStages[*stage];
    const auto dxil = reader.list();
    if (!dxil) return std::nullopt;
    shader.dxil = *dxil;
    const auto user_data = reader.word();
    if (!user_data) return std::nullopt;
    shader.user_data.reserve(std::min<std::size_t>(*user_data, kMaxUserData));
    for (std::uint32_t i = 0; i < *user_data; ++i) {
        const auto index = reader.word();
        const auto slot = reader.word();
        const auto root = reader.word();
        const auto dword = reader.word();
        const auto source = reader.word();
        using UserData = g6shim::shader::emit::UserData;
        using Source = UserData::Source;
        if (!index || !slot || !root || !dword || !source || *slot > 0xFF ||
            *source > static_cast<std::uint32_t>(Source::ShaderBufferRecordCount))
            return std::nullopt;
        const RootType type = root_type(*root);
        if (*source != static_cast<std::uint32_t>(Source::Root) && type.kind != RootType::Kind::Table)
            return std::nullopt;
        shader.user_data.push_back(UserData{*index, static_cast<std::uint8_t>(*slot), type, *dword,
                                            static_cast<Source>(*source)});
    }
    const auto samplers = reader.word();
    if (!samplers) return std::nullopt;
    shader.samplers.reserve(std::min<std::size_t>(*samplers, kMaxUserData));
    for (std::uint32_t i = 0; i < *samplers; ++i) {
        std::array<std::uint32_t, 4> words{};
        for (std::uint32_t& word : words) {
            const auto value = reader.word();
            if (!value) return std::nullopt;
            word = *value;
        }
        shader.samplers.push_back(words);
    }
    const auto static_samplers = reader.word();
    if (!static_samplers) return std::nullopt;
    shader.static_samplers.reserve(std::min<std::size_t>(*static_samplers, kMaxUserData));
    for (std::uint32_t i = 0; i < *static_samplers; ++i) {
        std::array<std::uint32_t, 10> words{};
        for (std::uint32_t& word : words) {
            const auto value = reader.word();
            if (!value) return std::nullopt;
            word = *value;
        }
        StaticSampler sampler;
        sampler.filter = words[0];
        sampler.address = {words[1], words[2], words[3]};
        sampler.mip_lod_bias = std::bit_cast<float>(words[4]);
        sampler.max_anisotropy = words[5];
        sampler.comparison = words[6];
        sampler.border = words[7];
        sampler.min_lod = std::bit_cast<float>(words[8]);
        sampler.max_lod = std::bit_cast<float>(words[9]);
        shader.static_samplers.push_back(sampler);
    }
    const auto root_cbvs = reader.list();
    if (!root_cbvs) return std::nullopt;
    shader.root_cbvs = *root_cbvs;
    const auto root_srvs = reader.list();
    if (!root_srvs) return std::nullopt;
    shader.root_srvs = *root_srvs;
    if (!reader.bytes.empty()) return std::nullopt;
    return shader;
}

std::optional<HostShader> translate_container(std::span<const std::uint8_t> container) {
    try {
        return g6shim::shader::emit::translate(container);
    } catch (const std::exception& error) {
        G6SHIM_WARN("d3d12_xs: a shader does not translate: %s", error.what());
        return std::nullopt;
    }
}

std::optional<HostShader> cached(const Cache& cache, const std::array<std::uint8_t, 16>& build,
                                 std::span<const std::uint8_t> container, const Translator& translate) {
    bool ran = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::filesystem::path path = cache.entry_path(build, container);
        if (!std::filesystem::exists(path)) {
            const auto shader = translate(container);
            if (!shader) return std::nullopt;
            ran = true;
            const std::vector<std::uint8_t> encoded = encode_entry(*shader);
            if (!write_atomically(path, encoded)) {
                G6SHIM_WARN("d3d12_xs: a translated shader was not written to %s", path.string().c_str());
                g_misses.fetch_add(1, std::memory_order_relaxed);
                return shader;
            }
        }
        if (const auto bytes = read_file(path); bytes) {
            if (auto shader = decode_entry(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()))) {
                if (ran)
                    g_misses.fetch_add(1, std::memory_order_relaxed);
                else
                    g_hits.fetch_add(1, std::memory_order_relaxed);
                G6SHIM_TRACE("d3d12_xs: shader %s (%s)", ran ? "translated and cached" : "from the cache",
                             path.string().c_str());
                return shader;
            }
        }
        log_damaged(path);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    g_misses.fetch_add(1, std::memory_order_relaxed);
    return translate(container);
}

std::optional<HostShader> host_shader(std::span<const std::uint8_t> container) {
    static std::once_flag once;
    static std::optional<Cache> cache;
    std::call_once(once, [] {
        const auto root = root_directory();
        if (!root) return;
        cache = Cache::open(*root / L"shaders");
        if (!cache) G6SHIM_WARN("d3d12_xs: no shader cache under %s", root->string().c_str());
    });
    if (!cache) return translate_container(container);
    return cached(*cache, build_fingerprint(), container, translate_container);
}

std::pair<std::uint64_t, std::uint64_t> counts() {
    return {g_hits.load(std::memory_order_relaxed), g_misses.load(std::memory_order_relaxed)};
}

}  // namespace g6shim::d3d12_xs::shader_cache
