// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "acp.hpp"

#include "audio_capture.hpp"

#include <windows.h>

#include <g6shim/log.hpp>

extern "C" HRESULT WINAPI g6shim_AcpHalCreate(void** hal) {
    g6shim::acphal::start_audio_capture();
    if (!hal) return E_INVALIDARG;
    const HRESULT hr = g6shim::acphal::create_acp(reinterpret_cast<g6shim::acphal::Acp**>(hal));
    G6SHIM_INFO("AcpHalCreate -> 0x%08lX", hr);
    return hr;
}
