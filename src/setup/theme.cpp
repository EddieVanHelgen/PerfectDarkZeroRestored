// PDZ Restored: the look of our own windows (theme.h).
#include "theme.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

namespace pdz::theme
{
  namespace
  {
    int Px(int v, int dpi) { return MulDiv(v, dpi, 96); }

    COLORREF Mix(COLORREF a, COLORREF b, double t)
    {
      auto c = [&](int x, int y) { return int(x + (y - x) * t + 0.5); };
      return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
    }

    TRIVERTEX Vertex(int x, int y, COLORREF c)
    {
      TRIVERTEX v{};
      v.x = x;
      v.y = y;
      v.Red = COLOR16(GetRValue(c) << 8);
      v.Green = COLOR16(GetGValue(c) << 8);
      v.Blue = COLOR16(GetBValue(c) << 8);
      return v;
    }

    // A clip region for a rounded rectangle; restore with SelectClipRgn(dc, nullptr) (our drawing never clips
    // otherwise).
    void ClipRound(HDC dc, const RECT &r, int radius)
    {
      HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);
      SelectClipRgn(dc, rgn);
      DeleteObject(rgn);
    }

    void Frame(HDC dc, const RECT &r, int radius, COLORREF border, int width = 1)
    {
      HPEN pen = CreatePen(PS_SOLID, width, border);
      HGDIOBJ old_pen = SelectObject(dc, pen);
      HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(pen);
    }

    // Orange like hair highlights: #FFB060 top-left, #FF7A2E, #E8481C, #B8300F bottom-right.
    void OrangeGradient(HDC dc, const RECT &r, bool hot)
    {
      const COLORREF tl = hot ? RGB(0xFF, 0xC0, 0x78) : RGB(0xFF, 0xB0, 0x60);
      TRIVERTEX v[4] = {Vertex(r.left, r.top, tl), Vertex(r.right, r.top, RGB(0xFF, 0x7A, 0x2E)),
                        Vertex(r.right, r.bottom, RGB(0xB8, 0x30, 0x0F)),
                        Vertex(r.left, r.bottom, RGB(0xE8, 0x48, 0x1C))};
      GRADIENT_TRIANGLE t[2] = {{0, 1, 2}, {0, 2, 3}};
      GradientFill(dc, v, 4, t, 2, GRADIENT_FILL_TRIANGLE);
    }

    // Brushed silver: vertical #F7F9FC, #D3D9E3 (42 %), #A7B0BF band (52 %), #C9D0DB (78 %), #E9EDF3.
    void SilverGradient(HDC dc, const RECT &r, bool hot)
    {
      const COLORREF stops[5] = {RGB(0xF7, 0xF9, 0xFC), RGB(0xD3, 0xD9, 0xE3), RGB(0xA7, 0xB0, 0xBF),
                                 RGB(0xC9, 0xD0, 0xDB), RGB(0xE9, 0xED, 0xF3)};
      const double at[5] = {0.0, 0.42, 0.52, 0.78, 1.0};
      const int h = r.bottom - r.top;
      for (int i = 0; i < 4; ++i)
      {
        COLORREF a = stops[i], b = stops[i + 1];
        if (hot)
        {
          a = Mix(a, RGB(255, 255, 255), 0.25);
          b = Mix(b, RGB(255, 255, 255), 0.25);
        }
        TRIVERTEX v[2] = {Vertex(r.left, r.top + int(h * at[i]), a), Vertex(r.right, r.top + int(h * at[i + 1]), b)};
        GRADIENT_RECT g{0, 1};
        GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
      }
    }

    std::wstring Upper(std::wstring s)
    {
      CharUpperBuffW(s.data(), DWORD(s.size()));
      return s;
    }

    HANDLE g_font_handle = nullptr;
    std::once_flag g_font_once;

