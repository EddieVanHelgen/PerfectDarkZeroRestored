@echo off
rem Syntax check of the native pipeline / shader cache / draw files against this worktree, with the include set
rem and defines of the game's own compile rule (out\build\win-amd64-release\build.ninja, target
rem perfectdarkzerorecomp: clang++ driver, -std=c++23). Nothing is written (no objects, no PCH).
rem
rem   check_syntax_pipeline.bat [WORKTREE]   default C:\Games\PerfectDarkZeroRecomp-render
rem   set LLVM_BIN=...                        default C:\Program Files\LLVM\bin
rem   set SDK_OVERLAY=...                     an SDK checkout whose include\ is searched before WORKTREE's SDK (a local
rem                                           SDK commit not yet in that worktree's thirdparty\rexglue-sdk)
rem This worktree's own src\ (the one holding this file) is searched before WORKTREE's, so a check from another
rem worktree sees its own headers; WORKTREE still provides generated\default and the build folder.
rem
rem Adapted from C:\Games\PDZRenderResearch\native\src\shadow\check_syntax.bat (stage 0).
setlocal
set "HERE=%~dp0"
set "SRC=%~dp0..\..\.."
set "PDZ=%~1"
if "%PDZ%"=="" set "PDZ=C:\Games\PerfectDarkZeroRecomp-render"
if "%LLVM_BIN%"=="" set "LLVM_BIN=C:\Program Files\LLVM\bin"
set "SDK=%PDZ%\thirdparty\rexglue-sdk"
set "OUT=%PDZ%\out\build\win-amd64-release"
set "OVL="
if not "%SDK_OVERLAY%"=="" set OVL=-I"%SDK_OVERLAY%\include"

set INC=%OVL% -I"%PDZ%" -I"%SRC%" -I"%PDZ%\src" -I"%PDZ%\generated\default" -I"%SDK%\include" -I"%OUT%\rexglue-sdk\include" -I"%SDK%\thirdparty\imgui" -I"%SDK%\thirdparty\xxHash" -I"%SDK%\thirdparty\simde" -I"%SDK%\thirdparty\simde\simde" -I"%SDK%\thirdparty\fmt\include" -I"%SDK%\thirdparty\spdlog\include" -I"%OUT%\rexglue-sdk\thirdparty\sdl3\include-revision" -I"%SDK%\thirdparty\sdl3\include" -I"%SDK%\thirdparty\dxc\include" -I"%SDK%\thirdparty\vulkan-headers\include" -I"%SDK%\thirdparty\vulkan-memory-allocator\include" -isystem "%SDK%\thirdparty\tomlplusplus\include" -isystem "%SDK%\thirdparty\spirv-tools\include"
set DEFS=-DNOMINMAX -DPDZ_NO_MODS=1 -DREXGLUE_BUILD_CONFIG=\"Release\" -DREX_HAS_D3D12=1 -DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DWIN32_LEAN_AND_MEAN
set FILES="%HERE%native_shaders.cpp" "%HERE%native_pipeline.cpp" "%HERE%native_depth20e4.cpp" "%HERE%native_coissue.cpp" "%HERE%native_blend7e3.cpp" "%HERE%native_draw.cpp" "%HERE%native_drawdump.cpp"

set RC=0
echo == clang++ -fsyntax-only -std=c++23 (the driver the game build uses)
for %%F in (%FILES%) do (
  echo    %%~nxF
  "%LLVM_BIN%\clang++.exe" -fsyntax-only -std=c++23 -D_DLL -D_MT -march=x86-64-v2 -msse4.1 -Wall -Wno-unused-function %DEFS% %INC% %%F
  if errorlevel 1 set RC=1
)

if "%RC%"=="0" (echo SYNTAX OK) else (echo SYNTAX ERRORS)
endlocal & exit /b %RC%
