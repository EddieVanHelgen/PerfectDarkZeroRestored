@call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d C:\Games\PerfectDarkZeroRecomp-tu3
if "%1"=="configure" cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
