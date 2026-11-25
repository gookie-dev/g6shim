// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"
#include "settings_parse.hpp"

#include <g6shim/settings.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using g6shim::settings::kBindings;
using g6shim::settings::kFileName;
using g6shim::settings::kMaxFileBytes;
using g6shim::settings::kMaxLineBytes;
using g6shim::settings::FileBytes;
using g6shim::settings::Ini;
using g6shim::settings::choose;
using g6shim::settings::copy_configured_file;
using g6shim::settings::find_value;
using g6shim::settings::install_bytes_for_test;
using g6shim::settings::parse_ini;
using g6shim::settings::read_settings_file;
using g6shim::settings::reload_file_for_test;
using g6shim::settings::settings_file_next_to;

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

bool warning_has(const Ini& ini, std::string_view fragment) {
    for (const std::string& warning : ini.warnings)
        if (warning.find(fragment) != std::string::npos) return true;
    return false;
}

struct FileReload {
    FileReload() { reload_file_for_test(); }
    ~FileReload() { reload_file_for_test(); }
};

struct NarrowEnv {
    std::string name;
    std::optional<std::string> saved;

    explicit NarrowEnv(const char* variable) : name(variable) {
        char buffer[32768];
        const DWORD length = GetEnvironmentVariableA(variable, buffer, sizeof buffer);
        if (length > 0 && length < sizeof buffer) saved = std::string(buffer, length);
        SetEnvironmentVariableA(variable, nullptr);
    }

    ~NarrowEnv() { SetEnvironmentVariableA(name.c_str(), saved ? saved->c_str() : nullptr); }
};

struct WideEnv {
    std::wstring name;
    std::optional<std::wstring> saved;

    explicit WideEnv(const wchar_t* variable) : name(variable) {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(variable, buffer, 32768);
        if (length > 0 && length < 32768) saved = std::wstring(buffer, length);
        SetEnvironmentVariableW(variable, nullptr);
    }

    ~WideEnv() { SetEnvironmentVariableW(name.c_str(), saved ? saved->c_str() : nullptr); }
};

}  // namespace

TEST(settings_file_sits_next_to_the_executable) {
    CHECK(settings_file_next_to(L"D:\\game\\title.exe") == std::optional<std::wstring>(L"D:\\game\\g6shim.ini"));
    CHECK(settings_file_next_to(L"D:/game/title.exe") == std::optional<std::wstring>(L"D:/game/g6shim.ini"));
    CHECK(!settings_file_next_to(L"title.exe"));
    wchar_t path[0x8000];
    const std::size_t length = copy_configured_file(path, 0x8000);
    CHECK(length > 0 && length < 0x8000);
    path[length] = L'\0';
    CHECK(std::wstring_view(path, length).ends_with(g6shim::settings::kFileNameWide));
}

TEST(settings_choose_environment_over_file) {
    CHECK(choose(std::nullopt, std::nullopt) == std::nullopt);
    CHECK(choose(std::nullopt, std::optional<std::string>("800x600")) == std::optional<std::string>("800x600"));
    CHECK(choose(std::optional<std::string>(""), std::optional<std::string>("800x600")) ==
          std::optional<std::string>("800x600"));
    CHECK(choose(std::optional<std::string>("1"), std::nullopt) == std::optional<std::string>("1"));
    CHECK(choose(std::optional<std::string>("0"), std::optional<std::string>("1")) == std::optional<std::string>("0"));
}

TEST(settings_reader_prefers_environment_over_installed_file) {
    FileReload reload;
    NarrowEnv dred("G6SHIM_D3D_DRED");
    const char* file_off = "[graphics]\nd3d_dred = 0\n";
    install_bytes_for_test(reinterpret_cast<const std::uint8_t*>(file_off), std::strlen(file_off));
    CHECK(!g6shim::settings::enabled("G6SHIM_D3D_DRED"));
    CHECK(g6shim::settings::get("G6SHIM_D3D_DRED") == std::optional<std::string>("0"));
    SetEnvironmentVariableA("G6SHIM_D3D_DRED", "1");
    CHECK(g6shim::settings::enabled("G6SHIM_D3D_DRED"));
    CHECK(g6shim::settings::get("G6SHIM_D3D_DRED") == std::optional<std::string>("1"));
    SetEnvironmentVariableA("G6SHIM_D3D_DRED", nullptr);
    const char* file_on = "[graphics]\nd3d_dred = 1\n";
    install_bytes_for_test(reinterpret_cast<const std::uint8_t*>(file_on), std::strlen(file_on));
    CHECK(g6shim::settings::enabled("G6SHIM_D3D_DRED"));
    install_bytes_for_test(nullptr, 0);
    CHECK(!g6shim::settings::enabled("G6SHIM_D3D_DRED"));
    SetEnvironmentVariableA("G6SHIM_D3D_DRED", "1");
    CHECK(g6shim::settings::enabled("G6SHIM_D3D_DRED"));
}

