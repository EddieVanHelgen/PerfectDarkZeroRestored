# PGO re-training for the play build (docs/PERFORMANCE.md, "PGO procedure"; night of 30-31 Oct 2026).
#
#   powershell -ExecutionPolicy Bypass -File tools\perf\pgo_train.ps1 [-Step all|build|train|merge] [-Runs <file>]
#
# 1. build: the instrumented build out\build\win-amd64-release-pgogen (PDZ_PGO_SCOPE=title, as the play build's use
#    side), its SDK DLLs, and the play build's native_shaders / native_cache copied next to it.
# 2. train: each training run (a runs file: label | script | seconds | extra args; scripts are looked up in
#    tools\perf\bench, then C:\Games\PDZRenderResearch\native\tools unless the path is absolute) with LLVM_PROFILE_FILE pointing at
#    <profiles>\raw; the game closes itself after <seconds> (--pdz_test_exit_s) and writes its profile at close.
# 3. merge: llvm-profdata merges raw\*.profraw into pdz_new.profdata; the current pdz.profdata is kept as
#    pdz_<date>.profdata and the new one takes its place (the play build reconfigures and rebuilds by itself:
#    the profile is a configure dependency). Then run tools\build_play.bat.
# Re-train after any change to the generated code, the SDK or the hot native-renderer paths (a stale profile only
# loses the gain for the functions that changed; nothing breaks). Takes C:\Games\RUN_LOCK.txt for steps 1-2.
param(
  [ValidateSet("all", "build", "train", "merge")][string]$Step = "all",
  [string]$Runs = "",
  [string]$Profiles = "C:\Games\PDZRenderResearch\native\stage2\m5\pgo",
  # With no display active (TV off), a visible window is held to ~6 presents a second and the scripts lose their
  # presses: -Minimized starts each training run minimized (--fullscreen=false). The presenter then skips its paints,
  # so that path trains less; use it only when no display can be had (3 Nov).
  [switch]$Minimized
)
$repo = Split-Path (Split-Path $PSScriptRoot)
$gen = Join-Path $repo "out\build\win-amd64-release-pgogen"
$play = Join-Path $repo "out\build\win-amd64-release-play"
$tools = "C:\Games\PDZRenderResearch\native\tools"
$userdata = "C:\Games\PDZRenderResearch\native\testdata\userdata"
$lock = "C:\Games\RUN_LOCK.txt"
$llvm = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\llvm-profdata.exe"

function Wait-UserGame {
  # Before every game run: the user may be playing (the play copy, perfectdarkzerorecomp.exe), which would skew the
  # run. Wait; never close it.
  while (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) {
    Write-Output "a game is running (PDZRestored.exe or the play copy perfectdarkzerorecomp.exe): waiting"; Start-Sleep -Seconds 30
  }
}
function Take-Lock {
  if (Test-Path $lock) { Write-Output "ERROR: $lock exists:"; Get-Content $lock; exit 2 }
  # C:\Games\BUILD.md (2026-11-03): a running PDZRestored.exe blocks a build; the user's play copy
  # (perfectdarkzerorecomp.exe) doesn't, but a training run waits for it (Wait-UserGame).
  if (Get-Process PDZRestored -ErrorAction SilentlyContinue) { Write-Output "ERROR: PDZRestored.exe is running"; exit 3 }
  "pgo_train pid $PID $(Get-Date -Format s)" | Set-Content -Encoding ascii $lock
}

