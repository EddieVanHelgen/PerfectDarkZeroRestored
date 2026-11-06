# Scripted timing / profiling runs on the play build (31 Oct 2026; docs/RARE_CODE_PROFILE.md).
#
#   powershell -ExecutionPolicy Bypass -File tools\perf\bench_run.ps1 -RunsFile <file> -Tag <tag> [-Out <folder>]
#
# Runs file lines: label | script | seconds | extra args. Scripts are looked up in tools\perf\bench unless the path is
# absolute. Each run: the play exe with --pdz_input_script, --pdz_test_exit_s=<seconds>, the research test user data,
# frame timing on, plus the extra args (settings\hardware.toml is read as usual and never written; put overrides on
# the line). Takes C:\Games\RUN_LOCK.txt for the whole batch and releases it at the end; waits (never kills) when the
# game is already running. The run's log and any guest_ip_profile_<n>.txt are copied to <Out>\<tag>_<label>.*; the
# level windows' averages are printed with tools\perf\level_split.py.
#
# -CpuMode full | midrange | deck (2 Nov; tools\perf\cpu_modes.ps1, docs/PERF_FOR_EVERYONE.md): a slower CPU for the
# run (E-cores only by process affinity, plus a temporary E-core maximum processor state). Applied per run and always
# restored (finally, plus a restore file for a killed batch); each run's restore line is printed. A runs-file line can
# override it with a 5th field (label | script | seconds | extra args | mode).
param(
  [Parameter(Mandatory = $true)][string]$RunsFile,
  [Parameter(Mandatory = $true)][string]$Tag,
  [string]$Out = "",
  [string]$Build = "",
  # Start the game minimized (with --fullscreen=false on the line): the presenter then skips its paints, so a run
  # with no display active (TV off: Windows holds presents back to about 6 a second) still gets past the menus. Not
  # for timing: a minimized game runs as a background app (31 Oct: record thread 12-14 ms, Swap blocked ~9 ms).
  [switch]$Minimized,
  [ValidateSet("full", "midrange", "deck")][string]$CpuMode = "full",
  # Timing runs: skip a run (no launch) when nvidia-smi reports no active display, checked before the launch; after
  # the run, a log without "display reports 120 Hz" is flagged INVALID FOR TIMING.
  [switch]$RequireDisplay
)
. (Join-Path $PSScriptRoot "cpu_modes.ps1")
Restore-PdzCpuModeLeftover
$repo = Split-Path (Split-Path $PSScriptRoot)
if (-not $Build) { $Build = Join-Path $repo "out\build\win-amd64-release-play" }
if (-not $Out) { $Out = Join-Path $Build "logs\bench" }
$lock = "C:\Games\RUN_LOCK.txt"
$userdata = "C:\Games\PDZRenderResearch\native\testdata\userdata"
New-Item -ItemType Directory -Force $Out | Out-Null

