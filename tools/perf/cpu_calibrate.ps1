# Calibrates the slow-CPU modes (tools\perf\cpu_modes.ps1) with a single-thread benchmark (tools\perf\stbench.cpp):
# the benchmark on one logical processor of each mode, its time against full speed (a P-core, no cap).
#   powershell -ExecutionPolicy Bypass -File tools\perf\cpu_calibrate.ps1 [-Out <folder>]
# Builds stbench.exe into <Out> with the Visual Studio clang. The power plan is restored after each mode.
param([string]$Out = "")
. (Join-Path $PSScriptRoot "cpu_modes.ps1")
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Out) { $Out = Join-Path $repo "out\build\win-amd64-release-play\logs\perf_everyone" }
New-Item -ItemType Directory -Force $Out | Out-Null
$clang = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\Llvm\x64\bin\clang++.exe" | Select-Object -First 1
$exe = Join-Path $Out "stbench.exe"
& $clang.FullName -O2 -march=x86-64-v3 (Join-Path $PSScriptRoot "stbench.cpp") -o $exe
if ($LASTEXITCODE -ne 0) { "ERROR: stbench build failed"; exit 1 }
Restore-PdzCpuModeLeftover
$results = @{}
foreach ($name in "full", "midrange", "deck") {
  $mode = Get-PdzCpuMode $name
  # The mode's first logical processor (full: LP 0, a P-core).
  $lp = 0; if ($mode.Mask) { while (-not ($mode.Mask -band (1 -shl $lp))) { $lp++ } }
  Enter-PdzCpuMode $mode
  try {
    Start-Sleep -Milliseconds 700
    $c = [double](& $exe $lp compute); $m = [double](& $exe $lp memory)
  } finally { Exit-PdzCpuMode }
  $results[$name] = @($c, $m)
}
"power plan now: min $(Get-PdzPowerAc PROCTHROTTLEMIN) max $(Get-PdzPowerAc PROCTHROTTLEMAX)"
foreach ($name in "full", "midrange", "deck") {
  $r = $results[$name]
  "{0,-9} compute {1,7:F1} ms ({2:F2} of full)   memory {3,7:F1} ms ({4:F2} of full)" -f $name, $r[0], ($results.full[0] / $r[0]), $r[1], ($results.full[1] / $r[1])
}
