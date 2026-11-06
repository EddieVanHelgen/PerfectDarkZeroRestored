# Performance pass batch: runs (label, mission, extra args) with the section D method (170 s, native default
# profile = scale 3, pdz_sp_120 default on). Takes C:\Games\RUN_LOCK.txt for the batch, releases it at the end.
param([string]$RunsFile, [int]$Seconds = 170)
$lock = "C:\Games\RUN_LOCK.txt"
# C:\Games\BUILD.md (2026-11-03): no measurement while a game runs, the user's play copy (perfectdarkzerorecomp.exe)
# included: wait, never close it.
while (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) { "a game is running (PDZRestored.exe or the play copy): waiting"; Start-Sleep -Seconds 30 }
if (Test-Path $lock) { "ERROR: lock held"; Get-Content $lock; exit 2 }
"claude perf pass (tu3-base) pid $PID started $(Get-Date -Format s)" | Set-Content -Encoding ascii $lock
try {
  $r = "C:\Games\PerfectDarkZeroRecomp-tu3\tools\latency\run_latency.ps1"
  $tools = "C:\Games\PDZRenderResearch\native\tools"
  $build = "C:\Games\PerfectDarkZeroRecomp-tu3\out\build\win-amd64-release-play"
  foreach ($line in Get-Content $RunsFile) {
    if (-not $line.Trim() -or $line.StartsWith("#")) { continue }
    $parts = $line.Split("|")
    $label = $parts[0].Trim(); $m = [int]$parts[1].Trim(); $extra = $parts[2].Trim()
    $script = Get-ChildItem ("$tools\mission_{0:D2}_*.script" -f $m) | Select-Object -First 1
    while (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) { "a game is running: waiting before $label"; Start-Sleep -Seconds 30 }
    "===== $label mission $m $(Get-Date -Format HH:mm:ss)"
    & powershell -ExecutionPolicy Bypass -File $r -Label $label -Script $script.FullName -Seconds $Seconds -NoLatency `
      -Extra "--pdz_frame_timing=true --pdz_game_clock_log=true $extra"
    $log = Get-ChildItem "$build\logs\PDZRestored_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
    Select-String -Path $log.FullName -Pattern "pdz_timing: scenes \(SUMMARY\)|pdz_timing: .*SUMMARY \(test exit\) swaps|pdz_clock: SUMMARY|spikes SUMMARY|pdz crash" |
      ForEach-Object { $_.Line -replace '^\[[^\]]*\] \[[^\]]*\] \[[^\]]*\] \[[^\]]*\] ', '' }
    # (No kill by name any more: perfectdarkzerorecomp.exe is the user's play copy. run_latency.ps1 stops only its
    # own run, by its process id, on a timeout.)
  }
} finally {
  Remove-Item $lock -Force
  "===== done $(Get-Date -Format HH:mm:ss)"
}
