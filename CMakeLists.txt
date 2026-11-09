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
)

if(WIN32)
    add_executable(lavender_shift WIN32 ${LAVENDER_SHIFT_SOURCES})
else()
    add_executable(lavender_shift ${LAVENDER_SHIFT_SOURCES})
endif()

# The SDK injects rex_app.cpp into the game target; ImGui stays a private SDK
# dependency, so its headers come from the source tree, and rex/version.h is
# configured into the SDK's sub-build (as in Pinyon Shift's CMake).
target_include_directories(lavender_shift PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
set_property(SOURCE "${REXGLUE_SHARE_DIR}/rex_app.cpp" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk/include")

# rexgpu-fh1 is ShiftGlue's Xenos (PM4) GPU plugin: the Stage 0 path.
rexglue_setup_target(lavender_shift GPU_PLUGINS fh1)