# C:\Games\BUILD.md (2026-11-03): wait while a game runs, the user's play copy (perfectdarkzerorecomp.exe) included;
# never close it.
while (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) { "a game is running (PDZRestored.exe or the play copy): waiting"; Start-Sleep -Seconds 30 }
if (Test-Path $lock) { "ERROR: $lock is held:"; Get-Content $lock; exit 2 }
"bench_run pid $PID $Tag $(Get-Date -Format s)" | Set-Content -Encoding ascii $lock
try {
  # The build's linker map beside the outputs: ip_profile_report.py names exe addresses from it (a rebuild moves them).
  $map = Join-Path $Build "PDZRestored.map"
  if (Test-Path $map) { Copy-Item $map (Join-Path $Out ("{0}_PDZRestored.map" -f $Tag)) -Force }
  foreach ($line in Get-Content $RunsFile) {
    if (-not $line.Trim() -or $line.TrimStart().StartsWith("#")) { continue }
    $p = $line.Split("|")
    $label = $p[0].Trim(); $script = $p[1].Trim(); $secs = [int]$p[2].Trim(); $extra = if ($p.Count -gt 3) { $p[3].Trim() } else { "" }
    $modeName = if ($p.Count -gt 4 -and $p[4].Trim()) { $p[4].Trim() } else { $CpuMode }
    $mode = Get-PdzCpuMode $modeName
    if (-not [System.IO.Path]::IsPathRooted($script)) { $script = Join-Path $PSScriptRoot "bench\$script" }
    if (Get-Process PDZRestored, perfectdarkzerorecomp -ErrorAction SilentlyContinue) { "ERROR: a game started meanwhile (PDZRestored.exe or the play copy); stopping the batch"; break }
    # A locked PC (LogonUI.exe up: the lock screen) runs the game as a background app: 31 Oct 11:35-11:59 every build,
    # the unchanged one too, ran 0.5-1 ms slower on the record thread and 10 swaps/s short. Wait for the unlock.
    while (Get-Process LogonUI -ErrorAction SilentlyContinue) { "the PC is locked (lock screen): waiting"; Start-Sleep -Seconds 30 }
    "===== $Tag $label ($modeName) $(Get-Date -Format HH:mm:ss)"
    $before = Get-ChildItem "$Build\logs\PDZRestored_*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    $args = @("--pdz_input_script=$script", "--pdz_test_exit_s=$secs", "--user_data_root=$userdata",
              "--pdz_latency_log=false", "--pdz_game_clock_log=true", "--pdz_frame_timing=true")
    if ($extra) { $args += $extra.Split(" ", [StringSplitOptions]::RemoveEmptyEntries) }
    $style = if ($Minimized) { "Minimized" } else { "Normal" }
    if ($RequireDisplay) {
      $disp = (& nvidia-smi --query-gpu=display_active --format=csv,noheader 2>$null | Select-Object -First 1)
      if ("$disp".Trim() -ne "Enabled") { "NO DISPLAY (nvidia-smi display_active '$disp'): $label skipped, not launched"; continue }
    }
    Enter-PdzCpuMode $mode
    try {
      $proc = Start-Process -FilePath "$Build\PDZRestored.exe" -WorkingDirectory $Build -ArgumentList $args -WindowStyle $style -PassThru
      [void](Set-PdzProcessMode $proc $mode)
      # The SDK resets the process affinity at startup: re-apply it until the run ends.
      $deadline = (Get-Date).AddSeconds($secs + 120); $resets = 0
      while (-not $proc.WaitForExit(1000)) {
        if (Set-PdzProcessMode $proc $mode) { $resets++ }
        if ((Get-Date) -gt $deadline) { Stop-Process -Id $proc.Id -Force; "TIMEOUT: our run killed after $($secs + 120) s"; break }
      }
      if ($mode.Mask) { "cpu_mode $modeName`: affinity applied $resets time(s) during the run" }
    } finally {
      Exit-PdzCpuMode
    }
    Start-Sleep -Seconds 3
    $log = Get-ChildItem "$Build\logs\PDZRestored_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($before -and $log.FullName -eq $before.FullName) { "WARNING: no new log file" }
    $dest = Join-Path $Out ("{0}_{1}.log" -f $Tag, $label)
    Copy-Item $log.FullName $dest -Force
    $n = $log.BaseName.Split("_")[-1]
    $prof = Join-Path $Build "logs\guest_ip_profile_$n.txt"
    if (Test-Path $prof) { Move-Item $prof (Join-Path $Out ("{0}_{1}.profile.txt" -f $Tag, $label)) -Force }
    "$label -> $($log.Name) (exit $($proc.ExitCode))"
    if ($RequireDisplay -and -not (Select-String -Path $dest -Pattern "display reports 120 Hz" -Quiet)) { "INVALID FOR TIMING: $label has no 'display reports 120 Hz' line" }
    Select-String -Path $log.FullName -Pattern "pdz crash|Unhandled guest|pdz_guest_ip_profile: " | Select-Object -First 3 | ForEach-Object { $_.Line }
    & python -I (Join-Path $PSScriptRoot "level_split.py") $dest
  }
} finally {
  Remove-Item $lock -Force -ErrorAction SilentlyContinue
  "===== done $(Get-Date -Format HH:mm:ss)"
}
