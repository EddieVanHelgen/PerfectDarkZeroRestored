# The XenosRecomp fork (pdz branch) as a static library inside PDZRestored.exe (thirdparty/XenosRecomp-pdz/README_PDZ.md).
# The native renderer translates shaders in-process with it; no separate translator exe is built or shipped.

set(PDZ_XR_DIR "${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp-pdz")
set(PDZ_XR_GEN "${CMAKE_BINARY_DIR}/pdz_xenosrecomp")

# shader_common_pdz.hlsli as bytes (it goes verbatim into every translated shader), rewritten when it changes.
file(READ "${PDZ_XR_DIR}/shader_common_pdz.hlsli" _pdz_xr_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _pdz_xr_bytes "${_pdz_xr_hex}")
file(CONFIGURE OUTPUT "${PDZ_XR_GEN}/xenosrecomp_include.inc" CONTENT "${_pdz_xr_bytes}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PDZ_XR_DIR}/shader_common_pdz.hlsli")

add_library(pdz_xenosrecomp STATIC
    "${PDZ_XR_DIR}/dxc_compiler.cpp"
    "${PDZ_XR_DIR}/pdz_recompiler.cpp"
    "${PDZ_XR_DIR}/shader_recompiler.cpp"
    "${PDZ_XR_DIR}/xenosrecomp_pdz.cpp")
# The fork's own settings (its CMakeLists.txt): C++17, the pch, clang's warning set, no UNLEASHED_RECOMP.
set_target_properties(pdz_xenosrecomp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_precompile_headers(pdz_xenosrecomp PRIVATE "${PDZ_XR_DIR}/pch.h")
target_include_directories(pdz_xenosrecomp PRIVATE "${PDZ_XR_DIR}" "${PDZ_XR_GEN}"
    "${CMAKE_SOURCE_DIR}/thirdparty/rexglue-sdk/thirdparty/dxc/include")
target_link_libraries(pdz_xenosrecomp PRIVATE fmt)
target_compile_definitions(pdz_xenosrecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pdz_xenosrecomp PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions
        -Wno-deprecated-declarations)
endif()

target_link_libraries(perfectdarkzerorecomp PRIVATE pdz_xenosrecomp)
target_include_directories(perfectdarkzerorecomp PRIVATE "${PDZ_XR_DIR}"
    "${CMAKE_SOURCE_DIR}/thirdparty/rexglue-sdk/thirdparty/stb") # stb_image.h: its zlib decoder (native_generate.cpp)

# DXC (decided 2026-11-03: shipped next to the exe under the DXC release licence; never committed): dxcompiler.dll
# 1.8.2407.7 and dxil.dll 101.8.2407.12, the release the shader cache was compiled with. Copied from PDZ_DXC_BIN_DIR.
set(PDZ_DXC_BIN_DIR "C:/Games/PDZRenderResearch/native/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64" CACHE PATH
    "Folder with dxcompiler.dll + dxil.dll (DXC 1.8.2407) to copy next to PDZRestored.exe")
if(EXISTS "${PDZ_DXC_BIN_DIR}/dxcompiler.dll" AND EXISTS "${PDZ_DXC_BIN_DIR}/dxil.dll")
    add_custom_command(TARGET perfectdarkzerorecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDZ_DXC_BIN_DIR}/dxcompiler.dll" "${PDZ_DXC_BIN_DIR}/dxil.dll"
                "$<TARGET_FILE_DIR:perfectdarkzerorecomp>"
        COMMENT "Copying dxcompiler.dll + dxil.dll next to PDZRestored.exe")
else()
    message(WARNING "PDZ_DXC_BIN_DIR (${PDZ_DXC_BIN_DIR}) has no dxcompiler.dll + dxil.dll: the native renderer can't "
                    "translate shaders without them")
endif()

# The bind list (data/native_bind_list.txt: hashes and vertex declaration formats only, tools/shaders/make_bind_list.py),
# embedded for the shader generator (native_generate.cpp).
file(READ "${CMAKE_SOURCE_DIR}/data/native_bind_list.txt" _pdz_bl_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _pdz_bl_bytes "${_pdz_bl_hex}")
file(CONFIGURE OUTPUT "${PDZ_XR_GEN}/native_bind_list.inc" CONTENT "${_pdz_bl_bytes}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/native_bind_list.txt")
# The pipeline list (data/native_pipeline_list.txt: shader hashes, vertex layouts and pipeline state only,
# tools/shaders/make_pipeline_list.py), embedded for the first start's pipeline pass (native_pipeline_seed.cpp).
file(READ "${CMAKE_SOURCE_DIR}/data/native_pipeline_list.txt" _pdz_pl_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _pdz_pl_bytes "${_pdz_pl_hex}")
file(CONFIGURE OUTPUT "${PDZ_XR_GEN}/native_pipeline_list.inc" CONTENT "${_pdz_pl_bytes}\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/native_pipeline_list.txt")
target_include_directories(perfectdarkzerorecomp PRIVATE "${PDZ_XR_GEN}")
