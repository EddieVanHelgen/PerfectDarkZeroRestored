// PDZ Restored: a small native window shown before the game starts (first-run setup, 2026-11-03). Built to be reused by
// the settings launcher: a title, two lines of text, a progress bar and up to two buttons, driven from a worker thread.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pdz::setup_ui
{
  // The window's state, written by the worker and shown by the window (a timer copies it to the controls).
  struct State
  {
    std::mutex mutex;
    std::wstring line1, line2;
    double progress = 0.0;   // 0..1; below 0: busy without a known amount
    bool done = false;       // the worker finished: the window closes
    std::atomic<bool> cancel{false}; // the Cancel button (or closing the window) asks the worker to stop
  };

  // Shows the window and runs `work` on a worker thread until it returns; the window closes then. Returns false if the
  // window couldn't be created (the work still runs, without a window).
  bool RunWithProgress(const std::wstring &title, State &state, const std::function<void(State &)> &work);

  // Our message boxes (theme.h), with the same text and buttons as Windows' ones.
  int Error(const std::wstring &title, const std::wstring &text); // MessageBoxW result (0: not shown)
  void Info(const std::wstring &title, const std::wstring &text);
  bool AskOkCancel(const std::wstring &title, const std::wstring &text);

  // Test aid (pdz_setup_test_capture): the progress window with a sample step, then a question with this text, each
  // written as a .bmp into `folder` (setup_progress.bmp, setup_message.bmp).
  void TestCapture(const std::filesystem::path &folder, const std::wstring &title, const std::wstring &message_title,
                   const std::wstring &message);

  // A file picker. `filters` is (name, pattern) pairs, e.g. {L"Disc image", L"*.iso;default.xex"}. Returns false when
  // the player cancels.
  bool PickFile(const std::wstring &title, const std::vector<std::pair<std::wstring, std::wstring>> &filters,
                std::filesystem::path &out);
} // namespace pdz::setup_ui
