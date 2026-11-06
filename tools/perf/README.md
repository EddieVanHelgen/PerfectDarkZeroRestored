# tools/perf (performance pass, 2026-10-29 / 30)

- `perf_batch.ps1 -RunsFile <file> [-Seconds 170]`: unattended mission runs (section D method: the PDZRenderResearch
  mission scripts through tools\latency\run_latency.ps1 -NoLatency, frame timing and the game clock log on). Takes
  C:\Games\RUN_LOCK.txt for the batch, closes the game after each run. Runs file lines: `label | mission | extra args`.
- `spike_classify.py <log>...`: every level spike line over 12.5 ms (needs `--pdz_frame_timing_phases=1
  --pdz_frame_timing_spike_ms=12 --pdz_frame_timing_spike_level_only=true`) by its largest cause.
- `level_split.py <log>...`: the level-only 600-frame windows averaged: frame, game thread split, native phases,
  record thread, GPU, and the `pdz_native_gpu_passes` kinds.
