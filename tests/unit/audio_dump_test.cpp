// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "check.hpp"

#include "acp.hpp"
#include "audio_capture.hpp"
#include "audio_dump.hpp"
#include "shape_config.hpp"
#include "xma.hpp"
#include "xma_voices.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" HRESULT WINAPI g6shim_AcpHalAllocateShapeContexts(void* config);
extern "C" HRESULT WINAPI g6shim_AcpHalReleaseShapeContexts();
extern "C" HRESULT WINAPI g6shim_AcpHalCreate(void** hal);
extern "C" HRESULT WINAPI g6shim_ApuAlloc(void** va, UINT32* apu, UINT32 size, UINT32 alignment, UINT32 flags);
extern "C" void WINAPI g6shim_ApuFree(void* va);

using namespace g6shim::acphal;

namespace {

std::uint16_t read_u16(const std::uint8_t* data, std::size_t at) {
    return static_cast<std::uint16_t>(data[at] | static_cast<std::uint16_t>(data[at + 1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t* data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
           static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::filesystem::path test_directory(const wchar_t* name) {
    static std::atomic<unsigned> counter{0};
    const std::wstring unique = std::wstring(name) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(counter.fetch_add(1));
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / unique;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return directory;
}

struct Config {
    unsigned char bytes[kConfigBytes] = {};

    void set_count(std::size_t kind, std::uint32_t count) {
        std::memcpy(bytes + kConfigCountsOffset + 4 * kind, &count, 4);
    }

    void* array(std::size_t kind) const {
        void* p = nullptr;
        std::memcpy(&p, bytes + kConfigArraysOffset + sizeof(void*) * kind, sizeof(p));
        return p;
    }
};

struct Context {
    std::uint32_t* words;

    std::uint32_t field(std::size_t word, std::uint32_t shift, std::uint32_t bits) const {
        return (*reinterpret_cast<volatile std::uint32_t*>(words + word) >> shift) & ((1u << bits) - 1);
    }

    void set(std::size_t word, std::uint32_t value) const {
        *reinterpret_cast<volatile std::uint32_t*>(words + word) = value;
    }
};

void put_bits(std::vector<std::uint8_t>& data, std::size_t pos, std::uint32_t count, std::uint32_t value) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = pos + i;
        const std::uint8_t mask = static_cast<std::uint8_t>(1u << (7 - at % 8));
        if (((value >> (count - 1 - i)) & 1u) != 0)
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] | mask);
        else
            data[at / 8] = static_cast<std::uint8_t>(data[at / 8] & ~mask);
    }
}

std::vector<std::uint8_t> silent_stream() {
    std::vector<std::uint8_t> data(2 * xma::kPacketBytes, 0xff);
    for (std::uint32_t packet = 0; packet < 2; ++packet) {
        const std::size_t base = static_cast<std::size_t>(packet) * xma::kPacketBytes * 8;
        put_bits(data, base, 32, 3u << 26);
        for (std::uint32_t frame = 0; frame < 3; ++frame) {
            const std::size_t at = base + 32 + frame * 31;
            put_bits(data, at, 15, 31);
            put_bits(data, at + 15, 2, 0b10);
            put_bits(data, at + 17, 8 + 1 + 3 + 2, 0);
        }
    }
    return data;
}

}  // namespace

