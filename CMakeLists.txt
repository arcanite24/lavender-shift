# Lavender Shift - Forza Horizon 2 (Xbox 360) on ShiftGlue
#
# Stage 0 reference build: the SDK's own project template, with the
# boilerplate in cmake/rexglue.cmake (from `rexglue init`, pointed at
# .local/generated and passing --xex_apply_patches=true to codegen).

cmake_minimum_required(VERSION 3.25)
project(lavender_shift LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ShiftGlue compiles its lightweight per-frame counters out of Release unless
# this is defined; Stage 0 is a measuring build (--perf_log_csv), as Pinyon
# Shift's builds are.
option(LAVENDER_SHIFT_PERF_COUNTERS "Compile ShiftGlue's per-frame perf counters" ON)
if(LAVENDER_SHIFT_PERF_COUNTERS)
    add_compile_definitions(REXGLUE_ENABLE_PERF_COUNTERS)
endif()

include(cmake/rexglue.cmake)

set(LAVENDER_SHIFT_SOURCES
    src/main.cpp
    src/lavender_shift_app.cpp
    src/diagnostics.cpp
    src/crash_reporter_win.cpp
    src/stall_dump.cpp
    src/hitch_sampler_win.cpp
    src/save_backups.cpp
    src/fixed_steps.cpp
    src/frame_rate.cpp
    src/cheats.cpp
    src/gamma_hooks.cpp
    src/guest_native/guest_native.cpp
    src/guest_native/crt_math.cpp
    src/guest_native/lzx_decoder.cpp
    src/guest_native/fmod_output.cpp
    src/guest_native/fmod_dsp.cpp
    src/guest_native/voice_chat.cpp
    src/ring_wait.cpp
    src/simulation_hooks.cpp
    src/graphics_hooks.cpp
    src/render_settings.cpp
    src/dev_options.cpp
    src/ai_driver.cpp
    src/cvar_cycle.cpp
    src/movie_hooks.cpp
    src/pause_menu_hooks.cpp
    src/wide_view.cpp
    src/ui/photo_export.cpp
    src/ui/png_writer.cpp
    src/config/host_config.cpp
    src/ui/settings_menu.cpp
    src/ui/xam_dialogs.cpp
    src/ui/hostui/bitmap_font.cpp
    src/ui/hostui/fh1_archive.cpp
    src/ui/hostui/glyph_atlas.cpp
    src/ui/hostui/host_ui.cpp
    src/ui/hostui/menu.cpp
    src/ui/hostui/pad_glyphs.cpp
    src/ui/hostui/vector_font.cpp
    src/ui/hostui/xds_texture.cpp
)

if(WIN32)
    add_executable(lavender_shift WIN32 ${LAVENDER_SHIFT_SOURCES})
else()
    add_executable(lavender_shift ${LAVENDER_SHIFT_SOURCES})
endif()

# The SDK injects rex_app.cpp into the game target; ImGui stays a private SDK
# dependency, so its headers come from the source tree, and rex/version.h is
# configured into the SDK's sub-build (as in Pinyon Shift's CMake).
target_include_directories(lavender_shift PRIVATE "${LAVENDER_SHIFT_SDK_SOURCE_DIR}/thirdparty/imgui"
    "${LAVENDER_SHIFT_SDK_SOURCE_DIR}/thirdparty/renderdoc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(REXSDK_DIR)
    set_property(SOURCE "${REXGLUE_SHARE_DIR}/rex_app.cpp" APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk/include")
endif()

# rexgpu-fh1 is ShiftGlue's Xenos (PM4) GPU plugin: the Stage 0 path.
rexglue_setup_target(lavender_shift GPU_PLUGINS fh1)
# The crash reporter writes minidumps and symbolized stacks.
target_link_libraries(lavender_shift PRIVATE dbghelp)

# The game and the SDK's DLLs use the Visual C++ runtime DLLs. Copies sit
# beside the game (Microsoft's app-local deployment), so it starts on a PC
# without the matching redistributable, as in Pinyon Shift. The developer
# prompt sets VCToolsRedistDir; a prebuilt kit carries the runtime its DLLs
# were built against, and the newer of the two serves both (Microsoft keeps
# it backward compatible). The fetched sysroot has no runtime, so there the
# kit's is the only one.
if(WIN32 AND DEFINED ENV{VCToolsRedistDir})
    set(LAVENDER_SHIFT_VC_REDIST_DIR "$ENV{VCToolsRedistDir}" CACHE PATH
        "Visual C++ redistributable folder of the build toolset")
endif()
if(WIN32 AND (LAVENDER_SHIFT_VC_REDIST_DIR OR LAVENDER_SHIFT_SHIFTGLUE_KIT))
    set(LAVENDER_SHIFT_VC_RUNTIME_DLLS)
    if(LAVENDER_SHIFT_VC_REDIST_DIR)
        file(GLOB LAVENDER_SHIFT_VC_RUNTIME_DLLS
            "${LAVENDER_SHIFT_VC_REDIST_DIR}/x64/Microsoft.VC*.CRT/*.dll")
    endif()
    if(LAVENDER_SHIFT_SHIFTGLUE_KIT AND NOT REXSDK_DIR AND
       EXISTS "${LAVENDER_SHIFT_SHIFTGLUE_KIT}/redist")
        string(JSON _lavender_kit_toolset GET "${_lavender_kit}" msvc_toolset)
        if(NOT LAVENDER_SHIFT_VC_RUNTIME_DLLS OR
           _lavender_kit_toolset VERSION_GREATER "$ENV{VCToolsVersion}")
            file(GLOB LAVENDER_SHIFT_VC_RUNTIME_DLLS "${LAVENDER_SHIFT_SHIFTGLUE_KIT}/redist/*.dll")
        endif()
        string(REGEX MATCH "^[0-9]+\\.[0-9]+" _lavender_kit_runtime "${_lavender_kit_toolset}")
        string(REGEX MATCH "^[0-9]+\\.[0-9]+" _lavender_headers "$ENV{VCToolsVersion}")
        if(NOT LAVENDER_SHIFT_VC_REDIST_DIR AND _lavender_headers AND
           _lavender_kit_runtime VERSION_LESS _lavender_headers)
            message(WARNING "The kit's C++ runtime (${_lavender_kit_toolset}) is older than "
                "the C++ library this build compiles against ($ENV{VCToolsVersion}).")
        endif()
    endif()
    if(LAVENDER_SHIFT_VC_RUNTIME_DLLS)
        add_custom_target(lavender_shift_stage_vc_runtime ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LAVENDER_SHIFT_VC_RUNTIME_DLLS} "$<TARGET_FILE_DIR:lavender_shift>"
            DEPENDS lavender_shift ${LAVENDER_SHIFT_VC_RUNTIME_DLLS}
            COMMENT "Staging the Visual C++ runtime beside lavender_shift"
            VERBATIM)
    endif()
endif()

# The archive helper tools/prepare_shaders.ps1 uses (method-21 ZIP members
# and the patched XEX image), built with the game by tools/build.ps1.
add_executable(lavender_shift_archive_extract EXCLUDE_FROM_ALL
    tools/archive_extract.cpp
    src/archive/fh_archive.cpp
)
target_include_directories(lavender_shift_archive_extract PRIVATE src)
target_link_libraries(lavender_shift_archive_extract PRIVATE rex::runtime)