    void LoadDisplayFont()
    {
      HMODULE module = GetModuleHandleW(nullptr);
      HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(10), MAKEINTRESOURCEW(10)); // 10 RCDATA (RT_RCDATA = 10)
      if (!res)
      {
        return;
      }
      HGLOBAL data = LoadResource(module, res);
      const DWORD size = SizeofResource(module, res);
      void *bytes = data ? LockResource(data) : nullptr;
      if (bytes && size)
      {
        DWORD count = 0;
        g_font_handle = AddFontMemResourceEx(bytes, size, nullptr, &count);
      }
    }

    HFONT Font(const wchar_t *face, int px_height, int weight)
    {
      LOGFONTW lf{};
      lf.lfHeight = -px_height;
      lf.lfWeight = weight;
      lf.lfCharSet = DEFAULT_CHARSET;
      lf.lfQuality = CLEARTYPE_QUALITY;
      wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
      return CreateFontIndirectW(&lf);
    }

    std::map<COLORREF, HBRUSH> g_brushes;
    std::mutex g_brush_mutex;

    // Hover state of our owner-drawn buttons (set by the subclass in EnableHover).
    LRESULT CALLBACK HoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
    {
      switch (msg)
      {
      case WM_MOUSEMOVE:
        if (!GetPropW(hwnd, L"pdz_hot"))
        {
          SetPropW(hwnd, L"pdz_hot", reinterpret_cast<HANDLE>(1));
          TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&t);
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
      case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"pdz_hot");
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
      case WM_NCDESTROY:
        RemovePropW(hwnd, L"pdz_hot");
        RemoveWindowSubclass(hwnd, HoverProc, 1);
        break;
      default:
        break;
      }
      return DefSubclassProc(hwnd, msg, wp, lp);
    }
  } // namespace

  void EnableHover(HWND button) { SetWindowSubclass(button, HoverProc, 1, 0); }
  bool IsHot(HWND button) { return GetPropW(button, L"pdz_hot") != nullptr; }

  Fonts MakeFonts(int dpi)
  {
    std::call_once(g_font_once, LoadDisplayFont);
    const wchar_t *display = g_font_handle ? L"Rajdhani" : L"Segoe UI";
    Fonts f;
    f.body = Font(L"Segoe UI", Px(14, dpi), FW_NORMAL);
    f.body_bold = Font(L"Segoe UI", Px(14, dpi), FW_SEMIBOLD);
    f.note = Font(L"Segoe UI", Px(13, dpi), FW_NORMAL);
    f.display = Font(display, Px(26, dpi), FW_BOLD);
    f.display_small = Font(display, Px(16, dpi), FW_BOLD);
    f.button = Font(display, Px(18, dpi), FW_BOLD);
    f.title = Font(display, Px(24, dpi), FW_BOLD);
    f.title_side = Font(display, Px(20, dpi), FW_BOLD);
    return f;
  }

  void FreeFonts(Fonts &f)
  {
    for (HFONT h : {f.body, f.body_bold, f.note, f.display, f.display_small, f.button, f.title, f.title_side})
    {
      if (h)
      {
        DeleteObject(h);
      }
    }
    f = Fonts{};
  }

  HBRUSH Brush(COLORREF colour)
  {
    std::lock_guard<std::mutex> lock(g_brush_mutex);
    HBRUSH &b = g_brushes[colour];
    // A cached brush that something deleted (a window class unregistered with it as its background, which Windows
    // deletes) is made again, so a fill never silently fails (2026-11-06: white setup windows).
    if (!b || GetObjectType(b) != OBJ_BRUSH)
    {
      b = CreateSolidBrush(colour);
    }
    return b;
  }

  void ThemeTitleBar(HWND window)
  {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = kTitleBar, border = kBorder, text = kTextDim;
    DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    DwmSetWindowAttribute(window, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
  }

  void FillRect(HDC dc, const RECT &r, COLORREF colour) { ::FillRect(dc, &r, Brush(colour)); }

  void FillRound(HDC dc, const RECT &r, int radius, COLORREF fill, COLORREF border)
  {
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, Brush(fill));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }

  void Text(HDC dc, const std::wstring &text, RECT r, HFONT font, COLORREF colour, UINT flags, int spacing)
  {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour);
    const int old_extra = SetTextCharacterExtra(dc, spacing);
    DrawTextW(dc, text.c_str(), int(text.size()), &r, flags | DT_NOPREFIX);
    SetTextCharacterExtra(dc, old_extra);
    SelectObject(dc, old);
  }

  void DrawButton(HDC dc, const RECT &r, ButtonStyle style, const std::wstring &text, const Fonts &fonts, int dpi,
                  bool selected, bool hot, bool pressed, bool focus, bool disabled)
  {
    const int radius = Px(6, dpi);
    RECT tr = r;
    if (pressed)
    {
      OffsetRect(&tr, 0, 1);
    }
    switch (style)
    {
    case ButtonStyle::kPrimary:
      if (disabled)
      {
        FillRound(dc, r, radius, kDisabledPanel, kBorder);
        Text(dc, Upper(text), tr, fonts.button, kDisabledText, DT_CENTER | DT_VCENTER | DT_SINGLELINE, Px(2, dpi));
        break;
      }
      ClipRound(dc, r, radius);
      OrangeGradient(dc, r, hot);
      SelectClipRgn(dc, nullptr);
      Frame(dc, r, radius, RGB(0xE8, 0x48, 0x1C));
      Text(dc, Upper(text), tr, fonts.button, kNavyText, DT_CENTER | DT_VCENTER | DT_SINGLELINE, Px(2, dpi));
      break;
    case ButtonStyle::kSecondary:
      ClipRound(dc, r, radius);
      SilverGradient(dc, r, hot && !disabled);
      SelectClipRgn(dc, nullptr);
      Frame(dc, r, radius, RGB(0x8E, 0x98, 0xA8));
      Text(dc, text, tr, fonts.body_bold, disabled ? RGB(0x5A, 0x63, 0x75) : kNavyText,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      break;
    case ButtonStyle::kDanger:
      FillRound(dc, r, radius, hot ? kPanel : kBackground, RGB(0x6A, 0x2E, 0x12));
      Text(dc, text, tr, fonts.body, kOrange, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      break;
    case ButtonStyle::kAccent:
      FillRound(dc, r, radius, hot ? Mix(kBlue, RGB(255, 255, 255), 0.12) : kBlue, kBlue);
      Text(dc, text, tr, fonts.body_bold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      break;
    case ButtonStyle::kTab:
    {
      FillRect(dc, r, kSideBar);
      if (selected || hot)
      {
        FillRound(dc, r, radius, selected ? kPanelHot : kPanel, selected ? kPanelHot : kPanel);
      }
      if (selected)
      {
        RECT bar{r.left, r.top + Px(6, dpi), r.left + Px(3, dpi), r.bottom - Px(6, dpi)};
        FillRect(dc, bar, kCyan);
      }
      RECT t = tr;
      t.left += Px(14, dpi);
      Text(dc, text, t, selected ? fonts.body_bold : fonts.body, selected ? RGB(255, 255, 255) : RGB(0xC9, 0xD3, 0xE6),
           DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      break;
    }
    case ButtonStyle::kPanel:
    default:
      FillRound(dc, r, radius, disabled ? kDisabledPanel : hot ? kPanelHot : kPanel, kBorder);
      Text(dc, text, tr, fonts.body, disabled ? kDisabledText : kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      break;
    }
    if (focus)
    {
      RECT f = r;
      InflateRect(&f, -Px(2, dpi), -Px(2, dpi));
      Frame(dc, f, std::max(1, radius - Px(2, dpi)), kCyan, std::max(1, Px(2, dpi) / 1));
    }
  }

  void DrawSegment(HDC dc, const RECT &r, const std::wstring &text, const Fonts &fonts, int dpi, bool selected,
                   bool hot, bool focus, bool disabled, bool first, bool last)
  {
    const int radius = Px(6, dpi);
    const COLORREF fill = disabled ? kDisabledPanel : selected ? kBlue : hot ? kPanelHot : kPanel;
    const COLORREF border = selected ? kBlue : kBorder;
    // Round only the outer corners: draw a rounded shape wider than the segment and clip it.
    RECT shape = r;
    if (!first)
    {
      shape.left -= radius * 2;
    }
    if (!last)
    {
      shape.right += radius * 2;
    }
    HRGN clip = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    SelectClipRgn(dc, clip);
    DeleteObject(clip);
    FillRound(dc, shape, radius, fill, border);
    if (!first)
    {
      HPEN pen = CreatePen(PS_SOLID, 1, border);
      HGDIOBJ old = SelectObject(dc, pen);
      MoveToEx(dc, r.left, r.top, nullptr);
      LineTo(dc, r.left, r.bottom);
      SelectObject(dc, old);
      DeleteObject(pen);
    }
    SelectClipRgn(dc, nullptr);
    Text(dc, text, r, selected ? fonts.body_bold : fonts.body,
         disabled ? kDisabledText : selected ? RGB(255, 255, 255) : kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (focus)
    {
      RECT f = r;
      InflateRect(&f, -Px(3, dpi), -Px(3, dpi));
      Frame(dc, f, Px(3, dpi), kCyan, std::max(1, Px(2, dpi)));
    }
  }

  void DrawToggle(HDC dc, const RECT &r, const std::wstring &text, const Fonts &fonts, int dpi, bool on, bool hot,
                  bool focus)
  {
    const int h = Px(22, dpi), w = Px(40, dpi);
    const int top = r.top + (r.bottom - r.top - h) / 2;
    RECT track{r.left + Px(2, dpi), top, r.left + Px(2, dpi) + w, top + h};
    FillRound(dc, track, h / 2, on ? kBlue : hot ? kPanelHot : kPanel, on ? kBlue : kBorder);
    const int k = h - Px(6, dpi);
    const int kx = on ? track.right - Px(3, dpi) - k : track.left + Px(3, dpi);
    RECT knob{kx, top + Px(3, dpi), kx + k, top + Px(3, dpi) + k};
    FillRound(dc, knob, k / 2, on ? RGB(255, 255, 255) : kTextDim, on ? RGB(255, 255, 255) : kTextDim);
    if (focus)
    {
      RECT f = track;
      InflateRect(&f, Px(2, dpi), Px(2, dpi));
      Frame(dc, f, h / 2 + Px(2, dpi), kCyan, std::max(1, Px(2, dpi)));
    }
    RECT t = r;
    t.left = track.right + Px(12, dpi);
    Text(dc, text, t, fonts.body, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  void DrawProgress(HDC dc, const RECT &r, double fraction, int marquee, int dpi)
  {
    const int radius = Px(5, dpi);
    FillRound(dc, r, radius, kPanel, kBorder);
    RECT inner = r;
    InflateRect(&inner, -Px(2, dpi), -Px(2, dpi));
    const int w = inner.right - inner.left;
    RECT fill = inner;
    if (marquee >= 0)
    {
      const int block = std::max(1, w / 4);
      const int x = inner.left - block + (marquee % (w + block));
      fill.left = std::max<LONG>(inner.left, x);
      fill.right = std::min<LONG>(inner.right, x + block);
    }
    else
    {
      fill.right = inner.left + int(w * std::clamp(fraction, 0.0, 1.0));
    }
    if (fill.right > fill.left)
    {
      FillRound(dc, fill, std::max(1, radius - Px(2, dpi)), kBlue, kBlue);
      RECT edge{std::max(fill.left, fill.right - Px(3, dpi)), fill.top, fill.right, fill.bottom};
      FillRect(dc, edge, kCyan);
    }
  }

  void DrawTitle(HDC dc, const RECT &r, HFONT font, int dpi, int spacing_px)
  {
    HGDIOBJ old = SelectObject(dc, font);
    const int spacing = Px(spacing_px, dpi);
    const int old_extra = SetTextCharacterExtra(dc, spacing);
    SIZE pdz{};
    GetTextExtentPoint32W(dc, L"PDZ ", 4, &pdz);
    SetTextCharacterExtra(dc, old_extra);
    SelectObject(dc, old);
    Text(dc, L"PDZ ", r, font, kText, DT_LEFT | DT_TOP | DT_SINGLELINE, spacing);
    RECT rest = r;
    rest.left += pdz.cx;
    Text(dc, L"RESTORED", rest, font, kTitleOrange, DT_LEFT | DT_TOP | DT_SINGLELINE, spacing);
  }

  void DrawInfoIcon(HDC dc, int x, int y, int size, COLORREF colour)
  {
    const int pw = std::max(1, size / 9);
    HPEN pen = CreatePen(PS_SOLID, pw, colour);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y, x + size, y + size);
    const int cx = x + size / 2;
    MoveToEx(dc, cx, y + size * 11 / 24, nullptr);
    LineTo(dc, cx, y + size * 17 / 24);
    SelectObject(dc, old_brush);
    RECT dot{cx - pw / 2 - 1, y + size * 7 / 24 - pw / 2 - 1, cx + pw / 2 + 1, y + size * 7 / 24 + pw / 2 + 1};
    ::FillRect(dc, &dot, Brush(colour));
    SelectObject(dc, old_pen);
    DeleteObject(pen);
  }

  bool CaptureWindow(HWND window, const std::wstring &bmp_path)
  {
    RECT wr{};
    GetWindowRect(window, &wr);
    const int cw = wr.right - wr.left, ch = wr.bottom - wr.top;
    if (cw <= 0 || ch <= 0)
    {
      return false;
    }
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(window, mdc, 2 /* PW_RENDERFULLCONTENT */);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + DWORD(cw) * DWORD(ch) * 4;
    std::ofstream out(bmp_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char *>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    out.write(static_cast<const char *>(bits), std::streamsize(size_t(cw) * size_t(ch) * 4));
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    return bool(out);
  }

  // ---- the message box ----

  namespace
  {
    std::wstring g_capture_next;

    struct Box
    {
      HWND window = nullptr;
      std::wstring text;
      UINT kind = MB_ICONINFORMATION;
      std::vector<std::pair<int, std::wstring>> buttons; // id, label; the first is the default (primary)
      int result = 0;
      int cancel_id = IDCANCEL;
      int dpi = 96;
      Fonts fonts;
      RECT text_rect{};
    };

    void Pump(DWORD ms)
    {
      for (const DWORD until = GetTickCount() + ms; GetTickCount() < until;)
      {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        {
          TranslateMessage(&m);
          DispatchMessageW(&m);
        }
        Sleep(10);
      }
    }

    LRESULT CALLBACK BoxProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      Box *box = reinterpret_cast<Box *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      switch (msg)
      {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, rc, kBackground);
        if (box)
        {
          const int d = box->dpi;
          RECT footer{0, rc.bottom - Px(68, d), rc.right, rc.bottom};
          FillRect(dc, footer, kSideBar);
          RECT line{0, footer.top, rc.right, footer.top + 1};
          FillRect(dc, line, kBorder);
          const COLORREF icon = box->kind == MB_ICONERROR || box->kind == MB_ICONWARNING ? kOrange : kCyan;
          DrawInfoIcon(dc, Px(24, d), Px(26, d), Px(28, d), icon);
          Text(dc, box->text, box->text_rect, box->fonts.body, kText, DT_LEFT | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DRAWITEM:
        if (box)
        {
          const DRAWITEMSTRUCT *di = reinterpret_cast<const DRAWITEMSTRUCT *>(lp);
          wchar_t label[64] = {};
          GetWindowTextW(di->hwndItem, label, 64);
          const bool primary = !box->buttons.empty() && int(di->CtlID) == box->buttons.front().first;
          FillRect(di->hDC, di->rcItem, kSideBar);
          DrawButton(di->hDC, di->rcItem, primary ? ButtonStyle::kPrimary : ButtonStyle::kSecondary, label,
                     box->fonts, box->dpi, false, IsHot(di->hwndItem), (di->itemState & ODS_SELECTED) != 0,
                     (di->itemState & ODS_FOCUS) != 0, false);
          return TRUE;
        }
        break;
      case WM_COMMAND:
        if (box && HIWORD(wp) == BN_CLICKED)
        {
          const int id = LOWORD(wp);
          for (const auto &b : box->buttons)
          {
            if (b.first == id)
            {
              box->result = id;
            }
          }
          if (id == IDCANCEL && !box->result)
          {
            box->result = box->cancel_id;
          }
        }
        return 0;
      case WM_CLOSE:
        if (box)
        {
          box->result = box->cancel_id;
        }
        return 0;
      default:
        break;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
  } // namespace

  void CaptureNextMessageBox(const std::wstring &bmp_path) { g_capture_next = bmp_path; }

  namespace
  {
    std::atomic<int> g_test_dpi{0};
  }

  void SetTestDpi(int dpi) { g_test_dpi = dpi > 0 ? dpi : 0; }

  int ScreenDpi()
  {
    if (const int test = g_test_dpi.load(); test > 0)
    {
      return test;
    }
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? dpi : 96;
  }

  int ShowMessage(HWND owner, const std::wstring &text, const std::wstring &caption, UINT kind, UINT buttons)
  {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered)
    {
      WNDCLASSW wc{};
      wc.lpfnWndProc = BoxProc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
      wc.hbrBackground = nullptr;
      wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
      wc.lpszClassName = L"PDZRestoredMessage";
      registered = RegisterClassW(&wc) != 0;
    }
    Box box;
    box.text = text;
    box.kind = kind;
    switch (buttons)
    {
    case MB_OKCANCEL:
      box.buttons = {{IDOK, L"OK"}, {IDCANCEL, L"Cancel"}};
      break;
    case MB_YESNO:
      box.buttons = {{IDYES, L"Yes"}, {IDNO, L"No"}};
      box.cancel_id = IDNO;
      break;
    case MB_YESNOCANCEL:
      box.buttons = {{IDYES, L"Yes"}, {IDNO, L"No"}, {IDCANCEL, L"Cancel"}};
      break;
    default:
      box.buttons = {{IDOK, L"OK"}};
      box.cancel_id = IDOK;
      break;
    }
    box.dpi = ScreenDpi();
    const int d = box.dpi;
    box.fonts = MakeFonts(d);
    // Size the text: a fixed width, as many lines as it needs.
    const int width = Px(500, d);
    RECT measure{0, 0, width - Px(24 + 28 + 18 + 28, d), 0};
    {
      HDC dc = GetDC(nullptr);
      HGDIOBJ old = SelectObject(dc, box.fonts.body);
      DrawTextW(dc, text.c_str(), int(text.size()), &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      SelectObject(dc, old);
      ReleaseDC(nullptr, dc);
    }
    const int text_h = std::max<int>(measure.bottom, Px(28, d));
    box.text_rect = RECT{Px(24 + 28 + 18, d), Px(28, d), width - Px(28, d), Px(28, d) + text_h};
    const int height = box.text_rect.bottom + Px(28 + 68, d);
    RECT wr{0, 0, width, height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_DLGMODALFRAME);
    box.window = registered ? CreateWindowExW(WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_TOPMOST), L"PDZRestoredMessage",
                                              caption.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left,
                                              wr.bottom - wr.top, owner, nullptr, inst, nullptr)
                            : nullptr;
    if (!box.window)
    {
      FreeFonts(box.fonts);
      return ::MessageBoxW(owner, text.c_str(), caption.c_str(), kind | buttons | MB_SETFOREGROUND);
    }
    SetWindowLongPtrW(box.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&box));
    ThemeTitleBar(box.window);
    // Buttons right-aligned in the footer, the default (primary) one rightmost.
    const int bw = Px(112, d), bh = Px(40, d), gap = Px(12, d);
    int x = width - Px(24, d) - bw;
    HWND first = nullptr;
    for (const auto &[id, label] : box.buttons)
    {
      HWND b = CreateWindowExW(0, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x,
                               height - Px(68, d) + (Px(68, d) - bh) / 2, bw, bh, box.window,
                               reinterpret_cast<HMENU>(INT_PTR(id)), inst, nullptr);
      EnableHover(b);
      first = first ? first : b;
      x -= bw + gap;
    }
    // Centred on the owner, else on the primary monitor.
    RECT ow{};
    if (owner && GetWindowRect(owner, &ow))
    {
    }
    else
    {
      ow = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    GetWindowRect(box.window, &wr);
    SetWindowPos(box.window, nullptr, ow.left + ((ow.right - ow.left) - (wr.right - wr.left)) / 2,
                 ow.top + ((ow.bottom - ow.top) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    if (owner)
    {
      EnableWindow(owner, FALSE);
    }
    ShowWindow(box.window, SW_SHOW);
    SetForegroundWindow(box.window);
    SetFocus(first);
    SetTimer(box.window, 1, 100, nullptr);
    if (!g_capture_next.empty())
    {
      const std::wstring path = g_capture_next;
      g_capture_next.clear();
      Pump(400);
      CaptureWindow(box.window, path);
      box.result = box.buttons.front().first;
    }
    MSG msg;
    while (!box.result && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
      if (msg.message == WM_TIMER && msg.hwnd == box.window)
      {
        PollController(box.window);
        continue;
      }
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
      {
        box.result = box.cancel_id;
        break;
      }
      if (!IsDialogMessageW(box.window, &msg))
      {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (owner)
    {
      EnableWindow(owner, TRUE);
      SetForegroundWindow(owner);
    }
    KillTimer(box.window, 1);
    SetWindowLongPtrW(box.window, GWLP_USERDATA, 0);
    DestroyWindow(box.window);
    FreeFonts(box.fonts);
    return box.result;
  }

  // ---- controller navigation ----

  namespace
  {
    struct XInputGamepad
    {
      WORD buttons;
      BYTE lt, rt;
      SHORT lx, ly, rx, ry;
    };
    struct XInputState
    {
      DWORD packet;
      XInputGamepad pad;
    };
    using GetStateFn = DWORD(WINAPI *)(DWORD, XInputState *);
    GetStateFn g_get_state = nullptr;
    std::once_flag g_xinput_once;
    WORD g_prev = 0;
  } // namespace

  UINT PollController(HWND window)
  {
    std::call_once(g_xinput_once, [] {
      HMODULE x = LoadLibraryW(L"xinput1_4.dll");
      if (!x)
      {
        x = LoadLibraryW(L"xinput9_1_0.dll");
      }
      g_get_state = x ? reinterpret_cast<GetStateFn>(GetProcAddress(x, "XInputGetState")) : nullptr;
    });
    if (!g_get_state || GetForegroundWindow() != window)
    {
      return 0;
    }
    WORD now = 0;
    for (DWORD i = 0; i < 4; ++i)
    {
      XInputState s{};
      if (g_get_state(i, &s) == ERROR_SUCCESS)
      {
        now |= s.pad.buttons;
        // The left stick as a D-pad.
        if (s.pad.ly > 20000)
        {
          now |= 0x0001;
        }
        if (s.pad.ly < -20000)
        {
          now |= 0x0002;
        }
        if (s.pad.lx < -20000)
        {
          now |= 0x0004;
        }
        if (s.pad.lx > 20000)
        {
          now |= 0x0008;
        }
      }
    }
    const WORD pressed = now & ~g_prev;
    g_prev = now;
    HWND focus = GetFocus();
    if (pressed & 0x0001) // up
    {
      SendMessageW(window, WM_NEXTDLGCTL, 1, FALSE);
      return VK_UP;
    }
    if (pressed & 0x0002) // down
    {
      SendMessageW(window, WM_NEXTDLGCTL, 0, FALSE);
      return VK_DOWN;
    }
    if (pressed & 0x1000) // A
    {
      wchar_t cls[32] = {};
      if (focus && GetClassNameW(focus, cls, 32) && _wcsicmp(cls, L"Button") == 0)
      {
        SendMessageW(focus, BM_CLICK, 0, 0);
      }
      else if (focus)
      {
        PostMessageW(focus, WM_KEYDOWN, VK_RETURN, 0x001C0001);
      }
      return VK_RETURN;
    }
    if (pressed & 0x2000) // B
    {
      return VK_ESCAPE;
    }
    if (pressed & 0x0004)
    {
      return VK_LEFT;
    }
    if (pressed & 0x0008)
    {
      return VK_RIGHT;
    }
    if (pressed & 0x0100) // LB
    {
      return VK_PRIOR;
    }
    if (pressed & 0x0200) // RB
    {
      return VK_NEXT;
    }
    return 0;
  }
} // namespace pdz::theme