TEST(audio_dump_wav_header_describes_16_bit_pcm) {
    const WavHeader stereo = make_wav_header(1000, 2, 48000);
    CHECK(stereo.size() == kWavHeaderBytes);
    CHECK(std::memcmp(stereo.data(), "RIFF", 4) == 0);
    CHECK(read_u32(stereo.data(), 4) == 36 + 4000);
    CHECK(std::memcmp(stereo.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(read_u32(stereo.data(), 16) == 16);
    CHECK(read_u16(stereo.data(), 20) == 1);
    CHECK(read_u16(stereo.data(), 22) == 2);
    CHECK(read_u32(stereo.data(), 24) == 48000);
    CHECK(read_u32(stereo.data(), 28) == 192000);
    CHECK(read_u16(stereo.data(), 32) == 4);
    CHECK(read_u16(stereo.data(), 34) == 16);
    CHECK(std::memcmp(stereo.data() + 36, "data", 4) == 0);
    CHECK(read_u32(stereo.data(), 40) == 4000);

    const WavHeader mono = make_wav_header(10, 1, 24000);
    CHECK(read_u16(mono.data(), 22) == 1);
    CHECK(read_u32(mono.data(), 24) == 24000);
    CHECK(read_u32(mono.data(), 28) == 48000);
    CHECK(read_u16(mono.data(), 32) == 2);
    CHECK(read_u32(mono.data(), 4) == 36 + 20);
    CHECK(read_u32(mono.data(), 40) == 20);

    const WavHeader clamped = make_wav_header(UINT64_MAX, 2, 48000);
    CHECK(read_u32(clamped.data(), 40) == kWavMaximumDataBytes);
    CHECK(read_u32(clamped.data(), 4) == kWavMaximumDataBytes + 36);
}

TEST(audio_dump_wav_file_patches_its_sizes_and_keeps_the_samples) {
    const std::filesystem::path directory = test_directory(L"g6shim_audio_dump_file_test");
    const std::filesystem::path path = directory / L"tone.wav";
    const std::int16_t samples[6] = {1, -2, 3, -4, 5, -6};
    {
        auto wav = WavFile::create(path, 48000, 2);
        CHECK(wav.has_value());
        if (!wav) return;
        wav->append(samples, sizeof samples);
        CHECK(wav->frames() == 3);
        wav->close();
    }
    const std::vector<std::uint8_t> bytes = read_file(path);
    CHECK(bytes.size() == kWavHeaderBytes + sizeof samples);
    if (bytes.size() != kWavHeaderBytes + sizeof samples) return;
    CHECK(read_u32(bytes.data(), 4) == 36 + sizeof samples);
    CHECK(read_u32(bytes.data(), 40) == sizeof samples);
    CHECK(std::memcmp(bytes.data() + kWavHeaderBytes, samples, sizeof samples) == 0);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(audio_dump_voice_file_holds_exactly_the_produced_samples) {
    const std::filesystem::path directory = test_directory(L"g6shim_voice_dump_test");
    std::vector<std::int16_t> produced;
    {
        auto dump = VoiceDump::create(directory, 7, 0, 44100, 2);
        CHECK(dump.has_value());
        if (!dump) return;
        for (std::uint32_t block = 0; block < 3; ++block) {
            std::array<std::int16_t, xma::kSubframeSamples> samples{};
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = block == 2 && i >= 40 ? 0 : static_cast<std::int16_t>(block * 1000 + i);
            dump->append(samples.data(), samples.size());
            produced.insert(produced.end(), samples.begin(), samples.end());
        }
        dump->close();
        CHECK(dump->frames() == produced.size() / 2);
    }
    const std::filesystem::path path = directory / L"voice_7_0.wav";
    const std::vector<std::uint8_t> bytes = read_file(path);
    CHECK(bytes.size() == kWavHeaderBytes + produced.size() * sizeof(std::int16_t));
    if (bytes.size() != kWavHeaderBytes + produced.size() * sizeof(std::int16_t)) return;
    CHECK(read_u16(bytes.data(), 22) == 2);
    CHECK(read_u32(bytes.data(), 24) == 44100);
    CHECK(read_u32(bytes.data(), 40) == produced.size() * sizeof(std::int16_t));
    CHECK(std::memcmp(bytes.data() + kWavHeaderBytes, produced.data(), produced.size() * sizeof(std::int16_t)) == 0);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(audio_dump_records_a_decoded_voice) {
    const std::filesystem::path directory = test_directory(L"g6shim_xma_dump_test");
    CHECK(::SetEnvironmentVariableW(L"G6SHIM_XMA_DUMP", directory.c_str()) != FALSE);

    Config config;
    config.set_count(kShapeKindXma, 4);
    CHECK(g6shim_AcpHalAllocateShapeContexts(config.bytes) == S_OK);
    auto* array = static_cast<std::uint8_t*>(config.array(kShapeKindXma));
    CHECK(array != nullptr);
    if (!array) {
        ::SetEnvironmentVariableW(L"G6SHIM_XMA_DUMP", nullptr);
        return;
    }
    Context context{reinterpret_cast<std::uint32_t*>(array + kXmaContextBytes)};

    void* input = nullptr;
    std::uint32_t input_apu = 0;
    CHECK(g6shim_ApuAlloc(&input, &input_apu, 0x1000, 0x800, 1) == S_OK);
    const std::vector<std::uint8_t> stream = silent_stream();
    std::memcpy(input, stream.data(), stream.size());
    void* output = nullptr;
    std::uint32_t output_apu = 0;
    CHECK(g6shim_ApuAlloc(&output, &output_apu, 0x400, 0x100, 2) == S_OK);
    std::memset(output, 0x55, 0x400);

    context.set(0, 2 | 1u << 20 | 4u << 22);
    context.set(1, 1u << 31 | 3u << 27);
    context.set(2, 32);
    context.set(5, input_apu);
    context.set(7, output_apu);

    void* acp = nullptr;
    CHECK(g6shim_AcpHalCreate(&acp) == S_OK);
    const AcpMethods* methods = static_cast<Acp*>(acp)->methods;
    CHECK((methods->connect)(static_cast<Acp*>(acp), 0x1000, 0x1000) == S_OK);
    const auto submit = [&](std::uint32_t command, const std::uint32_t* words) {
        CHECK((methods->submit)(static_cast<Acp*>(acp), command, 0xf1, UINT32_MAX, words, 0) == S_OK);
    };
    const std::uint32_t enable[1] = {1};
    submit(5, enable);
    xma_pump();
    for (int i = 0; i < 20; ++i) {
        const std::uint32_t advance[2] = {1, 4};
        submit(18, advance);
        xma_pump();
        if (context.field(2, 31, 1) == 1 && context.field(1, 31, 1) == 1) break;
    }
    const auto produced = xma_produced(1);
    CHECK(produced.has_value());
    const std::uint32_t disable[1] = {1};
    submit(7, disable);
    (methods->release)(static_cast<Acp*>(acp));
    g6shim_ApuFree(input);
    g6shim_ApuFree(output);
    CHECK(g6shim_AcpHalReleaseShapeContexts() == S_OK);
    ::SetEnvironmentVariableW(L"G6SHIM_XMA_DUMP", nullptr);

    const std::vector<std::uint8_t> wav = read_file(directory / L"voice_1_0.wav");
    CHECK(!wav.empty());
    if (!wav.empty()) {
        const std::uint32_t data = read_u32(wav.data(), 40);
        CHECK(read_u16(wav.data(), 22) == 1);
        CHECK(read_u32(wav.data(), 24) == 48000);
        CHECK(produced.has_value() && data == *produced * sizeof(std::int16_t));
        CHECK(wav.size() == kWavHeaderBytes + data);
        bool silent = true;
        for (std::size_t i = kWavHeaderBytes; i < wav.size(); ++i)
            if (wav[i] != 0) silent = false;
        CHECK(silent);
    }

    const std::vector<std::uint8_t> report = read_file(directory / L"voices.txt");
    const std::string text(report.begin(), report.end());
    CHECK(text.find("index=1 rate=48000 channels=1") != std::string::npos);
    CHECK(text.find("rejected=0") != std::string::npos);
    CHECK(text.find("first_failure=none") != std::string::npos);
    CHECK(produced.has_value() && text.find("produced=" + std::to_string(*produced)) != std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(audio_capture_recorder_receives_its_environment) {
    ::SetEnvironmentVariableW(L"G6SHIM_AUDIO_CAPTURE", L"stale.wav");
    std::vector<wchar_t> environment =
        g6shim::acphal::recorder_environment(4242, L"C:\\capture dir\\heard.wav");
    ::SetEnvironmentVariableW(L"G6SHIM_AUDIO_CAPTURE", nullptr);
    wchar_t system[MAX_PATH] = {};
    CHECK(::GetSystemDirectoryW(system, MAX_PATH) > 0);
    const std::wstring command = L"\"" + std::wstring(system) +
                                 L"\\cmd.exe\" /d /c if \"%G6SHIM_AUDIO_CAPTURE_OF%\"==\"4242\" "
                                 L"if \"%G6SHIM_AUDIO_CAPTURE%\"==\"C:\\capture dir\\heard.wav\" exit 7";
    const HANDLE process = g6shim::acphal::start_with_environment(command, environment);
    CHECK(process != nullptr);
    if (process == nullptr) return;
    constexpr DWORD kChildDeadlineMilliseconds = 10000;
    CHECK(::WaitForSingleObject(process, kChildDeadlineMilliseconds) == WAIT_OBJECT_0);
    DWORD code = 0;
    CHECK(::GetExitCodeProcess(process, &code));
    CHECK(code == 7);
    ::CloseHandle(process);
}
