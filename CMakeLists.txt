# perfectdarkzerorecomp - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(perfectdarkzerorecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Fix git symlinks on Windows (libmspack in SDK uses symlinks)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix_symlinks.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_optimizations.cmake)
# Compiler experiments: -march=x86-64-v3, ThinLTO tuning, PGO (all off by default; docs/PERFORMANCE.md).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pdz_compiler.cmake)

# Build ReXGlue SDK from source (git submodule)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk" CACHE PATH "Path to rexglue-sdk source tree" FORCE)

# Enable both graphics backends
if(WIN32)
    set(REXGLUE_USE_D3D12 ON CACHE BOOL "Enable D3D12 graphics backend")
endif()
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend")


include(generated/rexglue.cmake)

# tu3-base branch: the game is Title Update 3 (assets/default.xexp). Every
# mod hook names a launch address, so the mods stay out until re-targeted.
option(PDZ_NO_MODS "Build without the game mods (TU3 bring-up)" ON)

set(PERFECTDARKZERORECOMP_SOURCES
    src/main.cpp
    src/game_patches.cpp
    src/game_cvars.cpp
    src/local_profiles.cpp
    src/mod/third_person.cpp
    src/mod/look_tuning.cpp
    src/mod/live_toggles.cpp
    src/mod/aim_assist.cpp
    src/mod/ragdoll_tuning.cpp
    src/mod/fov_tuning.cpp
    src/mod/cover_log.cpp
    src/mod/ai_cover_fixes.cpp
    src/mod/controls_fixes.cpp
    src/mod/combat_log.cpp
    src/mod/shot_log.cpp
    src/mod/game_fixes.cpp
    src/mod/ai_game_fixes.cpp
    src/mod/ai_state_log.cpp
    src/mod/music_log.cpp
    src/mod/dlc_maps.cpp
    src/mod/mp_music.cpp
    src/mod/tu_fixes.cpp
    src/mod/button_log.cpp
    src/mod/fx_log.cpp
)

if(PDZ_NO_MODS)
    set(PERFECTDARKZERORECOMP_SOURCES src/main.cpp src/game_cvars.cpp)
endif()

# Ported to Title Update 3 so far (docs/TU3_BASE.md, re-targeting checklist):
# the interactive music player TU3 removed (restored from launch), the
# 60 fps unlock, the kept aim options, the fast-look zoom brake, the kept
# game fixes, the enemy AI fixes, the multiplayer Music row and Rare's cut
# Plant the Flag scenario (off by default).
list(APPEND PERFECTDARKZERORECOMP_SOURCES
    src/music/music_restore.cpp
    src/game_patches.cpp
    src/mod/aim_assist.cpp
    src/mod/mouse_look.cpp
    src/mod/zoom_steps.cpp
    src/mod/fov_tuning.cpp
    src/mod/look_tuning.cpp
    src/mod/game_fixes.cpp
    src/mod/ai_game_fixes.cpp
    src/mod/ai_cover_fixes.cpp
    src/mod/fix_log.cpp
    src/mod/mp_music.cpp
    src/mod/mp_music_custom.cpp
    src/mod/cut_scenarios.cpp
    src/mod/bot_turrets.cpp
    src/mod/minigame_log.cpp
    src/mod/objective_bots.cpp
    src/mod/bot_watch.cpp
    src/mod/cue_fixes.cpp
    src/mod/audio_presence_log.cpp
    src/mod/restorations.cpp
    src/mod/mp_announcer.cpp
    src/mod/anim_log.cpp
    src/mod/anim_viewer.cpp
    src/mod/ai_behaviour_debug.cpp
    src/mod/physics_step.cpp
    src/mod/file_read_log.cpp
    src/mod/stream_bank_warm.cpp
    src/mod/transparency_water.cpp
    src/mod/native_memcpy.cpp
    src/mod/hook_profile.cpp
    src/mod/crash_dump.cpp
    src/mod/render/shadow_record.cpp
    src/mod/render/shadow_hooks.cpp
    src/mod/render/shadow_cvars.cpp
    src/mod/render/shadow_dump.cpp
    src/mod/render/native/native_cvars.cpp
    src/mod/render/native/native_device.cpp
    src/mod/render/native/native_passes.cpp
    src/mod/render/native/native_postfx.cpp
    src/mod/render/native/native_textures.cpp
    src/mod/render/native/native_async_upload.cpp
    src/mod/render/native/native_surfaces.cpp
    src/mod/render/native/native_present.cpp
    src/mod/render/native/native_autodiff.cpp
    src/mod/render/native/native_passdump.cpp
    src/mod/render/test_harness.cpp
    src/mod/render/native/native_shaders.cpp
    src/mod/render/native/native_binder.cpp
    src/mod/render/native/native_pipeline.cpp
    src/mod/render/native/native_depth20e4.cpp
    src/mod/render/native/native_coissue.cpp
    src/mod/render/native/native_blend7e3.cpp
    src/mod/render/native/native_draw.cpp
    src/mod/render/native/native_drawdump.cpp
    src/mod/render/native/native_memlog.cpp
    src/mod/render/native/native_watchdog.cpp
    src/mod/render/native/native_recthread.cpp
    src/mod/render/native/native_reclist.cpp
    src/mod/render/shadow_assert.cpp
    src/mod/render/frame_timing.cpp
    src/mod/render/gpu_census.cpp
    src/mod/render/gpu_sync.cpp
    src/mod/render/pm4_skip.cpp
    src/mod/render/cpu_placement.cpp
    src/mod/render/game_pace.cpp
    src/mod/render/cutscene_smooth.cpp
    src/mod/render/latency_log.cpp
    src/mod/render/sun_shadows.cpp
    src/mod/render/game_thread_sampler.cpp
    src/mod/render/guest_ip_profile.cpp
    src/mod/player_limit.cpp
    src/mod/test_mp_setup.cpp
    src/build_info.cpp
    src/mod/render/native/native_dxc.cpp
    src/mod/render/native/native_generate.cpp
    src/mod/render/native/native_pipeline_seed.cpp
    src/setup/first_run.cpp
    src/setup/setup_window.cpp
    src/setup/theme.cpp
    src/setup/settings_window.cpp
    src/setup/update_check.cpp
    src/setup/map_packs.cpp
)

