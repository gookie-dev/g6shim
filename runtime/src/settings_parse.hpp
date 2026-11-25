// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <g6shim/settings.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g6shim::settings {

inline constexpr std::size_t kMaxFileBytes = 1u << 20;
inline constexpr std::size_t kMaxLineBytes = 65536;
inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxKeys = 256;
inline constexpr std::size_t kMaxWarnings = 64;
inline constexpr char kFileName[] = "g6shim.ini";
inline constexpr wchar_t kFileNameWide[] = L"g6shim.ini";

struct Binding {
    const char* variable;
    const char* section;
    const char* key;
};

inline constexpr Binding kBindings[] = {
    {"G6SHIM_LOG", "log", "file"},
    {"G6SHIM_LOG_STDERR", "log", "stderr"},
    {"G6SHIM_TRACE", "log", "trace"},
    {"G6SHIM_CONSOLE", "console", "model"},
    {"G6SHIM_WINDOW", "window", "size"},
    {"G6SHIM_FULLSCREEN", "window", "fullscreen"},
    {"G6SHIM_CAPTURE_FRAMES", "graphics", "capture_frames"},
    {"G6SHIM_D3D_DEBUG", "graphics", "d3d_debug"},
    {"G6SHIM_D3D_DRED", "graphics", "d3d_dred"},
    {"G6SHIM_D3D_WARP", "graphics", "d3d_warp"},
    {"G6SHIM_CACHE_DIR", "graphics", "cache_dir"},
    {"G6SHIM_AUDIO_CAPTURE", "audio", "capture"},
    {"G6SHIM_XMA_DUMP", "audio", "xma_dump"},
    {"G6SHIM_INPUT_AUTOPRESS", "input", "autopress"},
    {"G6SHIM_BCPACK_TABLES", "storage", "bcpack_tables"},
};

struct Entry {
    std::string section;
    std::string key;
    std::string value;
};

struct Ini {
    std::vector<Entry> entries;
    std::vector<std::string> warnings;
};

struct FileBytes {
    std::vector<std::uint8_t> bytes;
    bool missing = false;
    bool too_large = false;
    bool unreadable = false;
};

inline const Binding* binding_for(std::string_view variable) {
    for (const Binding& binding : kBindings)
        if (variable == binding.variable) return &binding;
    return nullptr;
}

inline bool known_section(std::string_view section) {
    for (const Binding& binding : kBindings)
        if (section == binding.section) return true;
    return false;
}

inline bool known_key(std::string_view section, std::string_view key) {
    for (const Binding& binding : kBindings)
        if (section == binding.section && key == binding.key) return true;
    return false;
}

inline const std::string* find_value(const Ini& ini, std::string_view section, std::string_view key) {
    const std::string* found = nullptr;
    for (const Entry& entry : ini.entries)
        if (entry.section == section && entry.key == key) found = &entry.value;
    return found;
}

inline std::optional<std::string> choose(const std::optional<std::string>& environment,
                                         const std::optional<std::string>& file) {
    if (environment && !environment->empty()) return environment;
    if (file && !file->empty()) return file;
    return std::nullopt;
}

inline std::optional<std::wstring> settings_file_next_to(std::wstring_view module) {
    const std::size_t slash = module.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return std::nullopt;
    std::wstring path(module.substr(0, slash + 1));
    path += kFileNameWide;
    return path;
}

inline FileBytes read_settings_file(const std::filesystem::path& path) {
    FileBytes out;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        out.missing = true;
        return out;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        out.unreadable = true;
        return out;
    }
    if (size > kMaxFileBytes) {
        out.too_large = true;
        return out;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.unreadable = true;
        return out;
    }
    out.bytes.resize(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(size))) {
        out.bytes.clear();
        out.unreadable = true;
    }
    return out;
}

namespace parse_detail {

inline bool ascii_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

inline bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

inline char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string lower_copy(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = lower_ascii(c);
    return out;
}

inline bool setting_name(std::string_view text) {
    if (text.empty() || text.size() > kMaxNameBytes) return false;
    if (!ascii_letter(text.front()) && text.front() != '_') return false;
    for (char c : text)
        if (!ascii_letter(c) && !ascii_digit(c) && c != '_' && c != '-') return false;
    return true;
}

inline std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

inline bool contains_nul(std::span<const std::uint8_t> bytes) {
    return std::find(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(0)) != bytes.end();
}

inline bool utf8_valid(std::span<const std::uint8_t> bytes) {
    constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
    constexpr std::uint32_t kSurrogateFirst = 0xD800;
    constexpr std::uint32_t kSurrogateLast = 0xDFFF;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::uint8_t lead = bytes[i];
        if (lead <= 0x7F) {
            ++i;
            continue;
        }
        std::size_t need = 0;
        std::uint32_t minimum = 0;
        std::uint32_t code = 0;
        if ((lead & 0xE0) == 0xC0) {
            need = 2;
            minimum = 0x80;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            need = 3;
            minimum = 0x800;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            need = 4;
            minimum = 0x10000;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (i + need > bytes.size()) return false;
        for (std::size_t n = 1; n < need; ++n) {
            const std::uint8_t next = bytes[i + n];
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > kMaxCodePoint || (code >= kSurrogateFirst && code <= kSurrogateLast)) return false;
        i += need;
    }
    return true;
}

inline void warn(Ini& ini, std::string message) {
    if (ini.warnings.size() > kMaxWarnings) return;
    if (ini.warnings.size() == kMaxWarnings) {
        ini.warnings.emplace_back(std::string("settings: further problems in ") + kFileName + " were omitted");
        return;
    }
    for (const std::string& existing : ini.warnings)
        if (existing == message) return;
    ini.warnings.push_back(std::move(message));
}

inline std::string line_label(std::size_t line) {
    return std::string("settings: ") + kFileName + ":line " + std::to_string(line) + ": ";
}

inline void store(Ini& ini, std::string section, std::string key, std::string value) {
    for (Entry& entry : ini.entries) {
        if (entry.section == section && entry.key == key) {
            entry.value = std::move(value);
            return;
        }
    }
    if (ini.entries.size() >= kMaxKeys) {
        warn(ini, std::string("settings: ") + kFileName + " has more than " + std::to_string(kMaxKeys) + " keys");
        return;
    }
    ini.entries.push_back(Entry{std::move(section), std::move(key), std::move(value)});
}

inline std::string unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return std::string(value.substr(1, value.size() - 2));
    return std::string(value);
}

