// Multiplayer match music: the player's own MP3s (pdz_mp_music_custom_dir), played by the host over the game's sound
// (rex/audio/host_stream.h). Used by mp_music.cpp; see mp_music_custom.cpp.
#pragma once

#include <string>

namespace mp_music_custom
{
  // The files found (scanned once, at the first call; 0 with the cvar empty or the folder missing).
  int Count();
  // "REMIX: <TITLE>", cut to fit the Music row.
  const std::string &MenuName(int index);
  // What mp_music.txt keeps for it ("REMIX FILE:<file name>"), and back (-1 = not one of ours).
  std::string SaveName(int index);
  int FromSaveName(const std::string &name);
  // False once its measure failed (Random leaves it out).
  bool Playable(int index);

  // Starts a file (loop: a chosen remix, fading out at its end and back in; else Random's one play-through). The
  // first time a file is measured (a worker thread) it waits for that. Never decodes on the caller's thread.
  void Start(int index, bool loop);
  void FadeOut();
  void Stop();
  enum class State
  {
    Idle,     // nothing started
    Waiting,  // measuring the file first
    Playing,
    Finished, // played through (Random moves on)
    Failed,   // couldn't be read or decoded: skipped (logged)
  };
  // The game thread, every music tick: starts a waiting file once measured, follows the volume. Cheap.
  State Tick();
  int Playing(); // index, -1 = none
} // namespace mp_music_custom
