// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_capture.hpp"

#include "audio_dump.hpp"

#include <g6shim/log.hpp>
#include <g6shim/settings.hpp>
#include <windows.h>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace g6shim::acphal {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint16_t kChannels = 2;
constexpr std::uint32_t kFrameBytes = kChannels * kPcmBytes;
constexpr REFERENCE_TIME kBufferDuration = 10'000'000;
constexpr DWORD kPollMilliseconds = 10;
constexpr std::uint64_t kTicksPerSecond = 10'000'000;
constexpr std::uint64_t kGapFrames = kRate / 500;
constexpr std::size_t kSilenceChunkBytes = 64 * 1024;
constexpr wchar_t kRecorderExport[] = L"g6shim_audio_capture_recorder";
constexpr char kCaptureVariable[] = "G6SHIM_AUDIO_CAPTURE";
constexpr wchar_t kCaptureVariableWide[] = L"G6SHIM_AUDIO_CAPTURE";
constexpr wchar_t kTargetVariable[] = L"G6SHIM_AUDIO_CAPTURE_OF";
constexpr wchar_t kRundll32[] = L"\\System32\\rundll32.exe";
constexpr DWORD kRecorderCreationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;

std::optional<std::wstring> environment_variable(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return std::wstring(buffer.data(), length);
}

std::optional<std::wstring> module_path(const void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
        return std::nullopt;
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return std::wstring(buffer.data(), length);
}

std::wstring system_rundll32() {
    std::wstring root;
    if (auto value = environment_variable(L"SystemRoot")) root = std::move(*value);
    if (root.empty()) root = L"C:\\Windows";
    return root + kRundll32;
}

bool assignment_of(const wchar_t* entry, const wchar_t* name) {
    const std::size_t length = std::wcslen(name);
    return _wcsnicmp(entry, name, length) == 0 && entry[length] == L'=';
}

}  // namespace

std::vector<wchar_t> recorder_environment(std::uint32_t target, const std::wstring& capture_path) {
    std::vector<wchar_t> block;
    const wchar_t* current = GetEnvironmentStringsW();
    if (current) {
        for (const wchar_t* entry = current; *entry;) {
            const std::size_t length = std::wcslen(entry);
            if (!assignment_of(entry, kTargetVariable) && !assignment_of(entry, kCaptureVariableWide))
                block.insert(block.end(), entry, entry + length + 1);
            entry += length + 1;
        }
        FreeEnvironmentStringsW(const_cast<wchar_t*>(current));
    }
    const std::wstring target_value = std::wstring(kTargetVariable) + L"=" + std::to_wstring(target);
    block.insert(block.end(), target_value.begin(), target_value.end());
    block.push_back(L'\0');
    const std::wstring capture_value = std::wstring(kCaptureVariableWide) + L"=" + capture_path;
    block.insert(block.end(), capture_value.begin(), capture_value.end());
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

HANDLE start_with_environment(std::wstring command, std::vector<wchar_t>& environment) {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, kRecorderCreationFlags,
                        environment.data(), nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}

namespace {

bool spawn_recorder(const std::wstring& dll, const std::wstring& exe, const std::wstring& capture_path) {
    std::vector<wchar_t> environment = recorder_environment(GetCurrentProcessId(), capture_path);
    const HANDLE process =
        start_with_environment(L"\"" + exe + L"\" \"" + dll + L"\"," + std::wstring(kRecorderExport), environment);
    if (process == nullptr) {
        G6SHIM_WARN("audio capture not started: error %lu", GetLastError());
        return false;
    }
    CloseHandle(process);
    return true;
}

void start_timing_probe() {
    if (!g6shim::trace_enabled()) return;
    std::thread([] {
        for (;;) {
            Sleep(1000);
            const Clock::time_point at = Clock::now();
            Sleep(1);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - at).count();
            G6SHIM_TRACE("Sleep(1) took %.3f ms", ms);
        }
    }).detach();
}

void start_audio_capture_once() {
    start_timing_probe();
    const auto path = settings::get_wide(kCaptureVariable);
    if (!path || path->empty()) return;
    const auto dll = module_path(reinterpret_cast<const void*>(&start_audio_capture_once));
    if (!dll) {
        G6SHIM_WARN("audio capture not started: this module has no path");
        return;
    }
    if (spawn_recorder(*dll, system_rundll32(), *path))
        G6SHIM_INFO("audio capture: recording to %ls", path->c_str());
}

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { return S_OK; }

    static ActivationHandler* create() { return new (std::nothrow) ActivationHandler(); }

private:
    std::atomic<ULONG> references_{1};
};

struct ProcessLoopbackActivation {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    PROPVARIANT variant{};

    explicit ProcessLoopbackActivation(std::uint32_t pid) {
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof params;
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    }
};

WAVEFORMATEX capture_format() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kRate;
    format.nAvgBytesPerSec = kRate * kFrameBytes;
    format.nBlockAlign = static_cast<WORD>(kFrameBytes);
    format.wBitsPerSample = kPcmBits;
    return format;
}