if ($Step -in @("all", "build")) {
  Take-Lock
  try {
    $bat = Join-Path $env:TEMP "pdz_pgo_gen.bat"
    @"
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "$repo" || exit /b 1
set LLVM_PROFILE_FILE=$gen\buildtool-%%m-%%p.profraw
cmake --preset win-amd64-release-pgogen -B "$gen" -DPDZ_PGO_SCOPE=title || exit /b 1
cmake --build "$gen" || exit /b 1
cmake -E copy_if_different "$gen\rexglue-out\rexgpu-xenos.dll" "$gen\rexglue-out\rexruntime.dll" "$gen" || exit /b 1
"@ | Set-Content -Encoding ascii $bat
    cmd /c $bat
    if ($LASTEXITCODE -ne 0) { Write-Output "ERROR: the instrumented build failed ($LASTEXITCODE)"; exit 1 }
    foreach ($d in @("native_shaders", "native_cache")) {
      if (-not (Test-Path (Join-Path $gen $d)) -and (Test-Path (Join-Path $play $d))) {
        Copy-Item -Recurse (Join-Path $play $d) (Join-Path $gen $d)
      }
    }
    Write-Output "built: $gen\PDZRestored.exe"
  } finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
}

if ($Step -in @("all", "train")) {
  if (-not $Runs) { Write-Output "ERROR: -Runs <file> (label | script | seconds | extra args)"; exit 1 }
  $raw = Join-Path $Profiles "raw_new"
  New-Item -ItemType Directory -Force $raw | Out-Null
  $env:LLVM_PROFILE_FILE = "$raw\pdz-%m-%p.profraw"
  Take-Lock
  try {
    foreach ($line in Get-Content $Runs) {
      if (-not $line.Trim() -or $line.StartsWith("#")) { continue }
      $p = $line.Split("|")
      $label = $p[0].Trim(); $script = $p[1].Trim(); $secs = [int]$p[2].Trim(); $extra = $p[3].Trim()
      if (-not [System.IO.Path]::IsPathRooted($script)) {
        $own = Join-Path $PSScriptRoot "bench\$script"
        $script = if (Test-Path $own) { $own } else { Join-Path $tools $script }
      }
      $args = @("--pdz_input_script=$script", "--pdz_test_exit_s=$secs", "--user_data_root=$userdata", "--pdz_latency_log=false")
      if ($extra) { $args += $extra.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
      if ($Minimized) { $args += "--fullscreen=false" }
      $style = if ($Minimized) { "Minimized" } else { "Normal" }
      Wait-UserGame
      Write-Output "===== train $label $(Get-Date -Format HH:mm:ss)"
      $proc = Start-Process -FilePath "$gen\PDZRestored.exe" -WorkingDirectory $gen -ArgumentList $args -WindowStyle $style -PassThru
      if (-not $proc.WaitForExit(($secs + 150) * 1000)) { Stop-Process -Id $proc.Id -Force; Write-Output "killed our run after $($secs + 150) s (the profile is written at close)" }
      Start-Sleep -Seconds 3
      $log = Get-ChildItem "$gen\logs\PDZRestored_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
      Select-String -Path $log.FullName -Pattern "pdz_pgo: profiles written|pdz crash" | Select-Object -First 2 | ForEach-Object { $_.Line }
    }
  } finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue }
  Write-Output ("raw profiles: {0}" -f (Get-ChildItem "$raw\*.profraw").Count)
}

if ($Step -in @("all", "merge")) {
  $raw = Join-Path $Profiles "raw_new"
  $files = (Get-ChildItem "$raw\pdz-*.profraw").FullName
  if (-not $files) { Write-Output "ERROR: no raw profiles in $raw"; exit 1 }
  $new = Join-Path $Profiles "pdz_new.profdata"
  & $llvm merge -o $new $files
  if ($LASTEXITCODE -ne 0) { Write-Output "ERROR: merge failed"; exit 1 }
  & $llvm show $new | Select-String "Total functions|Maximum function count"
  $cur = Join-Path $Profiles "pdz.profdata"
  if (Test-Path $cur) { Copy-Item $cur (Join-Path $Profiles ("pdz_{0}.profdata" -f (Get-Date -Format yyyyMMdd_HHmm))) }
  Copy-Item $new $cur -Force
  Write-Output "installed: $cur (the previous one kept beside it). Next: tools\build_play.bat"
}
