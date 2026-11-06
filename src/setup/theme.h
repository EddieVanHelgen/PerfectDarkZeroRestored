// PDZ Restored: the look of our own windows (settings window, first-run setup), 2026-11-04. Colours, fonts and the
// drawing of buttons, segments, toggles, progress bars and the title, plus our own message box and controller
// navigation. Plain Win32 / GDI: every window stays a normal window with normal keyboard focus.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace pdz::theme
{
  // The palette (the agreed design).
  constexpr COLORREF kBackground = RGB(0x05, 0x0B, 0x1E); // window
  constexpr COLORREF kPanel = RGB(0x0E, 0x1C, 0x44);      // controls, panels
  constexpr COLORREF kPanelHot = RGB(0x14, 0x27, 0x5A);   // hover, the selected tab
  constexpr COLORREF kSideBar = RGB(0x08, 0x12, 0x30);    // side bar, help strip, footer
  constexpr COLORREF kTitleBar = RGB(0x03, 0x07, 0x14);
  constexpr COLORREF kBorder = RGB(0x23, 0x40, 0x7F);
  constexpr COLORREF kText = RGB(0xE4, 0xEA, 0xF4);
  constexpr COLORREF kTextDim = RGB(0xA2, 0xB1, 0xCF);
  constexpr COLORREF kBlue = RGB(0x1F, 0x5F, 0xE0); // selected options, toggles, progress
  constexpr COLORREF kCyan = RGB(0x5F, 0xD3, 0xFF); // the selected tab's marker, info notes, the info icon
  constexpr COLORREF kOrange = RGB(0xFF, 0x8A, 0x50); // warnings, errors
  constexpr COLORREF kTitleOrange = RGB(0xFF, 0x6A, 0x2A); // "RESTORED"
  constexpr COLORREF kNavyText = RGB(0x06, 0x10, 0x26);    // on the orange and silver buttons
  constexpr COLORREF kDisabledText = RGB(0x8A, 0x99, 0xB8);
  constexpr COLORREF kDisabledPanel = RGB(0x0A, 0x15, 0x34);

  enum class ButtonStyle
  {
    kPrimary,   // orange (Play, a question's main answer)
    kSecondary, // brushed silver (Safe mode, Cancel)
    kPanel,     // a plain control
    kDanger,    // orange text and border (Remove, Reset)
    kAccent,    // electric blue (Add...)
    kTab,       // side bar tab
  };

  struct Fonts
  {
    HFONT body = nullptr, body_bold = nullptr, note = nullptr, display = nullptr, display_small = nullptr,
          button = nullptr, title = nullptr, title_side = nullptr; // title_side: the settings side bar's
  };

  // Loads Rajdhani from the exe's resources once (a private font) and makes the fonts for a DPI.
  Fonts MakeFonts(int dpi);
  void FreeFonts(Fonts &fonts);

  HBRUSH Brush(COLORREF colour); // cached, never freed

  // Hover tracking for our owner-drawn buttons (a subclass); IsHot: the mouse is over it.
  void EnableHover(HWND button);
  bool IsHot(HWND button);

  // The window's title bar in the theme (Windows 11; dark mode on Windows 10).
  void ThemeTitleBar(HWND window);

  void FillRect(HDC dc, const RECT &r, COLORREF colour);
  void FillRound(HDC dc, const RECT &r, int radius, COLORREF fill, COLORREF border);
  // Text in a rectangle; `flags` as DrawTextW. `spacing` adds letter spacing (upper-case labels), in pixels.
  void Text(HDC dc, const std::wstring &text, RECT r, HFONT font, COLORREF colour, UINT flags, int spacing = 0);

  // A button face. `selected`: a chosen segment / toggle on / the current tab; `hot`: under the mouse; `focus`: has
  // the keyboard focus (a cyan ring).
  void DrawButton(HDC dc, const RECT &r, ButtonStyle style, const std::wstring &text, const Fonts &fonts, int dpi,
                  bool selected, bool hot, bool pressed, bool focus, bool disabled);
  // One segment of a segmented choice: `first` / `last` round the outer corners.
  void DrawSegment(HDC dc, const RECT &r, const std::wstring &text, const Fonts &fonts, int dpi, bool selected,
                   bool hot, bool focus, bool disabled, bool first, bool last);
  // A switch with its label to the right.
  void DrawToggle(HDC dc, const RECT &r, const std::wstring &text, const Fonts &fonts, int dpi, bool on, bool hot,
                  bool focus);
  // The progress bar: blue with a cyan leading edge; `marquee` >= 0: a moving block (busy, amount unknown).
  void DrawProgress(HDC dc, const RECT &r, double fraction, int marquee, int dpi);
  // "PDZ RESTORED" ("RESTORED" in orange) in the display font, left-aligned in `r`.
  void DrawTitle(HDC dc, const RECT &r, HFONT font, int dpi, int spacing_px = 2);
  // The small info icon (a cyan circled "i") with its top-left at (x, y).
  void DrawInfoIcon(HDC dc, int x, int y, int size, COLORREF colour);

  // Our message box: same text and buttons as a Windows one. `kind`: MB_ICONINFORMATION, MB_ICONWARNING,
  // MB_ICONERROR or MB_ICONQUESTION; `buttons`: MB_OK, MB_OKCANCEL, MB_YESNO or MB_YESNOCANCEL. Returns IDOK, IDCANCEL,
  // IDYES or IDNO (Esc / the close box: IDCANCEL, or IDNO for Yes / No). Modal to `owner` (may be null).
  int ShowMessage(HWND owner, const std::wstring &text, const std::wstring &caption, UINT kind, UINT buttons);
  // Test aid: the next ShowMessage writes itself as a .bmp to this path and answers its default button.
  void CaptureNextMessageBox(const std::wstring &bmp_path);

  // The DPI our setup windows and message boxes lay out for: the screen's, or a test value (pdz_settings_test_dpi, for
  // the 100 % / 150 % screenshots). 0 = the screen's.
  int ScreenDpi();
  void SetTestDpi(int dpi);

  // Writes a window (as composed by DWM) to a .bmp.
  bool CaptureWindow(HWND window, const std::wstring &bmp_path);

  // Controller navigation for our windows, polled from a timer: D-pad / left stick up and down move the keyboard
  // focus, A presses the focused control, B is Esc, LB / RB are Ctrl+Shift+Tab / Ctrl+Tab. Returns the virtual key it
  // sent (0: none).
  UINT PollController(HWND window);
} // namespace pdz::theme