IAudioClient* activate_client(std::uint32_t pid, HRESULT* result) {
    *result = E_OUTOFMEMORY;
    ActivationHandler* handler = ActivationHandler::create();
    if (!handler) return nullptr;
    ProcessLoopbackActivation activation(pid);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activation.variant, handler, &operation);
    handler->Release();
    *result = hr;
    if (FAILED(hr) || !operation) {
        if (operation) operation->Release();
        return nullptr;
    }
    IAudioClient* client = nullptr;
    for (;;) {
        HRESULT activated = E_FAIL;
        IUnknown* unknown = nullptr;
        const HRESULT call = operation->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(call)) {
            if (SUCCEEDED(activated) && unknown)
                hr = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
            else
                hr = FAILED(activated) ? activated : E_NOINTERFACE;
            if (unknown) unknown->Release();
            break;
        }
        Sleep(kPollMilliseconds);
    }
    operation->Release();
    *result = hr;
    return SUCCEEDED(hr) ? client : nullptr;
}

void append_silence(WavFile& file, std::uint64_t frames) {
    static const std::array<std::uint8_t, kSilenceChunkBytes> silence{};
    std::uint64_t bytes = frames * kFrameBytes;
    while (bytes > 0) {
        const std::size_t chunk = static_cast<std::size_t>(bytes < silence.size() ? bytes : silence.size());
        file.append(silence.data(), chunk);
        bytes -= chunk;
    }
}

void record(std::uint32_t pid, const std::wstring& path) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!target) {
        G6SHIM_WARN("audio capture stopped: process %lu cannot be watched, error %lu", pid, GetLastError());
        if (com) CoUninitialize();
        return;
    }
    HRESULT activated = E_FAIL;
    IAudioClient* client = activate_client(pid, &activated);
    if (!client) {
        G6SHIM_WARN("audio capture stopped: process loopback is not available (0x%08lX)",
                    static_cast<unsigned long>(activated));
        CloseHandle(target);
        if (com) CoUninitialize();
        return;
    }
    const WAVEFORMATEX format = capture_format();
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                    kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) {
        G6SHIM_WARN("audio capture stopped: audio client initialization failed with 0x%08lX", hr);
        client->Release();
        CloseHandle(target);
        if (com) CoUninitialize();
        return;
    }
    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        G6SHIM_WARN("audio capture stopped: no capture service, 0x%08lX", hr);
        client->Release();
        CloseHandle(target);
        if (com) CoUninitialize();
        return;
    }
    auto wav = WavFile::create(path, kRate, kChannels);
    if (!wav) {
        G6SHIM_WARN("audio capture stopped: cannot write %ls", path.c_str());
        capture->Release();
        client->Release();
        CloseHandle(target);
        if (com) CoUninitialize();
        return;
    }
    hr = client->Start();
    if (FAILED(hr)) {
        G6SHIM_WARN("audio capture stopped: audio client did not start, 0x%08lX", hr);
        capture->Release();
        client->Release();
        CloseHandle(target);
        if (com) CoUninitialize();
        return;
    }
    G6SHIM_INFO("audio capture: recording process %lu to %ls", pid, path.c_str());

    std::optional<std::uint64_t> first_qpc;
    std::uint64_t written = 0;
    Clock::time_point patched = Clock::now();
    for (;;) {
        Sleep(kPollMilliseconds);
        const bool ended = WaitForSingleObject(target, 0) == WAIT_OBJECT_0;
        for (;;) {
            UINT32 available = 0;
            hr = capture->GetNextPacketSize(&available);
            if (FAILED(hr) || available == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
            if (FAILED(hr)) break;
            if (!first_qpc) first_qpc = qpc;
            const std::uint64_t at = qpc >= *first_qpc ? (qpc - *first_qpc) * kRate / kTicksPerSecond : written;
            if (at > written + kGapFrames) {
                G6SHIM_TRACE("audio capture: %.0f ms with nothing played",
                             static_cast<double>((at - written) * 1000 / kRate));
                append_silence(*wav, at - written);
                written = at;
            }
            const std::size_t bytes = static_cast<std::size_t>(frames) * kFrameBytes;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                append_silence(*wav, frames);
            else
                wav->append(data, bytes);
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && written > 0)
                G6SHIM_TRACE("audio capture: discontinuity");
            written += frames;
            capture->ReleaseBuffer(frames);
        }
        if (ended || Clock::now() - patched >= std::chrono::seconds(1)) {
            if (!wav->update_header()) G6SHIM_WARN("audio capture: header not updated");
            patched = Clock::now();
        }
        if (ended) break;
    }
    wav->close();
    G6SHIM_INFO("audio capture: %llu frames recorded", static_cast<unsigned long long>(written));
    capture->Release();
    client->Release();
    CloseHandle(target);
    if (com) CoUninitialize();
}

void run_recorder() {
    const auto target = environment_variable(kTargetVariable);
    const auto path = settings::get_wide(kCaptureVariable);
    if (!target || !path || path->empty()) return;
    const std::uint32_t pid = static_cast<std::uint32_t>(std::wcstoul(target->c_str(), nullptr, 10));
    if (pid == 0) return;
    record(pid, *path);
}

}  // namespace

void start_audio_capture() {
    static std::once_flag once;
    std::call_once(once, start_audio_capture_once);
}

}  // namespace g6shim::acphal

extern "C" __declspec(dllexport) void CALLBACK g6shim_audio_capture_recorder(HWND, HINSTANCE, LPSTR, int) {
    g6shim::acphal::run_recorder();
}
