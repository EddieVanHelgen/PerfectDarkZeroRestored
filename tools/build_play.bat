@echo off
rem The one build (2026-10-28): this repo (C:\Games\PerfectDarkZeroRecomp-tu3, branch tu3-base), preset
rem win-amd64-release-pgousev3 (x86-64-v3 + PGO with C:\Games\PDZRenderResearch\native\stage2\m5\pgo\pdz.profdata, LTO)
rem with PDZ_PGO_SCOPE=title, into out\build\win-amd64-release-play: PDZRestored.exe (the release name since 2026-11-03).
rem Only build with no PDZRestored.exe running and C:\Games\RUN_LOCK.txt absent (write it while building, delete it
rem after). A running perfectdarkzerorecomp.exe is the user's play copy (C:\Games\PDZ_PlayCopy): it doesn't block a
rem build. See C:\Games\BUILD.md.
setlocal
rem The lock is yours when it contains %PDZ_LOCK_HOLDER% (e.g. set PDZ_LOCK_HOLDER=pid 1234, the text you wrote in it).
if exist C:\Games\RUN_LOCK.txt if "%PDZ_LOCK_HOLDER%"=="" (echo C:\Games\RUN_LOCK.txt exists: another terminal holds the lock & type C:\Games\RUN_LOCK.txt & exit /b 2)
if exist C:\Games\RUN_LOCK.txt findstr /C:"%PDZ_LOCK_HOLDER%" C:\Games\RUN_LOCK.txt >nul || (echo C:\Games\RUN_LOCK.txt is held by another terminal & type C:\Games\RUN_LOCK.txt & exit /b 2)
tasklist /FI "IMAGENAME eq PDZRestored.exe" 2>nul | find /I "PDZRestored.exe" >nul && (echo PDZRestored.exe is running: build later & exit /b 3)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.." || exit /b 1
set BUILD=%CD%\out\build\win-amd64-release-play
rem PGO fallback (night of 31 Oct): "tools\build_play.bat nopgo", or no profile at the presets path, builds the same
rem x86-64-v3 + LTO play build without PGO (PDZ_PGO empty). The next plain run goes back to PGO (full rebuild either way).
rem Re-train the profile with tools\perf\pgo_train.ps1 (docs\PERFORMANCE.md, "PGO retraining").
set PGO_ARGS=
if /I "%~1"=="nopgo" set PGO_ARGS=-DPDZ_PGO=
if not exist "C:\Games\PDZRenderResearch\native\stage2\m5\pgo\pdz.profdata" (echo no PGO profile: building without PGO & set PGO_ARGS=-DPDZ_PGO=)
cmake --preset win-amd64-release-pgousev3 -B "%BUILD%" -DPDZ_PGO_SCOPE=title %PGO_ARGS% || exit /b 1
cmake --build "%BUILD%" || exit /b 1
cmake -E copy_if_different "%BUILD%\rexglue-out\rexgpu-xenos.dll" "%BUILD%\rexglue-out\rexruntime.dll" "%BUILD%" || exit /b 1
rem The exe from before the release name, left by older builds, is removed so nothing starts a stale build by mistake
rem (its logs would reuse numbers). The play copy in C:\Games\PDZ_PlayCopy is a separate copy.
if exist "%BUILD%\perfectdarkzerorecomp.exe" del "%BUILD%\perfectdarkzerorecomp.exe" "%BUILD%\perfectdarkzerorecomp.map" 2>nul
echo built: %BUILD%\PDZRestored.exe
