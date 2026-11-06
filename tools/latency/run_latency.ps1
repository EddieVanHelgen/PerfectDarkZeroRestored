# Unattended latency run (pdz_latency_log): starts the play build like the bare exe (the native default profile and
# settings\hardware.toml apply), feeds an input script, closes the game after -Seconds and prints the latency report.
# The caller holds C:\Games\RUN_LOCK.txt and makes sure no other game runs.
#
#   powershell -File tools\latency\run_latency.ps1 -Label dc60 [-Script latency_datacore.script] [-Seconds 200]
#                                                  [-Extra "--pdz_null_refresh_hz=120 ..."] [-NoLatency]
param(
  [Parameter(Mandatory = $true)][string]$Label,
  [string]$Script = "latency_datacore.script",
  [int]$Seconds = 200,
  [string]$Extra = "",
  [switch]$NoLatency  # performance runs: no pdz_latency_log (and so no frame_pacing trace)
)
$repo = Split-Path (Split-Path $PSScriptRoot)
$build = Join-Path $repo "out\build\win-amd64-release-play"
if (-not (Test-Path $Script)) { $Script = Join-Path $PSScriptRoot $Script }
$Script = (Resolve-Path $Script).Path
$userdata = "C:\Games\PDZRenderResearch\native\testdata\userdata" # a throwaway copy of the save data
$args = @("--pdz_latency_log=$(if ($NoLatency) { 'false' } else { 'true' })", "--pdz_input_script=$Script", "--pdz_test_exit_s=$Seconds",
          "--user_data_root=$userdata")
if ($Extra) { $args += $Extra.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
# C:\Games\BUILD.md (2026-11-03): wait while a game runs, the user's play copy (perfectdarkzerorecomp.exe) included:
# it would skew the run. Never close it.
while (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) {
  Write-Output "a game is running (PDZRestored.exe or the play copy perfectdarkzerorecomp.exe): waiting"; Start-Sleep -Seconds 30
}
$before = Get-ChildItem "$build\logs\PDZRestored_*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
$p = Start-Process -FilePath "$build\PDZRestored.exe" -WorkingDirectory $build -ArgumentList $args -PassThru
if (-not $p.WaitForExit(($Seconds + 90) * 1000)) {
  Stop-Process -Id $p.Id -Force
  Write-Output "TIMEOUT: the game did not exit within $($Seconds + 90) s (killed)"
}
$log = Get-ChildItem "$build\logs\PDZRestored_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
if ($before -and $log.FullName -eq $before.FullName) { Write-Output "WARNING: no new log file" }
Write-Output "$Label -> $($log.Name) (exit $($p.ExitCode)) args: $Extra"
$csv = Join-Path $build "logs\frame_pacing.csv"
if (-not $NoLatency -and (Test-Path $csv)) { Copy-Item $csv (Join-Path $build ("logs\frame_pacing_{0}.csv" -f $log.BaseName)) -Force }
& python -I (Join-Path $PSScriptRoot "latency_report.py") $log.FullName
