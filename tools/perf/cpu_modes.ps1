# Slow-CPU test modes for timing runs (2 Nov 2026; docs/PERF_FOR_EVERYONE.md). Dot-source it:
#   . tools\perf\cpu_modes.ps1
#   $m = Get-PdzCpuMode midrange; Enter-PdzCpuMode $m; try { ...; Set-PdzProcessMode $proc $m } finally { Exit-PdzCpuMode }
#
# This PC (Core Ultra 9 285K): 8 P-cores (LP 0-1, 10-13, 22-23, efficiency class 1) and 16 E-cores (LP 2-9, 14-21,
# class 0, clusters of 4 sharing an L2). Modes:
#   full     : no change.
#   midrange : 6 E-cores (LP 2-7), E-core maximum processor state 99 (no boost): about 0.57-0.6 of a P-core's
#              single-thread speed (tools\perf\cpu_calibrate.ps1). A Ryzen 5 3600 / Core i5-10400 class CPU, a little
#              below a Ryzen 5 5600 / i5-12400 (about 0.65-0.75): the E-cores have only two steps here (boost: 0.85;
#              no boost: 0.57 and lower), and the P-cores' own cap (PROCTHROTTLEMAX1) is a hidden power setting that
#              can't be read back for an exact restore, so it isn't used.
#   deck     : 4 E-cores of one cluster (LP 2-5: 4 cores sharing a 4 MB L2, as the Deck's 4 cores share a 4 MB L3),
#              E-core maximum processor state 80: about 0.46 of a P-core (a Steam Deck's Zen 2 at 3.5 GHz is about
#              0.42-0.47). 4 logical processors (the Deck has 4 cores / 8 threads: SMT not modelled, so slightly
#              pessimistic on thread count; the GPU and memory are this PC's).
# The power-plan change (PROCTHROTTLEMIN to 5 and PROCTHROTTLEMAX to the mode's cap, AC values of the active scheme)
# applies only to the E-cores, only for one run, and is always restored: Exit-PdzCpuMode in a finally, plus a
# restore file (logs\cpu_mode_restore.txt) that the next Enter / Restore-PdzCpuModeLeftover applies first if a run
# was killed. The game's process affinity is set after launch and re-applied while it runs: the SDK resets its
# process affinity to all processors at startup (rex::thread::EnableAffinityConfiguration).

$script:PdzRestoreFile = Join-Path (Split-Path (Split-Path $PSScriptRoot)) "out\build\win-amd64-release-play\logs\cpu_mode_restore.txt"

function Get-PdzPowerAc([string]$setting) {
  $line = powercfg /q SCHEME_CURRENT SUB_PROCESSOR $setting | Select-String "Current AC Power Setting Index"
  if (-not $line) { throw "powercfg: $setting not readable" }
  return [Convert]::ToInt32($line.ToString().Split(":")[1].Trim(), 16)
}

function Set-PdzPowerAc([int]$min, [int]$max) {
  powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN $min | Out-Null
  powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMAX $max | Out-Null
  powercfg /setactive SCHEME_CURRENT | Out-Null
}

function Get-PdzCpuMode([string]$name) {
  switch ($name) {
    "full" { return @{ Name = "full"; Mask = 0; Cap = 0 } }
    "midrange" { return @{ Name = "midrange"; Mask = 0xFC; Cap = 99 } }   # LP 2-7
    "deck" { return @{ Name = "deck"; Mask = 0x3C; Cap = 80 } }           # LP 2-5
    default { throw "unknown CPU mode '$name' (full, midrange, deck)" }
  }
}

function Restore-PdzCpuModeLeftover {
  if (Test-Path $script:PdzRestoreFile) {
    $v = (Get-Content $script:PdzRestoreFile -Raw).Trim().Split(" ")
    Set-PdzPowerAc ([int]$v[0]) ([int]$v[1])
    Remove-Item $script:PdzRestoreFile -Force
    "cpu_mode: restored a leftover power plan change: min $($v[0]) max $($v[1]) (now min $(Get-PdzPowerAc PROCTHROTTLEMIN) max $(Get-PdzPowerAc PROCTHROTTLEMAX))"
  }
}

function Enter-PdzCpuMode($mode) {
  Restore-PdzCpuModeLeftover
  $script:PdzSaved = $null
  if ($mode.Cap -gt 0) {
    $min = Get-PdzPowerAc PROCTHROTTLEMIN; $max = Get-PdzPowerAc PROCTHROTTLEMAX
    New-Item -ItemType Directory -Force (Split-Path $script:PdzRestoreFile) | Out-Null
    "$min $max" | Set-Content -Encoding ascii $script:PdzRestoreFile
    $script:PdzSaved = @($min, $max)
    Set-PdzPowerAc 5 $mode.Cap
    "cpu_mode $($mode.Name): E-core maximum processor state $($mode.Cap) (was min $min max $max), affinity 0x$('{0:X}' -f $mode.Mask)"
  } elseif ($mode.Mask) {
    "cpu_mode $($mode.Name): affinity 0x$('{0:X}' -f $mode.Mask)"
  }
}

function Exit-PdzCpuMode {
  if ($script:PdzSaved) {
    Set-PdzPowerAc $script:PdzSaved[0] $script:PdzSaved[1]
    Remove-Item $script:PdzRestoreFile -Force -ErrorAction SilentlyContinue
    "cpu_mode: power plan restored: min $(Get-PdzPowerAc PROCTHROTTLEMIN) max $(Get-PdzPowerAc PROCTHROTTLEMAX)"
    $script:PdzSaved = $null
  }
}

# Sets (or re-sets) the game process's affinity; returns $true when it had to be changed.
function Set-PdzProcessMode($proc, $mode) {
  if (-not $mode.Mask -or $proc.HasExited) { return $false }
  try {
    $proc.Refresh()
    if ([int64]$proc.ProcessorAffinity -ne $mode.Mask) { $proc.ProcessorAffinity = [IntPtr][int64]$mode.Mask; return $true }
  } catch { }
  return $false
}
