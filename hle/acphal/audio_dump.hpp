// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace g6shim::acphal {

inline constexpr std::uint16_t kPcmBits = 16;
inline constexpr std::uint16_t kPcmBytes = kPcmBits / 8;
inline constexpr std::size_t kWavHeaderBytes = 44;
inline constexpr std::uint32_t kWavMaximumDataBytes = UINT32_MAX - 36;
inline constexpr std::uint32_t kVoiceHeaderUpdateBlocks = 64;

using WavHeader = std::array<std::uint8_t, kWavHeaderBytes>;

WavHeader make_wav_header(std::uint64_t frames, std::uint16_t channels, std::uint32_t rate);

class WavFile {
public:
    WavFile() = default;
    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;
    WavFile(WavFile&& other) noexcept;
    WavFile& operator=(WavFile&& other) noexcept;
    ~WavFile();

    static std::optional<WavFile> create(const std::filesystem::path& path, std::uint32_t rate, std::uint16_t channels);

    void append(const void* samples, std::size_t bytes);
    bool update_header();
    void close();

    bool valid() const { return file_ != nullptr; }
    std::uint64_t frames() const { return frames_; }
    std::uint32_t rate() const { return rate_; }
    std::uint16_t channels() const { return channels_; }
    const std::filesystem::path& path() const { return path_; }

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    std::uint64_t frames_ = 0;
    std::uint32_t rate_ = 0;
    std::uint16_t channels_ = 0;
};

class VoiceDump {
public:
    VoiceDump() = default;
    VoiceDump(const VoiceDump&) = delete;
    VoiceDump& operator=(const VoiceDump&) = delete;
    VoiceDump(VoiceDump&&) noexcept = default;
    VoiceDump& operator=(VoiceDump&&) noexcept = default;

    static std::optional<VoiceDump> create(const std::filesystem::path& directory, std::uint32_t index,
                                           std::uint32_t sequence, std::uint32_t rate, std::uint16_t channels);

    void append(const std::int16_t* samples, std::size_t count) {
        wav_.append(samples, count * sizeof(std::int16_t));
        if (++blocks_since_header_ >= kVoiceHeaderUpdateBlocks) {
            wav_.update_header();
            blocks_since_header_ = 0;
        }
    }
    void close() { wav_.close(); }

    const std::filesystem::path& directory() const { return directory_; }
    std::uint64_t frames() const { return wav_.frames(); }

private:
    WavFile wav_;
    std::filesystem::path directory_;
    std::uint32_t blocks_since_header_ = 0;
};

struct VoiceReport {
    std::uint32_t index = 0;
    std::uint32_t rate = 0;
    std::uint16_t channels = 1;
    std::uint64_t produced = 0;
    std::uint64_t consumed = 0;
    std::uint32_t advance_reads = 0;
    std::uint64_t advance_blocks = 0;
    std::uint32_t buffer_switches = 0;
    std::uint32_t input_switches = 0;
    std::uint32_t underrun_reads = 0;
    std::uint64_t underrun_blocks = 0;
    std::uint32_t frames = 0;
    std::uint32_t invalid_input = 0;
    std::uint32_t unreadable = 0;
    std::uint32_t rejected = 0;
    const char* first_failure = "none";
    std::optional<std::uint64_t> first_failure_bit;
};

void append_voice_report(const std::filesystem::path& directory, const VoiceReport& report);

std::optional<std::filesystem::path> voice_dump_directory();

}  // namespace g6shim::acphal
