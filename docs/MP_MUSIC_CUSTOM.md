# Your own music in multiplayer matches (2026-11-04)

`pdz_mp_music_custom_dir` (default empty = off) names a folder of your own MP3 files. With it set:

- **Music row:** each file is added after the game's tracks (MULTIPLAYER setup screen, MUSIC) as `REMIX: <TITLE>`.
  - The title is the file's ID3 title, or its file name when there is no title or the tag is a web address (as
    download sites leave). A leading track number ("3-") is dropped, and so is a last word "REMIX" (the prefix says it).
  - Names are upper case, cut to 22 characters (the longest of the game's track names, LABORATORY CONTAINMENT). Names
    that would come out the same lose the words they share at the start: "DATADYNE CENTRAL MIX ONE" ->
    `REMIX: MIX ONE`.
  - Files are listed in the number order of their names.
- **Random** draws from the game's 31 level tracks and your files alike.
- **Playback:** the host plays the file, not the game's sound engine.
  - It is decoded ahead on a thread of its own with the SDK's FFmpeg (mp3float, the MPEG audio parser) and mixed into
    the host audio output after the game's sound, over the map's ambience: front left / right, under the output gain
    and `audio_mute` (SDK `rex/audio/host_stream.h`).
  - It fades in over 2 s and out over 3 s before its end. A chosen file loops through those fades; Random plays it
    once, then picks again.
  - The game's own tracks are unchanged.
- **Loudness:** each file is decoded once on a worker thread and measured: gated loudness (BS.1770 gating, no K filter)
  and peak.
  - Its gain brings it to -9.84 dB, the median of the game's 31 Random tracks measured the same way from their wave
    banks (16-bit PCM; -14.8 to -8.1 dB).
  - The gain stays within -24 / +6 dB and never lifts the peak above -0.3 dBFS.
  - The result is kept in `mp_music_custom.txt` in the user data folder (file name, size, time), so each file is
    measured once.
  - `pdz_mp_music_custom_volume_db` (default 0, -30 to +12) adds a trim on top and is followed live.
- **Volume:** the game has no music volume setting of its own. None of its 89 DARKVALUE settings is a volume, and Rare
  never sets the XACT music category's volume. So the file follows the host's output gain, mute and
  the trim cvar.
- **Multiplayer only:** files start only where the Music row's tracks do (game mode 2 / 3). They stop, and the stream
  and its decoder are freed, when the level is left or its audio torn down.
- **Bad files:** a file that can't be read or decoded is skipped with a log line (`pdz_mp_music: skipped ...`).
  Random leaves it out from then on; chosen, it plays nothing.
  - Damage: an MP3 frame the decoder rejects, or one that decodes past +3.5 dBFS (a damaged frame can decode to
    +35 dBFS of noise), is dropped.
  - A file with more than 10 % of its frames damaged counts as bad.
- **Shipping:** no music file is in the repo or the release. `stage_release.py` refuses any audio file and anything
  under `MyMusic`.

Example (`settings\hardware.toml`, under `[Gameplay]` or anywhere):

```toml
pdz_mp_music_custom_dir = "C:\\Games\\MyMusic"
```

Logs (`pdz_music_log = true` for the track changes):
- the files found;
- each file's measure (`REMIX: ... = <file>: 242.0 s, loudness ... dB (the game's tracks -9.84), peak ..., gain ...`);
- `playing ...`, `stopped ...`, `skipped ...`.

## Tests (2026-11-04)

A fresh release folder, Deathmatch on Urban with bots, `pdz_music_log`, a user data folder per run:
- **Chosen one of the `REMIX: ...` entries, 4-minute match:**
  - The 8 files were measured in 1.5 s on the worker, while the setup screen was open: loudness -10.9 to -9.1 dB,
    gains -0.7 to +0.5 dB.
  - The file started 4.5 s after the match start. Its second pass began 173.8 s later (the file is 173.7 s long).
  - It stopped at the match end. One 5 ms output block was not ready, at the very start.
- **Not audio** (a text file named `.mp3`): skipped at its measure ("no audio decoded"); chosen, the match had no
  music; no crash.
- **Damaged** (every third byte overwritten after the first 200 KB):
  - The first build played it as noise peaking at +35 dBFS.
  - Fixed: frames that decode past +3.5 dBFS are dropped, and a file with over 10 % bad frames is refused. Now it
    is skipped: "damaged: 5023 of 5269 MP3 frames bad". Chosen, the match had no music.
- **Lightly damaged** (40 bytes overwritten): it plays, and its measure matches the clean file (174.4 s, -10.05 dB);
  0 output blocks missed.
- **Datacore** (a fresh release folder, this build): 0 pipelines built during play; gate MAE 1.53.
