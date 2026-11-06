# pdz_compiler.cmake: compiler experiments (docs/PERFORMANCE.md, "Compiler experiments").
#
# Every option is OFF / empty by default, and then this file adds nothing: the default build's command lines are the
# same as without it. It is included from CMakeLists.txt after build_optimizations.cmake and before the SDK is added,
# so add_compile_options() / add_link_options() here reach the SDK, its third-party libraries, the generated code
# (target perfectdarkzerorecomp_recomp, generated/default/*.cpp) and the title's own sources. pdz_compiler_finalize()
# runs at the end of CMakeLists.txt, once the SDK targets exist.
#
# What the default Release build already does (build_optimizations.cmake + the SDK, clang 22 / lld-link):
#   C++: -O3 -flto=thin (RDRRECOMP_ENABLE_LTO, ON) -march=x86-64-v2, then -msse4.1 from the SDK. So ThinLTO is
#        already on for all C++ (generated code, SDK, mods); lld-link runs its post-link ThinLTO backend at its
#        default /opt:lldlto=2 (the clang driver passes no LTO level to lld-link).
#   C:   -O3 -march=x86-64-v2 -msse4.1, no LTO (SDL3, FFmpeg's libavcodec / libavutil, mspack, xxhash, aes ...).
#   SDK targets add -ffp-model=strict (= -ffp-contract=off -frounding-math -ffp-exception-behavior=strict); the
#   generated code and the title's sources do not (clang's default -ffp-contract=on).

option(PDZ_CPU_V3
    "Experiment: -march=x86-64-v3 (AVX2, FMA, BMI1/2, LZCNT, MOVBE, F16C) and -ffp-contract=off on all C/C++; the exe then needs such a CPU"
    OFF)
option(PDZ_THINLTO
    "Experiment: ThinLTO link tuning on top of the existing ThinLTO: O3 LTO backend, all threads, an LTO cache"
    OFF)
set(PDZ_PGO "" CACHE STRING "Experiment: clang front-end PGO: empty = off, gen = instrumented build, use = optimise with PDZ_PGO_PROFILE")
set_property(CACHE PDZ_PGO PROPERTY STRINGS "" gen use)
set(PDZ_PGO_PROFILE "" CACHE FILEPATH "PDZ_PGO=use: the merged profile (llvm-profdata merge output, .profdata)")
set(PDZ_PGO_SCOPE "all" CACHE STRING "PDZ_PGO scope: all = SDK + title + generated code, title = the exe's targets only")
set_property(CACHE PDZ_PGO_SCOPE PROPERTY STRINGS all title)
option(PDZ_SDK_OUT_IN_BUILD
    "Put the SDK's libraries, DLLs and tools in <build dir>/rexglue-out instead of the shared thirdparty/rexglue-sdk/out/<platform> (variant builds)"
    OFF)

