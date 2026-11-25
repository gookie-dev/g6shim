# SPDX-FileCopyrightText: 2026 gookie and g6shim contributors
# SPDX-License-Identifier: GPL-3.0-or-later
g6shim_require_opus()
add_executable(g6shim_tests
  unit/main.cpp
  unit/sample_pe.cpp
  unit/pe_test.cpp
  unit/def_test.cpp
  unit/inventory_test.cpp
  unit/relink_test.cpp
  unit/game_drive_test.cpp
  unit/drives_test.cpp
  unit/topology_test.cpp
  unit/packet_test.cpp
  unit/view_desc_test.cpp
  unit/auto_event_test.cpp
  unit/manifest_test.cpp
  unit/input_mapping_test.cpp
  unit/apu_memory_test.cpp
  unit/shape_contexts_test.cpp
  unit/graphics_object_test.cpp
  unit/console_d3d12_test.cpp
  unit/console_device_test.cpp
  unit/xg_layout_test.cpp
  unit/capture_test.cpp
  unit/xmem_alloc_test.cpp
  unit/chad_dsp_test.cpp
  unit/xapu_test.cpp
  unit/gfx10_layout_test.cpp
  unit/gfx10_xor_checks.cpp
  unit/gfx10_xor_reference.cpp
  unit/xg_xs_test.cpp
  unit/shader_gfx10_test.cpp
  unit/shader_container_test.cpp
  unit/shader_ir_test.cpp
  unit/shader_structure_test.cpp
  unit/shader_resource_test.cpp
  unit/shader_io_test.cpp
  unit/shader_bitcode_test.cpp
  unit/shader_emit_container_test.cpp
  unit/shader_emit_sampler_test.cpp
  unit/shader_emit_wave_test.cpp
  unit/shader_emit_validator_test.cpp
  unit/shader_valu_test.cpp
  unit/shader_lowering.cpp
  unit/shader_fixtures_test.cpp
  unit/shader_image_atomic_test.cpp
  unit/shader_geometry_test.cpp
  unit/shader_lane_mask_test.cpp
  unit/shader_waterfall_test.cpp
  unit/shader_tool_test.cpp
  unit/shader_fuzz_test.cpp
  unit/parser_fuzz_test.cpp
  unit/title_memory_test.cpp
  unit/xmem_virtual_test.cpp
  unit/xmem_physical_test.cpp
  unit/xmem_statistics_test.cpp
  unit/console_model_test.cpp
  unit/advapi32_test.cpp
  unit/settings_test.cpp
  unit/xma_test.cpp
  unit/audio_dump_test.cpp
  unit/bcpack_test.cpp
  unit/xcrdapi_test.cpp
  unit/xgameruntime_xasync_test.cpp
  unit/xgameruntime_xuser_test.cpp
  unit/xgameruntime_xgame_test.cpp
  unit/xgameruntime_xsystem_test.cpp
  unit/xgameruntime_module_test.cpp
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime/runtime.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime/xasync.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime/xuser.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime/xgame.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime/xsystem.cpp"
  unit/log_format_test.cpp
  unit/cpu_emulation_test.cpp
  "${PROJECT_SOURCE_DIR}/runtime/src/cpu_emulation.cpp"
  unit/d3d12_xs_object_test.cpp
  unit/d3d12_xs_device_test.cpp
  unit/d3d12_xs_dxgi_test.cpp
  unit/d3d12_xs_fence_test.cpp
  unit/d3d12_xs_frame_test.cpp
  unit/d3d12_xs_root_test.cpp
  unit/d3d12_xs_descriptor_test.cpp
  unit/d3d12_xs_resource_test.cpp
  unit/d3d12_xs_sampler_test.cpp
  unit/d3d12_xs_host_test.cpp
  unit/bvh_reference_test.cpp
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/bvh/format.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/bvh/reference.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/bvh/builder.cpp"
  unit/bvh_builder_test.cpp
  unit/d3d12_xs_pipeline_test.cpp
  unit/d3d12_xs_list_test.cpp
  unit/d3d12_xs_acceleration_test.cpp
  unit/d3d12_xs_queue_test.cpp
  unit/d3d12_xs_window_test.cpp
  unit/d3d12_xs_primitive_test.cpp
  unit/d3d12_xs_indirect_test.cpp
  unit/d3d12_xs_draw_test.cpp
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/console.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/descriptor.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/descriptor_host.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/device.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/draw.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/dxgi.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/fence.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/frame.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/host.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/indirect.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/later.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/list.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/object.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/pipeline.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/pipeline_host.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/pipeline_library.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/pipeline_make.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/primitive.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/query.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/queue.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/resource.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/root.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/sampler.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/shader_cache.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/shadow.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/signature.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/texture.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/watch.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/window.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/window_geometry.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/graphics_object.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/device.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/objects.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/dxgi.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/frame_pacing.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/texture_shadow.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/removal.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/present.cpp"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x/constant_views.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xg_x/layout.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xg_x/computer.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xg_x/console_xg.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xmem/alloc.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xmem/virtual_memory.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xmem/physical.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xmem/statistics.cpp"
  "${PROJECT_SOURCE_DIR}/hle/chad/dsp.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xapu/apu.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xg_xs/gfx10_layout.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xg_xs/texture_computer.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/apu.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/shape.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/acp.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/hal.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/xma.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/xma_voices.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/audio_dump.cpp"
  "${PROJECT_SOURCE_DIR}/hle/acphal/audio_capture.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xcrdapi/bcpack.cpp"
  "${PROJECT_SOURCE_DIR}/hle/xcrdapi/dstorage.cpp"
  "${PROJECT_SOURCE_DIR}/hle/g6shim_advapi32/registry.cpp")
