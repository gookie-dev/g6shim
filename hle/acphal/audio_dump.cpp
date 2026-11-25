// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_dump.hpp"

#include <g6shim/settings.hpp>

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace g6shim::acphal {

namespace {

constexpr char kRiffTag[] = "RIFF";
constexpr char kWaveTag[] = "WAVEfmt ";
constexpr char kDataTag[] = "data";
constexpr std::uint32_t kRiffHeaderBytes = 36;
constexpr std::uint32_t kFormatBytes = 16;
constexpr std::uint16_t kPcmFormat = 1;
constexpr std::size_t kRiffTagOffset = 0;
constexpr std::size_t kRiffSizeOffset = 4;
constexpr std::size_t kWaveTagOffset = 8;
constexpr std::size_t kFormatSizeOffset = 16;
constexpr std::size_t kFormatOffset = 20;
constexpr std::size_t kChannelsOffset = 22;
constexpr std::size_t kRateOffset = 24;
constexpr std::size_t kByteRateOffset = 28;
constexpr std::size_t kBlockAlignOffset = 32;
constexpr std::size_t kBitsOffset = 34;
constexpr std::size_t kDataTagOffset = 36;
constexpr std::size_t kDataSizeOffset = 40;
constexpr wchar_t kReportName[] = L"voices.txt";

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xff);
    out[1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xff);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

bool write_header(std::FILE* file, const WavHeader& header) {
    return std::fwrite(header.data(), 1, header.size(), file) == header.size();
}

std::wstring voice_file_name(std::uint32_t index, std::uint32_t sequence) {
    std::wstring name = L"voice_";
    name += std::to_wstring(index);
    name += L"_";
    name += std::to_wstring(sequence);
    name += L".wav";
    return name;
}

}  // namespace

WavHeader make_wav_header(std::uint64_t frames, std::uint16_t channels, std::uint32_t rate) {
    const std::uint32_t block_align = static_cast<std::uint32_t>(channels) * kPcmBytes;
    const std::uint64_t available = block_align == 0 ? 0 : frames * block_align;
    const std::uint32_t data = available > kWavMaximumDataBytes ? kWavMaximumDataBytes
                                                                 : static_cast<std::uint32_t>(available);
    WavHeader header{};
    std::memcpy(header.data() + kRiffTagOffset, kRiffTag, 4);
    put_u32(header.data() + kRiffSizeOffset, kRiffHeaderBytes + data);
    std::memcpy(header.data() + kWaveTagOffset, kWaveTag, 8);
    put_u32(header.data() + kFormatSizeOffset, kFormatBytes);
    put_u16(header.data() + kFormatOffset, kPcmFormat);
    put_u16(header.data() + kChannelsOffset, channels);
    put_u32(header.data() + kRateOffset, rate);
    put_u32(header.data() + kByteRateOffset, rate * block_align);
    put_u16(header.data() + kBlockAlignOffset, static_cast<std::uint16_t>(block_align));
    put_u16(header.data() + kBitsOffset, kPcmBits);
    std::memcpy(header.data() + kDataTagOffset, kDataTag, 4);
    put_u32(header.data() + kDataSizeOffset, data);
    return header;
}

WavFile::WavFile(WavFile&& other) noexcept
    : file_(other.file_), path_(std::move(other.path_)), frames_(other.frames_), rate_(other.rate_),
      channels_(other.channels_) {
    other.file_ = nullptr;
}

WavFile& WavFile::operator=(WavFile&& other) noexcept {
    if (this == &other) return *this;
    close();
    file_ = other.file_;
    path_ = std::move(other.path_);
    frames_ = other.frames_;
    rate_ = other.rate_;
    channels_ = other.channels_;
    other.file_ = nullptr;
    return *this;
}

WavFile::~WavFile() { close(); }

std::optional<WavFile> WavFile::create(const std::filesystem::path& path, std::uint32_t rate, std::uint16_t channels) {
    if (rate == 0 || channels == 0) return std::nullopt;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) return std::nullopt;
    }
    std::FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) return std::nullopt;
    if (!write_header(file, make_wav_header(0, channels, rate))) {
        std::fclose(file);
        return std::nullopt;
    }
    WavFile result;
    result.file_ = file;
    result.path_ = path;
    result.rate_ = rate;
    result.channels_ = channels;
    return result;
}

void WavFile::append(const void* samples, std::size_t bytes) {
    if (!file_ || !samples || bytes == 0) return;
    const std::size_t block = static_cast<std::size_t>(channels_) * kPcmBytes;
    const std::size_t whole = bytes - bytes % block;
    if (whole == 0) return;
    if (std::fwrite(samples, 1, whole, file_) != whole) return;
    frames_ += whole / block;
}

bool WavFile::update_header() {
    if (!file_) return false;
    if (_fseeki64(file_, 0, SEEK_SET) != 0) return false;
    if (!write_header(file_, make_wav_header(frames_, channels_, rate_))) return false;
    if (std::fflush(file_) != 0) return false;
    return _fseeki64(file_, 0, SEEK_END) == 0;
}

void WavFile::close() {
    if (!file_) return;
    update_header();
    std::fclose(file_);
    file_ = nullptr;
}

std::optional<VoiceDump> VoiceDump::create(const std::filesystem::path& directory, std::uint32_t index,
                                           std::uint32_t sequence, std::uint32_t rate, std::uint16_t channels) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return std::nullopt;
    auto wav = WavFile::create(directory / voice_file_name(index, sequence), rate, channels);
    if (!wav) return std::nullopt;
    VoiceDump dump;
    dump.wav_ = std::move(*wav);
    dump.directory_ = directory;
    return dump;
}

std::optional<std::filesystem::path> voice_dump_directory() {
    const std::optional<std::wstring> path = settings::get_wide("G6SHIM_XMA_DUMP");
    if (!path || path->empty()) return std::nullopt;
    return std::filesystem::path(*path);
}

void append_voice_report(const std::filesystem::path& directory, const VoiceReport& report) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    std::FILE* file = _wfopen((directory / kReportName).c_str(), L"ab");
    if (!file) return;
    std::ostringstream line;
    line << "index=" << report.index << " rate=" << report.rate << " channels=" << report.channels
         << " produced=" << report.produced << " consumed=" << report.consumed
         << " advance_reads=" << report.advance_reads << " advance_blocks=" << report.advance_blocks
         << " buffer_switches=" << report.buffer_switches << " input_switches=" << report.input_switches
         << " underrun_reads=" << report.underrun_reads << " underrun_blocks=" << report.underrun_blocks
         << " frames=" << report.frames
         << " invalid_input=" << report.invalid_input << " unreadable=" << report.unreadable
         << " rejected=" << report.rejected << " first_failure=" << report.first_failure;
    if (report.first_failure_bit) line << " first_failure_bit=" << *report.first_failure_bit;
    line << "\n";
    const std::string text = line.str();
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

}  // namespace g6shim::acphal