if((PDZ_CPU_V3 OR PDZ_THINLTO OR NOT PDZ_PGO STREQUAL "")
   AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PDZ_CPU_V3 / PDZ_THINLTO / PDZ_PGO are written for clang / lld-link")
endif()

# --- PDZ_CPU_V3 ---------------------------------------------------------------------------------------------------
# -march=x86-64-v3 comes after build_optimizations.cmake's -march=x86-64-v2 on every command line (same directory,
# added later), and the last -march wins. The SDK's -msse4.1 comes later still (its CMakeLists.txt and
# rexglue_apply_target_settings) and only adds a feature v3 already has: clang -### for
# "-march=x86-64-v2 -march=x86-64-v3 -msse4.1" gives -target-cpu x86-64-v3 -target-feature +sse4.1.
# -ffp-contract=off: with FMA in the target, clang's default -ffp-contract=on would fuse a*b+c written in one
# expression (title / mod code, SDK headers compiled into the generated code) into one rounding. Off keeps every
# multiply and add rounded separately, as the x86-64-v2 build does (it has no FMA instruction to fuse into).
# std::fma (the PPC fmadd family) is an exact fused operation either way: a UCRT call today, vfmadd with v3.
if(PDZ_CPU_V3)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        message(FATAL_ERROR "PDZ_CPU_V3 is for x86-64 builds")
    endif()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>")
    message(STATUS "PDZ_CPU_V3: -march=x86-64-v3 -ffp-contract=off on all C/C++")
endif()

# --- PDZ_THINLTO --------------------------------------------------------------------------------------------------
# C++ is already -flto=thin (RDRRECOMP_ENABLE_LTO); this keeps it on even if that is OFF and tunes the link:
#   /opt:lldlto=3          post-link ThinLTO optimisation at O3 (lld-link's default is 2): changes the code
#   /opt:lldltojobs=all    all hardware threads for the ThinLTO backends (default: physical cores): link time only
#   /lldltocache:<dir>     reuse unchanged modules' native objects across relinks: link time only
# C stays out of LTO: no hot game code is C (SDL3, FFmpeg, mspack, xxhash, aes), and FFmpeg's inline assembly is
# the usual LTO trouble spot. The generated code, the SDK runtime (rexruntime.dll), the GPU plugin
# (rexgpu-xenos.dll) and the title are all C++. Each DLL and the exe is its own LTO unit: nothing inlines across a
# DLL boundary (the SDK helpers the generated code calls most are header inlines, compiled into the exe).
if(PDZ_THINLTO)
    if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "-flto=thin")
        string(APPEND CMAKE_CXX_FLAGS_RELEASE " -flto=thin")
    endif()
    if(WIN32)
        add_link_options(
            "LINKER:/opt:lldlto=3"
            "LINKER:/opt:lldltojobs=all"
            "LINKER:/lldltocache:${CMAKE_BINARY_DIR}/lto-cache"
            "LINKER:/lldltocachepolicy:cache_size_bytes=32g")
    else()
        add_link_options(
            "LINKER:--lto-O3"
            "LINKER:--thinlto-jobs=all"
            "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
    endif()
    message(STATUS "PDZ_THINLTO: -flto=thin (C++), LTO backend O3, all threads, cache ${CMAKE_BINARY_DIR}/lto-cache")
endif()

# --- PDZ_PGO --------------------------------------------------------------------------------------------------------
# gen: -fprofile-instr-generate. clang adds --dependent-lib=clang_rt.profile-x86_64.lib to every object and the
#      driver links the runtime, so each instrumented module (the exe, rexruntime.dll, rexgpu-xenos.dll) carries its
#      own copy and writes its own .profraw. The file name comes from LLVM_PROFILE_FILE at run time (use %m and %p:
#      one file per module and process). The SDK ends the process with std::_Exit (rex_app.cpp OnClosing), which
#      skips the exe's atexit handlers, so src/pgo_flush.cpp writes the profiles when the window is asked to close
#      (src/perfectdarkzerorecomp_app.h, PDZ_PGO_GEN).
# use: -fprofile-instr-use=<PDZ_PGO_PROFILE>. The profile's MD5 goes on the command line as a define, and the file is
#      a configure dependency: a new profile reconfigures and rebuilds everything (ninja cannot see the file itself).
# The function hashes clang matches are front-end (AST) hashes, so gen and use may differ in LTO / -march; the
# sources (generated code included) must be the same.
set(_pdz_pgo_compile "")
set(_pdz_pgo_link "")
if(PDZ_PGO STREQUAL "gen")
    set(_pdz_pgo_compile -fprofile-instr-generate)
    set(_pdz_pgo_link -fprofile-instr-generate)
elseif(PDZ_PGO STREQUAL "use")
    if(NOT PDZ_PGO_PROFILE OR NOT EXISTS "${PDZ_PGO_PROFILE}")
        message(FATAL_ERROR "PDZ_PGO=use needs PDZ_PGO_PROFILE = an existing .profdata (got '${PDZ_PGO_PROFILE}')")
    endif()
    file(TO_CMAKE_PATH "${PDZ_PGO_PROFILE}" _pdz_pgo_profile)
    file(MD5 "${_pdz_pgo_profile}" _pdz_pgo_md5)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pdz_pgo_profile}")
    set(_pdz_pgo_compile
        "-fprofile-instr-use=${_pdz_pgo_profile}"
        -Wno-profile-instr-unprofiled
        -Wno-profile-instr-out-of-date
        -Wno-profile-instr-missing
        "-DPDZ_PGO_PROFILE_MD5=${_pdz_pgo_md5}")