target_link_libraries(g6shim_tests PRIVATE g6shim_core g6shim_shader g6shim::winrt Opus::opus d3d12 dxgi d3dcompiler onecore mmdevapi ole32)
target_link_options(g6shim_tests PRIVATE "/EXPORT:__XMemAllocationHooks__,DATA" "/EXPORT:g6shim_xmem_map_alias")
foreach(forwarder IN ITEMS hle_g6shim_file hle_g6shim_file2 hle_g6shim_crtfs hle_g6shim_kernel32
                         hle_g6shim_libloader hle_g6shim_libloader2 hle_xgameruntime)
  if(TARGET ${forwarder})
    add_dependencies(g6shim_tests ${forwarder})
  endif()
endforeach()
target_include_directories(g6shim_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/hle/acphal"
  "${PROJECT_SOURCE_DIR}/hle/kernelx"
  "${PROJECT_SOURCE_DIR}/hle/d3d11_x"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_x"
  "${PROJECT_SOURCE_DIR}/hle"
  "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/bvh"
  "${PROJECT_SOURCE_DIR}/hle/xg_x"
  "${PROJECT_SOURCE_DIR}/hle/xmem"
  "${PROJECT_SOURCE_DIR}/hle/chad"
  "${PROJECT_SOURCE_DIR}/hle/xapu"
  "${PROJECT_SOURCE_DIR}/hle/xg_xs"
  "${PROJECT_SOURCE_DIR}/hle/xcrdapi"
  "${PROJECT_SOURCE_DIR}/hle/xgameruntime"
  "${PROJECT_SOURCE_DIR}/runtime/src"
  "${PROJECT_SOURCE_DIR}/winrt")
g6shim_add_compute_dxil(g_bvh_build_dxil "${PROJECT_SOURCE_DIR}/hle/d3d12_xs/bvh/build.hlsl" main)
g6shim_target_compute_dxil(g6shim_tests g_bvh_build_dxil)
add_test(NAME unit COMMAND g6shim_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 30
  ENVIRONMENT "G6SHIM_LOG=${CMAKE_CURRENT_BINARY_DIR}/unit.log;G6SHIM_LOG_STDERR=0")

set(sample "${CMAKE_CURRENT_SOURCE_DIR}/integration/sample")
set(fake_lib "${CMAKE_CURRENT_BINARY_DIR}/fakeconsole.lib")
add_custom_command(OUTPUT "${fake_lib}"
  COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${sample}/fakeconsole.def" "/out:${fake_lib}"
  DEPENDS "${sample}/fakeconsole.def"
  VERBATIM)
add_custom_target(sample_fakeconsole_lib DEPENDS "${fake_lib}")

add_library(sample_gamelib SHARED "${sample}/gamelib.cpp")
add_executable(sample_game "${sample}/sample_game.cpp")
add_dependencies(sample_game sample_fakeconsole_lib)
target_link_libraries(sample_game PRIVATE sample_gamelib "${fake_lib}")
set_target_properties(sample_gamelib sample_game PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/sample_game")
set_target_properties(sample_gamelib PROPERTIES OUTPUT_NAME gamelib)

add_executable(spawn_suspended integration/spawn_suspended.cpp)

add_test(NAME integration
  COMMAND "${CMAKE_COMMAND}"
    "-DSCAN=$<TARGET_FILE:g6shim-scan>"
    "-DSTUBGEN=$<TARGET_FILE:g6shim-stubgen>"
    "-DRUN=$<TARGET_FILE:g6shim-run>"
    "-DRELINK=$<TARGET_FILE:g6shim-relink>"
    "-DSPAWN=$<TARGET_FILE:spawn_suspended>"
    "-DGAME_DIR=$<TARGET_FILE_DIR:sample_game>"
    "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK=${CMAKE_BINARY_DIR}/integration/$<CONFIG>"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DCONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_pipeline.cmake")
set_tests_properties(integration PROPERTIES TIMEOUT 30)
