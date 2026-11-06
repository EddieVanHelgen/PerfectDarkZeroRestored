@echo off
rem Syntax check of the shadow assertion (and the files it touches) against
rem this worktree, read-only: nothing is written (no objects, no PCH).
rem Mirrors the compile rule of out\build\win-amd64-release\build.ninja
rem (target perfectdarkzerorecomp: clang++ driver, -std=c++23, same defines
rem and include set). Runs clang-cl /Zs first, then clang++ -fsyntax-only.
rem
rem   check_syntax_assert.bat [PDZ_DIR]   default C:\Games\PerfectDarkZeroRecomp-render
rem   set LLVM_BIN=...                    default C:\Program Files\LLVM\bin
rem   set SDK_OVERLAY=...                 an SDK checkout whose include\ is searched before PDZ_DIR's SDK (a local
rem                                       SDK commit not yet in that worktree's thirdparty\rexglue-sdk, e.g.
rem                                       C:\Games\PerfectDarkZeroRecomp-render-m0-sdk for gpu_census.cpp)
rem This worktree's own src\ (the one holding this file) is searched before PDZ_DIR's, so a check from another
rem worktree sees its own headers (main.cpp is checked too: the app header's OnPreSetup).
setlocal
set "HERE=%~dp0"
set "SRC=%~dp0..\.."
set "PDZ=%~1"
if "%PDZ%"=="" set "PDZ=C:\Games\PerfectDarkZeroRecomp-render"
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
set "SDK=%PDZ%\thirdparty\rexglue-sdk"
set "OUT=%PDZ%\out\build\win-amd64-release"
set "OVL="
if not "%SDK_OVERLAY%"=="" set OVL=-I"%SDK_OVERLAY%\include"

set INC=%OVL% -I"%PDZ%" -I"%SRC%" -I"%PDZ%\src" -I"%PDZ%\generated\default" -I"%SDK%\include" -I"%OUT%\rexglue-sdk\include" -I"%SDK%\thirdparty\imgui" -I"%SDK%\thirdparty\xxHash" -I"%SDK%\thirdparty\simde" -I"%SDK%\thirdparty\simde\simde" -I"%SDK%\thirdparty\fmt\include" -I"%SDK%\thirdparty\spdlog\include" -I"%OUT%\rexglue-sdk\thirdparty\sdl3\include-revision" -I"%SDK%\thirdparty\sdl3\include" -I"%SDK%\thirdparty\dxc\include" -I"%SDK%\thirdparty\vulkan-headers\include" -I"%SDK%\thirdparty\vulkan-memory-allocator\include" -I"%SDK%\thirdparty\tomlplusplus\include" -I"%SDK%\thirdparty\spirv-tools\include"
set DEFS=-DNOMINMAX -DPDZ_NO_MODS=1 -DREXGLUE_BUILD_CONFIG=\"Release\" -DREX_HAS_D3D12=1 -DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DWIN32_LEAN_AND_MEAN
set FILES="%HERE%shadow_assert.cpp" "%HERE%shadow_cvars.cpp" "%HERE%frame_timing.cpp" "%HERE%shadow_record.cpp" "%HERE%shadow_hooks.cpp" "%HERE%test_harness.cpp" "%HERE%gpu_census.cpp" "%HERE%gpu_sync.cpp" "%HERE%game_pace.cpp" "%HERE%game_thread_sampler.cpp" "%SRC%\game_cvars.cpp" "%SRC%\game_patches.cpp" "%SRC%\main.cpp"

set RC=0
echo == clang-cl /Zs (syntax only, C++23, /MD /EHsc)
for %%F in (%FILES%) do (
  "%LLVM_BIN%\clang-cl.exe" /Zs /nologo /std:c++latest /MD /EHsc -march=x86-64-v2 -msse4.1 %DEFS% %INC% %%F
  if errorlevel 1 set RC=1
)

echo == clang++ -fsyntax-only -std=c++23 (the driver the game build uses), -Wall -Wextra
for %%F in (%FILES%) do (
  "%LLVM_BIN%\clang++.exe" -fsyntax-only -std=c++23 -Wall -Wextra -Wno-unused-parameter -D_DLL -D_MT -march=x86-64-v2 -msse4.1 %DEFS% %INC% %%F
  if errorlevel 1 set RC=1
)

if "%RC%"=="0" (echo SYNTAX OK) else (echo SYNTAX ERRORS)
endlocal & exit /b %RC%
