@echo off
rem Builds one compiler-experiment variant (docs\PERFORMANCE.md, "Compiler experiments") in its own build folder,
rem out\build\<preset>, next to the default out\build\win-amd64-release (which it never touches).
rem
rem   tools\build_variant.bat <preset> [configure] [source folder]
rem
rem   preset:   win-amd64-release-base | -v3 | -lto | -v3lto | -nolto | -pgogen | -pgouse | -pgousev3   (CMakePresets.json)
rem   configure: re-run the configure step (it also runs when the folder has no build.ninja yet)
rem   source folder: the checkout to build (default: this script's repo, e.g. C:\Games\PerfectDarkZeroRecomp-render
rem             after the tu3-compiler commit is merged there)
rem
rem The variant presets set PDZ_SDK_OUT_IN_BUILD: the SDK's DLLs, libraries and rexglue.exe go to
rem out\build\<preset>\rexglue-out instead of the shared thirdparty\rexglue-sdk\out\win-amd64 that the default build
rem (and build_render.bat's copy) uses.
setlocal
set PRESET=%~1
if "%PRESET%"=="" (
  echo usage: tools\build_variant.bat ^<preset^> [configure] [source folder]
  exit /b 1
)
set SRC=%~3
if "%SRC%"=="" set SRC=%~dp0..
@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%SRC%" || exit /b 1
set BUILD=%CD%\out\build\%PRESET%
rem pgogen instruments everything built with the SDK, the codegen tool rexglue.exe included, which runs during the
rem build: keep the .profraw files it writes at exit in the build folder (not the source tree, not the training set).
set LLVM_PROFILE_FILE=%BUILD%\buildtool-%%m-%%p.profraw
if "%~2"=="configure" (
  cmake --preset %PRESET% || exit /b 1
) else if not exist "%BUILD%\build.ninja" (
  cmake --preset %PRESET% || exit /b 1
)
cmake --build --preset %PRESET% || exit /b 1
rem As build_render.bat: the exe's POST_BUILD copy of the SDK DLLs only runs when the exe relinks, so copy them
rem after every build (from this variant's own SDK output folder).
cmake -E copy_if_different "%BUILD%\rexglue-out\rexgpu-xenos.dll" "%BUILD%\rexglue-out\rexruntime.dll" "%BUILD%" || exit /b 1
rem The native renderer's translated-shader cache lives next to the exe: start from the default build's, so the first
rem timed run does not translate shaders (it is a cache: never committed, safe to copy).
if not exist "%BUILD%\native_shaders" if exist "%CD%\out\build\win-amd64-release\native_shaders" (
  xcopy /e /i /q /y "%CD%\out\build\win-amd64-release\native_shaders" "%BUILD%\native_shaders" >nul
)
echo built %PRESET%: %BUILD%\PDZRestored.exe