if(WIN32)
    list(APPEND PERFECTDARKZERORECOMP_SOURCES
        src/perfectdarkzerorecomp.rc
    )
endif()

if(WIN32)
    add_executable(perfectdarkzerorecomp WIN32 ${PERFECTDARKZERORECOMP_SOURCES})
else()
    add_executable(perfectdarkzerorecomp ${PERFECTDARKZERORECOMP_SOURCES})
endif()
# The release name (2026-11-03): the exe is PDZRestored.exe (with PDZRestored.map). The target, the generated code
# (perfectdarkzerorecomp_*) and the user data folder (Documents\perfectdarkzerorecomp-tu3,
# src/perfectdarkzerorecomp_app.h) keep the old name.
set_target_properties(perfectdarkzerorecomp PROPERTIES OUTPUT_NAME PDZRestored)

rexglue_setup_target(perfectdarkzerorecomp GPU_PLUGINS xenos)
# Startup log line (src/build_info.cpp): the build time and the source commit, written at every build by
# cmake/pdz_build_info.cmake into the build folder (rewritten only when the commit changes).
add_custom_target(pdz_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/pdz_info
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pdz_build_info.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/pdz_info/pdz_build_info.h
    COMMENT "Build info (commit, time)")
add_dependencies(perfectdarkzerorecomp pdz_build_info)
target_include_directories(perfectdarkzerorecomp PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/pdz_info)
# Crash breadcrumbs (src/mod/crash_dump.cpp): dbghelp for the stack walk and the minidump; /MAP so the
# host addresses it logs resolve to functions (out/build/<preset>/PDZRestored.map).
if(WIN32)
    target_link_libraries(perfectdarkzerorecomp PRIVATE dbghelp)
    target_link_options(perfectdarkzerorecomp PRIVATE "LINKER:/MAP")
endif()
if(PDZ_NO_MODS)
    target_compile_definitions(perfectdarkzerorecomp PRIVATE PDZ_NO_MODS=1)
endif()
# Counter-Operative (docs/COUNTEROP_STUDY.md), parked 2026-10-30: out of the build unless asked for. ON compiles
# src/mod/counter_op.cpp (its hooks and pdz_counterop* cvars) and the one call site in cut_scenarios.cpp.
option(PDZ_COUNTEROP "Build the parked Counter-Operative mod (src/mod/counter_op.cpp)" OFF)
if(PDZ_COUNTEROP)
    target_sources(perfectdarkzerorecomp PRIVATE src/mod/counter_op.cpp)
    target_compile_definitions(perfectdarkzerorecomp PRIVATE PDZ_COUNTEROP=1)
endif()

# SDL looks for this file (name set by settings/mapping.toml's
# hid_mappings_file) next to the exe at startup to fill in gamepads it
# doesn't already recognize. Sourced from
# https://github.com/mdqinc/SDL_GameControllerDB, see settings/README.md.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt")
    add_custom_command(TARGET perfectdarkzerorecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/settings/gamecontrollerdb.txt"
                "$<TARGET_FILE_DIR:perfectdarkzerorecomp>/gamecontrollerdb.txt"
        COMMENT "Copying gamecontrollerdb.txt next to PDZRestored.exe")
endif()

# The shader translator inside the exe, and DXC next to it (cmake/pdz_xenosrecomp.cmake).
include(cmake/pdz_xenosrecomp.cmake)

# Compiler experiments, second half: needs the SDK targets (cmake/pdz_compiler.cmake).
pdz_compiler_finalize()