elseif(NOT PDZ_PGO STREQUAL "")
    message(FATAL_ERROR "PDZ_PGO must be empty, gen or use (got '${PDZ_PGO}')")
endif()
if(NOT PDZ_PGO STREQUAL "" AND NOT PDZ_PGO_SCOPE MATCHES "^(all|title)$")
    message(FATAL_ERROR "PDZ_PGO_SCOPE must be all or title (got '${PDZ_PGO_SCOPE}')")
endif()
if(_pdz_pgo_compile AND PDZ_PGO_SCOPE STREQUAL "all")
    foreach(_opt IN LISTS _pdz_pgo_compile)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:${_opt}>")
    endforeach()
    if(_pdz_pgo_link)
        add_link_options(${_pdz_pgo_link})
    endif()
endif()
if(_pdz_pgo_compile)
    message(STATUS "PDZ_PGO: ${PDZ_PGO} (scope ${PDZ_PGO_SCOPE}) ${PDZ_PGO_PROFILE}")
endif()

# Collects the buildsystem targets of a directory and its subdirectories (as rexglue_link_audit.cmake does).
function(_pdz_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        _pdz_collect_targets("${_sub}" _sub_targets)
        list(APPEND _targets ${_sub_targets})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Called at the end of CMakeLists.txt.
function(pdz_compiler_finalize)
    # The SDK writes its libraries, DLLs and rexglue.exe to thirdparty/rexglue-sdk/out/<platform> (its
    # CMakeLists.txt, CMAKE_*_OUTPUT_DIRECTORY), whatever the build folder: two build folders of one checkout
    # overwrite each other's rexruntime.dll there, and build_render.bat copies from there. Variant builds move them
    # into their own build folder. The exe's POST_BUILD copies ($<TARGET_RUNTIME_DLLS>, $<TARGET_FILE:rexgpu-xenos>)
    # and codegen's $<TARGET_FILE:rex::rexglue> follow the new place.
    if(PDZ_SDK_OUT_IN_BUILD)
        set(_out "${CMAKE_BINARY_DIR}/rexglue-out")
        _pdz_collect_targets("${REXSDK_DIR}" _sdk_targets)
        set(_moved 0)
        foreach(_t IN LISTS _sdk_targets)
            get_target_property(_type ${_t} TYPE)
            if(_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
                set_target_properties(${_t} PROPERTIES
                    ARCHIVE_OUTPUT_DIRECTORY "${_out}"
                    LIBRARY_OUTPUT_DIRECTORY "${_out}"
                    RUNTIME_OUTPUT_DIRECTORY "${_out}")
                math(EXPR _moved "${_moved} + 1")
            endif()
        endforeach()
        message(STATUS "PDZ_SDK_OUT_IN_BUILD: ${_moved} SDK targets write to ${_out}")
    endif()

    if(_pdz_pgo_compile AND PDZ_PGO_SCOPE STREQUAL "title")
        foreach(_t perfectdarkzerorecomp perfectdarkzerorecomp_recomp)
            if(TARGET ${_t})
                foreach(_opt IN LISTS _pdz_pgo_compile)
                    target_compile_options(${_t} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:${_opt}>")
                endforeach()
            endif()
        endforeach()
        if(_pdz_pgo_link)
            target_link_options(perfectdarkzerorecomp PRIVATE ${_pdz_pgo_link})
        endif()
    endif()

    if(PDZ_PGO STREQUAL "gen")
        target_sources(perfectdarkzerorecomp PRIVATE "${CMAKE_SOURCE_DIR}/src/pgo_flush.cpp")
        target_compile_definitions(perfectdarkzerorecomp PRIVATE PDZ_PGO_GEN=1)
        # The DLLs' profile writers, exported so pgo_flush.cpp can call them by name (each module has its own
        # runtime copy). They also write from DLL_PROCESS_DETACH, which ExitProcess (std::_Exit) still runs.
        if(WIN32 AND PDZ_PGO_SCOPE STREQUAL "all")
            foreach(_dll rexruntime rexgpu-xenos)
                if(TARGET ${_dll})
                    target_link_options(${_dll} PRIVATE "LINKER:/EXPORT:__llvm_profile_write_file")
                endif()
            endforeach()
        endif()
    endif()
endfunction()
