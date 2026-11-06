// PDZ Restored: the setup window (setup_window.h), drawn in the theme (theme.h, 2026-11-04).
#include "setup_window.h"

#include "theme.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace pdz::setup_ui
{
  namespace
  {
    constexpr int kIdCancel = 101;
    constexpr UINT_PTR kTimer = 1;

    struct Ui
    {
      State *state = nullptr;
      HWND window = nullptr, button = nullptr;
      theme::Fonts fonts;
      int dpi = 96;
      std::wstring line1, line2;
      double progress = 0.0;
      int marquee = 0;
      bool cancelled = false;
      int px(int v) const { return MulDiv(v, dpi, 96); }
    };

    void Refresh(Ui &ui)
    {
      std::wstring l1, l2;
      double p = 0.0;
      {
        std::lock_guard<std::mutex> lock(ui.state->mutex);
        l1 = ui.state->line1;
        l2 = ui.state->line2;
        p = ui.state->progress;
      }
      const bool changed = l1 != ui.line1 || l2 != ui.line2 || p != ui.progress || p < 0.0;
      ui.line1 = l1;
      ui.line2 = l2;
      ui.progress = p;
      if (p < 0.0)
      {
        ui.marquee += ui.px(8);
      }
      if (changed)
      {
        InvalidateRect(ui.window, nullptr, FALSE);
      }
    }

    void Paint(Ui &ui, HDC target)
    {
      RECT rc;
      GetClientRect(ui.window, &rc);
      // Double-buffered: the bar moves every 50 ms.
      HDC dc = CreateCompatibleDC(target);
      HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(dc, bmp);
      theme::FillRect(dc, rc, theme::kBackground);
      const int x = ui.px(28), w = rc.right - ui.px(56);
      theme::DrawTitle(dc, RECT{x, ui.px(20), rc.right, ui.px(56)}, ui.fonts.title, ui.dpi);
      theme::Text(dc, ui.line1, RECT{x, ui.px(70), x + w, ui.px(94)}, ui.fonts.body_bold, theme::kText,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
      theme::DrawProgress(dc, RECT{x, ui.px(102), x + w, ui.px(120)}, ui.progress, ui.progress < 0.0 ? ui.marquee : -1,
                          ui.dpi);
      theme::Text(dc, ui.line2, RECT{x, ui.px(128), x + w, ui.px(150)}, ui.fonts.note, theme::kTextDim,
                  DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
      RECT footer{0, rc.bottom - ui.px(68), rc.right, rc.bottom};
      theme::FillRect(dc, footer, theme::kSideBar);
      theme::FillRect(dc, RECT{0, footer.top, rc.right, footer.top + 1}, theme::kBorder);
      BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
      SelectObject(dc, old);
      DeleteObject(bmp);
      DeleteDC(dc);
    }

    void Cancel(Ui &ui)
    {
      ui.state->cancel = true; // the worker stops; the window closes when it has
      ui.cancelled = true;
      EnableWindow(ui.button, FALSE);
      InvalidateRect(ui.button, nullptr, FALSE);
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
      Ui *ui = reinterpret_cast<Ui *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      switch (msg)
      {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (ui)
        {
          Paint(*ui, dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DRAWITEM:
        if (ui)
        {
          const DRAWITEMSTRUCT *di = reinterpret_cast<const DRAWITEMSTRUCT *>(lp);
          theme::FillRect(di->hDC, di->rcItem, theme::kSideBar);
          theme::DrawButton(di->hDC, di->rcItem, theme::ButtonStyle::kSecondary, L"Cancel", ui->fonts, ui->dpi, false,
                            theme::IsHot(di->hwndItem), (di->itemState & ODS_SELECTED) != 0,
                            (di->itemState & ODS_FOCUS) != 0, ui->cancelled);
          return TRUE;
        }
        break;
      case WM_TIMER:
        if (ui)
        {
          Refresh(*ui);
          if (theme::PollController(hwnd) == VK_ESCAPE)
          {
            Cancel(*ui);
          }
        }
        return 0;
      case WM_COMMAND:
        if (ui && LOWORD(wp) == kIdCancel)
        {
          Cancel(*ui);
        }
        return 0;
      case WM_CLOSE:
        if (ui)
        {
          Cancel(*ui);
        }
        return 0;
      default:
        break;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // TestCapture: from then on each progress window writes itself once into this folder: setup_progress.bmp (the
    // sample), then setup_progress_1.bmp, _2... (the real ones: the disc copy, the shader translation after the
    // settings window).
    std::filesystem::path g_capture_folder;
    int g_capture_count = 0;
  } // namespace

  bool RunWithProgress(const std::wstring &title, State &state, const std::function<void(State &)> &work)
  {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered)
    {
      WNDCLASSW wc{};
      wc.lpfnWndProc = Proc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
      wc.hbrBackground = nullptr;
      wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
      wc.lpszClassName = L"PDZRestoredSetup";
      registered = RegisterClassW(&wc) != 0;
    }
    Ui ui;
    ui.state = &state;
    ui.dpi = theme::ScreenDpi();
    RECT wr{0, 0, ui.px(560), ui.px(232)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&wr, style, FALSE);
    ui.window = registered ? CreateWindowExW(0, L"PDZRestoredSetup", title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                             wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr)
                           : nullptr;
    std::thread worker([&] {
      work(state);
      std::lock_guard<std::mutex> lock(state.mutex);
      state.done = true;
    });
    if (!ui.window)
    {
      worker.join();
      return false;
    }
    SetWindowLongPtrW(ui.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ui));
    theme::ThemeTitleBar(ui.window);
    ui.fonts = theme::MakeFonts(ui.dpi);
    RECT rc{};
    GetClientRect(ui.window, &rc);
    const int bw = ui.px(112), bh = ui.px(40);
    ui.button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                rc.right - ui.px(28) - bw, rc.bottom - ui.px(68) + (ui.px(68) - bh) / 2, bw, bh,
                                ui.window, reinterpret_cast<HMENU>(INT_PTR(kIdCancel)), inst, nullptr);
    theme::EnableHover(ui.button);
    // Centred on the primary monitor.
    GetWindowRect(ui.window, &wr);
    SetWindowPos(ui.window, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(ui.window, SW_SHOWNORMAL);
    SetForegroundWindow(ui.window);
    SetFocus(ui.button);
    SetTimer(ui.window, kTimer, 50, nullptr);
    Refresh(ui);
    const DWORD started = GetTickCount();
    std::wstring capture_path;
    if (!g_capture_folder.empty())
    {
      capture_path = (g_capture_folder / (g_capture_count == 0 ? std::wstring(L"setup_progress.bmp")
                                                                 : L"setup_progress_" + std::to_wstring(g_capture_count) +
                                                                       L".bmp"))
                         .wstring();
      ++g_capture_count;
    }
    for (;;)
    {
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.done)
        {
          break;
        }
      }
      if (!capture_path.empty() && GetTickCount() - started > 700)
      {
        theme::CaptureWindow(ui.window, capture_path);
        capture_path.clear();
      }
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
          Cancel(ui);
          continue;
        }
        if (!IsDialogMessageW(ui.window, &msg))
        {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
      }
    }
    worker.join();
    KillTimer(ui.window, kTimer);
    SetWindowLongPtrW(ui.window, GWLP_USERDATA, 0);
    DestroyWindow(ui.window);
    theme::FreeFonts(ui.fonts);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return true;
  }

  int Error(const std::wstring &title, const std::wstring &text)
  {
    return theme::ShowMessage(nullptr, text, title, MB_ICONERROR, MB_OK);
  }

  void Info(const std::wstring &title, const std::wstring &text)
  {
    theme::ShowMessage(nullptr, text, title, MB_ICONINFORMATION, MB_OK);
  }

  bool AskOkCancel(const std::wstring &title, const std::wstring &text)
  {
    return theme::ShowMessage(nullptr, text, title, MB_ICONQUESTION, MB_OKCANCEL) == IDOK;
  }

  void TestCapture(const std::filesystem::path &folder, const std::wstring &title, const std::wstring &message_title,
                   const std::wstring &message)
  {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    g_capture_folder = folder;
    g_capture_count = 0;
    State state;
    RunWithProgress(title, state, [](State &s) {
      {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.line1 = L"Preparing graphics";
        s.line2 = L"Making the shaders from your game files: about 10 seconds left";
        s.progress = 0.62;
      }
      Sleep(1200);
    });
    theme::CaptureNextMessageBox((folder / "setup_message.bmp").wstring());
    AskOkCancel(message_title, message);
  }

  bool PickFile(const std::wstring &title, const std::vector<std::pair<std::wstring, std::wstring>> &filters,
                std::filesystem::path &out)
  {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool picked = false;
    IFileOpenDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
      std::vector<COMDLG_FILTERSPEC> specs;
      for (const auto &[name, pattern] : filters)
      {
        specs.push_back({name.c_str(), pattern.c_str()});
      }
      dialog->SetTitle(title.c_str());
      if (!specs.empty())
      {
        dialog->SetFileTypes(UINT(specs.size()), specs.data());
      }
      DWORD options = 0;
      dialog->GetOptions(&options);
      dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
      if (SUCCEEDED(dialog->Show(nullptr)))
      {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)))
        {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
          {
            out = path;
            picked = true;
            CoTaskMemFree(path);
          }
          item->Release();
        }
      }
      dialog->Release();
    }
    if (SUCCEEDED(init))
    {
      CoUninitialize();
    }
    return picked;
  }
} // namespace pdz::setup_ui
