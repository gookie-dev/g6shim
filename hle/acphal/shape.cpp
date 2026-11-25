// SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "shape_contexts.hpp"

#include "audio_capture.hpp"
#include "xma_voices.hpp"

#include <g6shim/log.hpp>

namespace g6shim::acphal {

ShapeContexts& shape_contexts() {
    static ShapeContexts contexts(apu_memory());
    return contexts;
}

}  // namespace g6shim::acphal

namespace {

using namespace g6shim::acphal;

void log_allocation(const void* config, HRESULT hr) {
    if (!config) {
        G6SHIM_WARN("AcpHalAllocateShapeContexts(null) -> 0x%08lX", hr);
        return;
    }
    const ShapeCounts counts = read_shape_counts(config);
    G6SHIM_INFO("AcpHalAllocateShapeContexts: %s %u, %s %u, %s %u, %s %u, %s %u, %s %u -> 0x%08lX", kKindNames[0],
                counts[0], kKindNames[1], counts[1], kKindNames[2], counts[2], kKindNames[3], counts[3], kKindNames[4],
                counts[4], kKindNames[5], counts[5], hr);
    if (FAILED(hr)) return;
    for (std::size_t kind = 0; kind < kShapeKinds; ++kind)
        if (counts[kind] && !kKindProcessed[kind])
            G6SHIM_WARN("AcpHalAllocateShapeContexts: %u %s contexts are allocated but not processed", counts[kind],
                        kKindNames[kind]);
}

}  // namespace

extern "C" HRESULT WINAPI g6shim_AcpHalAllocateShapeContexts(void* config) {
    start_audio_capture();
    const HRESULT hr = shape_contexts().allocate(config);
    log_allocation(config, hr);
    if (SUCCEEDED(hr)) {
        const ShapeCounts counts = read_shape_counts(config);
        shape_contexts().with_table([&](const ShapeTable& table) {
            set_xma_context_array(static_cast<std::uint8_t*>(table.arrays[kShapeKindXma]), counts[kShapeKindXma]);
        });
    }
    return hr;
}

extern "C" HRESULT WINAPI g6shim_AcpHalReleaseShapeContexts() {
    G6SHIM_INFO("AcpHalReleaseShapeContexts");
    clear_xma_context_array();
    return shape_contexts().release();
}