TEST(settings_wide_value_comes_from_file_or_environment) {
    FileReload reload;
    WideEnv dump(L"G6SHIM_XMA_DUMP");
    const char file[] = "[audio]\nxma_dump = C:/caf\xc3\xa9\n";
    install_bytes_for_test(reinterpret_cast<const std::uint8_t*>(file), sizeof file - 1);
    CHECK(g6shim::settings::get_wide("G6SHIM_XMA_DUMP") == std::optional<std::wstring>(L"C:/caf\u00e9"));
    SetEnvironmentVariableW(L"G6SHIM_XMA_DUMP", L"D:\\\u00e9");
    CHECK(g6shim::settings::get_wide("G6SHIM_XMA_DUMP") == std::optional<std::wstring>(L"D:\\\u00e9"));
}

TEST(settings_ini_accepts_known_keys_and_warns_on_unknown) {
    std::string text;
    for (const auto& binding : kBindings) {
        text += "[";
        text += binding.section;
        text += "]\n";
        text += binding.key;
        text += " = value-";
        text += binding.variable;
        text += "\n";
    }
    text += "[window]\nsize = \"1920x1080\"\nunknown_key = 1\n";
    text += "[nope]\nsize = 1\n";
    text += "# comment\n; another\n\n";
    const Ini ini = parse_ini(bytes_of(text));
    CHECK(warning_has(ini, "unknown key \"unknown_key\""));
    CHECK(warning_has(ini, "unknown section \"nope\""));
    CHECK(!warning_has(ini, "malformed"));
    const std::string* size = find_value(ini, "window", "size");
    CHECK(size && *size == "1920x1080");
    for (const auto& binding : kBindings) {
        if (std::string_view(binding.section) == "window" && std::string_view(binding.key) == "size") continue;
        const std::string* value = find_value(ini, binding.section, binding.key);
        CHECK(value && *value == std::string("value-") + binding.variable);
    }
}

TEST(settings_ini_reports_malformed_lines_and_keeps_going) {
    const Ini ini = parse_ini(bytes_of("no-section = 1\n[window\n[]\n[window]\n= missing\n1bad = 1\nsize = 800x600\n"));
    CHECK(warning_has(ini, "malformed"));
    const std::string* size = find_value(ini, "window", "size");
    CHECK(size && *size == "800x600");
    const Ini utf16 = parse_ini(std::vector<std::uint8_t>{0xFF, 0xFE, 'a', 0});
    CHECK(warning_has(utf16, "not UTF-8"));
    CHECK(utf16.entries.empty());
    std::vector<std::uint8_t> bad{static_cast<std::uint8_t>('['), 0xC3, 0x28, static_cast<std::uint8_t>(']')};
    const Ini invalid = parse_ini(bad);
    CHECK(warning_has(invalid, "not UTF-8"));
    CHECK(invalid.entries.empty());
}

TEST(settings_missing_file_is_empty_and_oversized_file_is_ignored) {
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / L"g6shim_settings_missing.ini";
    std::filesystem::remove(missing);
    const FileBytes absent = read_settings_file(missing);
    CHECK(absent.missing);
    CHECK(!absent.too_large);
    CHECK(!absent.unreadable);
    CHECK(absent.bytes.empty());
    const Ini empty = parse_ini(absent.bytes);
    CHECK(empty.entries.empty());
    CHECK(empty.warnings.empty());

    const std::filesystem::path huge = std::filesystem::temp_directory_path() / L"g6shim_settings_huge.ini";
    {
        std::ofstream out(huge, std::ios::binary | std::ios::trunc);
        std::vector<char> chunk(64 * 1024, 'a');
        for (std::size_t written = 0; written < kMaxFileBytes + 1; written += chunk.size()) out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    const FileBytes oversized = read_settings_file(huge);
    CHECK(oversized.too_large);
    CHECK(oversized.bytes.empty());
    std::filesystem::remove(huge);
    std::vector<std::uint8_t> bomb(kMaxFileBytes + 1, 'x');
    const Ini ignored = parse_ini(bomb);
    CHECK(ignored.entries.empty());
    CHECK(warning_has(ignored, "larger than"));
}

TEST(settings_malformed_bytes_do_not_crash) {
    constexpr int kRounds = 200;
    constexpr std::size_t kFuzzBytes = 2048;
    std::mt19937 rng(0x6E15u);
    std::vector<std::uint8_t> data(kFuzzBytes);
    for (int round = 0; round < kRounds; ++round) {
        const std::size_t size = rng() % (kFuzzBytes + 1);
        for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(rng());
        const Ini ini = parse_ini(std::span<const std::uint8_t>(data.data(), size));
        CHECK(ini.warnings.size() <= g6shim::settings::kMaxWarnings + 1);
        CHECK(ini.entries.size() <= g6shim::settings::kMaxKeys);
    }
    std::vector<std::uint8_t> line(kMaxLineBytes + 64, 'A');
    line.push_back('\n');
    line.insert(line.end(), {'[', 'w', 'i', 'n', 'd', 'o', 'w', ']', '\n', 's', 'i', 'z', 'e', ' ', '=', ' ', '1', 'x', '1', '\n'});
    const Ini after = parse_ini(line);
    CHECK(warning_has(after, "longer than"));
    const std::string* kept = find_value(after, "window", "size");
    CHECK(kept && *kept == "1x1");
}