inline void parse_section(Ini& ini, std::string_view text, std::size_t line, std::string& section, bool& known) {
    known = false;
    section.clear();
    if (text.back() != ']') {
        warn(ini, line_label(line) + "malformed section");
        return;
    }
    const std::string name = lower_copy(trim(text.substr(1, text.size() - 2)));
    if (!setting_name(name)) {
        warn(ini, line_label(line) + "malformed section");
        return;
    }
    section = name;
    known = known_section(section);
    if (!known) warn(ini, std::string("settings: ") + kFileName + ": unknown section \"" + section + "\"");
}

inline void parse_assignment(Ini& ini, std::string_view text, std::size_t line, const std::string& section, bool known) {
    const std::size_t eq = text.find('=');
    if (eq == std::string_view::npos || section.empty()) {
        warn(ini, line_label(line) + "malformed line");
        return;
    }
    const std::string key = lower_copy(trim(text.substr(0, eq)));
    if (!setting_name(key)) {
        warn(ini, line_label(line) + "malformed line");
        return;
    }
    if (!known) return;
    if (!known_key(section, key)) {
        warn(ini, std::string("settings: ") + kFileName + ": unknown key \"" + key + "\" in [" + section + "]");
        return;
    }
    std::string value = unquote(trim(text.substr(eq + 1)));
    if (value.size() > kMaxValueBytes) {
        warn(ini, line_label(line) + "value longer than " + std::to_string(kMaxValueBytes) + " bytes");
        return;
    }
    store(ini, section, key, std::move(value));
}

}  // namespace parse_detail

inline Ini parse_ini(std::span<const std::uint8_t> bytes) {
    Ini ini;
    if (bytes.size() > kMaxFileBytes) {
        parse_detail::warn(ini, std::string("settings: ") + kFileName + " is larger than " +
                                    std::to_string(kMaxFileBytes) + " bytes; ignoring it");
        return ini;
    }
    std::size_t i = 0;
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        parse_detail::warn(ini, std::string("settings: ") + kFileName + " is not UTF-8; ignoring it");
        return ini;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) i = 3;
    std::string section;
    bool known = false;
    std::size_t line = 1;
    while (i < bytes.size()) {
        const std::size_t begin = i;
        const std::size_t limit = std::min(bytes.size(), begin + kMaxLineBytes);
        while (i < limit && bytes[i] != '\n') ++i;
        const bool too_long = i == limit && i < bytes.size() && bytes[i] != '\n';
        std::span<const std::uint8_t> raw = bytes.subspan(begin, i - begin);
        if (too_long) {
            parse_detail::warn(ini, parse_detail::line_label(line) + "longer than " + std::to_string(kMaxLineBytes) +
                                        " bytes");
            while (i < bytes.size() && bytes[i] != '\n') ++i;
        }
        if (i < bytes.size() && bytes[i] == '\n') ++i;
        if (!too_long) {
            if (parse_detail::contains_nul(raw) || !parse_detail::utf8_valid(raw)) {
                parse_detail::warn(ini, parse_detail::line_label(line) + (parse_detail::contains_nul(raw)
                                                                               ? "malformed line"
                                                                               : "not UTF-8"));
            } else {
                const std::string_view text = parse_detail::trim(
                    std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
                if (!text.empty() && text.front() != '#' && text.front() != ';') {
                    if (text.front() == '[')
                        parse_detail::parse_section(ini, text, line, section, known);
                    else
                        parse_detail::parse_assignment(ini, text, line, section, known);
                }
            }
        }
        if (line < SIZE_MAX) ++line;
    }
    return ini;
}

G6SHIM_RUNTIME_API void install_bytes_for_test(const std::uint8_t* bytes, std::size_t size);
G6SHIM_RUNTIME_API void reload_file_for_test();
G6SHIM_RUNTIME_API std::size_t copy_configured_file(wchar_t* buffer, std::size_t buffer_size);

}  // namespace g6shim::settings
