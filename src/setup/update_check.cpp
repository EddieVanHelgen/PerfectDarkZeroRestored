// PDZ Restored: the update check of the settings window (see update_check.h).
#include "update_check.h"
#include "../pdz_version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/xchar.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#pragma comment(lib, "winhttp.lib")

REXCVAR_DEFINE_BOOL(pdz_update_check, true, "Setup",
                    "Check for updates: when the settings window opens, ask GitHub in the background whether a newer "
                    "PDZ Restored release is out, and show it in the window (\"Open download page\"). Nothing is sent "
                    "but the request itself; nothing is ever downloaded. The window's Advanced tab sets it");
REXCVAR_DEFINE_STRING(pdz_update_test_version, "", "Testing",
                      "TESTING TOOL: the update check compares the latest release with this version instead of the "
                      "exe's (e.g. \"0.9.0\" shows the real latest release as new). Empty = the exe's version");
REXCVAR_DEFINE_STRING(pdz_update_test_url, "", "Testing",
                      "TESTING TOOL: the address the update check asks instead of GitHub's latest-release address (a "
                      "broken or unreachable one tests the offline case). Empty = GitHub");

namespace pdz::update_check
{
  namespace
  {
    constexpr const wchar_t *kLatestUrl =
        L"https://api.github.com/repos/EddieVanHelgen/PerfectDarkZeroRestored/releases/latest";
    constexpr const char *kReleasePage = "https://github.com/EddieVanHelgen/PerfectDarkZeroRestored/releases/tag/";
    constexpr DWORD kTimeoutMs = 3000;          // each of resolve, connect, send and receive
    constexpr size_t kMaxBody = 512 * 1024;     // the latest-release JSON is a few KB

    std::mutex g_mutex;
    Result g_result;
    std::string g_line;          // the check's log line, once it is known
    bool g_logging_ready = false; // LoggingReady was called: log lines go out at once
    bool g_logged = false;

    void Finish(const Result &r, const std::string &line)
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_result = r;
      g_line = line;
      if (g_logging_ready && !g_logged)
      {
        g_logged = true;
        REXLOG_INFO("{}", g_line);
      }
    }

    std::wstring Widen(const std::string &s) { return std::wstring(s.begin(), s.end()); }

    // "v0.9.3" / "0.9.3" -> {0, 9, 3}; empty if it isn't 1 to 4 dot-separated numbers.
    std::vector<int> ParseVersion(std::string s)
    {
      if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
      {
        s.erase(0, 1);
      }
      std::vector<int> parts;
      size_t i = 0;
      while (i < s.size())
      {
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
        {
          ++j;
        }
        if (j == i || j - i > 6)
        {
          return {};
        }
        parts.push_back(std::stoi(s.substr(i, j - i)));
        if (j == s.size())
        {
          break;
        }
        if (s[j] != '.')
        {
          return {};
        }
        i = j + 1;
        if (i == s.size())
        {
          return {};
        }
      }
      if (parts.empty() || parts.size() > 4)
      {
        return {};
      }
      parts.resize(4, 0);
      return parts;
    }

    // The string value of "key" in a JSON object's text ("" if absent or not a plain string).
    std::string JsonString(const std::string &json, const std::string &key)
    {
      const std::string needle = "\"" + key + "\"";
      size_t at = json.find(needle);
      if (at == std::string::npos)
      {
        return {};
      }
      at = json.find_first_not_of(" \t\r\n", at + needle.size());
      if (at == std::string::npos || json[at] != ':')
      {
        return {};
      }
      at = json.find_first_not_of(" \t\r\n", at + 1);
      if (at == std::string::npos || json[at] != '"')
      {
        return {};
      }
      const size_t end = json.find('"', at + 1);
      if (end == std::string::npos || end - at > 64)
      {
        return {};
      }
      return json.substr(at + 1, end - at - 1);
    }

    struct Handle
    {
      HINTERNET h = nullptr;
      ~Handle()
      {
        if (h)
        {
          WinHttpCloseHandle(h);
        }
      }
    };

    // GET the address; the body, or "" with `why` set.
    std::string Get(const std::wstring &url, std::string &why)
    {
      URL_COMPONENTS uc{};
      uc.dwStructSize = sizeof(uc);
      wchar_t host[256] = {}, path[1024] = {};
      uc.lpszHostName = host;
      uc.dwHostNameLength = DWORD(std::size(host));
      uc.lpszUrlPath = path;
      uc.dwUrlPathLength = DWORD(std::size(path));
      if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
      {
        why = fmt::format("the address can't be read (error {})", GetLastError());
        return {};
      }
      const std::wstring agent = Widen(std::string("PDZRestored/") + PDZ_RESTORED_VERSION);
      Handle session;
      session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
      if (!session.h) // before Windows 8.1
      {
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
      }
      if (!session.h)
      {
        why = fmt::format("WinHTTP didn't open (error {})", GetLastError());
        return {};
      }
      WinHttpSetTimeouts(session.h, int(kTimeoutMs), int(kTimeoutMs), int(kTimeoutMs), int(kTimeoutMs));
      Handle connection;
      connection.h = WinHttpConnect(session.h, host, uc.nPort, 0);
      if (!connection.h)
      {
        why = fmt::format("no connection (error {})", GetLastError());
        return {};
      }
      Handle request;
      request.h = WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
      if (!request.h)
      {
        why = fmt::format("the request didn't open (error {})", GetLastError());
        return {};
      }
      // No cookies, no stored credentials: the request carries the User-Agent and nothing about the player.
      DWORD off = WINHTTP_DISABLE_COOKIES;
      WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
      DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
      WinHttpSetOption(request.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon));
      if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
          !WinHttpReceiveResponse(request.h, nullptr))
      {
        const DWORD e = GetLastError();
        why = e == ERROR_WINHTTP_TIMEOUT             ? "no answer within 3 s"
              : e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "the name doesn't resolve (offline?)"
              : e == ERROR_WINHTTP_CANNOT_CONNECT    ? "can't connect (offline?)"
                                                     : fmt::format("no answer (WinHTTP error {})", e);
        return {};
      }
      DWORD status = 0, size = sizeof(status);
      WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX);
      if (status != 200)
      {
        why = fmt::format("the answer was HTTP {}", status);
        return {};
      }
      std::string body;
      for (;;)
      {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available))
        {
          why = fmt::format("the answer broke off (error {})", GetLastError());
          return {};
        }
        if (available == 0)
        {
          break;
        }
        if (body.size() + available > kMaxBody)
        {
          why = "the answer is too long";
          return {};
        }
        const size_t at = body.size();
        body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + at, available, &read))
        {
          why = fmt::format("the answer broke off (error {})", GetLastError());
          return {};
        }
        body.resize(at + read);
      }
      return body;
    }

    void Check(std::wstring url, std::string mine, bool test_version)
    {
      const std::string label = test_version ? fmt::format("{} (pdz_update_test_version)", mine) : mine;
      std::string why;
      const std::string body = Get(url, why);
      if (body.empty())
      {
        Finish(Result{State::kFailed}, fmt::format("pdz_update: no update check this start: {} (this is {})",
                                                   why.empty() ? "an empty answer" : why, label));
        return;
      }
      const std::string tag = JsonString(body, "tag_name");
      const std::vector<int> latest = ParseVersion(tag), have = ParseVersion(mine);
      if (latest.empty() || have.empty())
      {
        Finish(Result{State::kFailed},
               fmt::format("pdz_update: no update check this start: the release tag '{}' can't be read (this is {})",
                           tag.substr(0, 32), label));
        return;
      }
      if (latest <= have)
      {
        Finish(Result{State::kCurrent},
               fmt::format("pdz_update: the latest release is {}; this is {}: nothing to show", tag, label));
        return;
      }
      Result r{State::kNewer};
      r.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
      r.page = kReleasePage + tag; // the tag is digits and dots only (ParseVersion), so the address is this project's
      Finish(r, fmt::format("pdz_update: {} is available (this is {}): shown in the settings window ({})", tag, label,
                            r.page));
    }
  } // namespace

  void Start()
  {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_result.state != State::kIdle)
      {
        return;
      }
      g_result.state = State::kPending;
    }
    const std::string test_url = REXCVAR_GET(pdz_update_test_url);
    const std::string test_version = REXCVAR_GET(pdz_update_test_version);
    const std::wstring url = test_url.empty() ? std::wstring(kLatestUrl) : Widen(test_url);
    const std::string mine = test_version.empty() ? std::string(PDZ_RESTORED_VERSION) : test_version;
    // Detached: the window and the game never wait for it (at most about 12 s of timeouts, then it ends).
    std::thread(Check, url, mine, !test_version.empty()).detach();
  }

  Result Get()
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_result;
  }

  void LoggingReady()
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_logging_ready = true;
    if (g_logged)
    {
      return;
    }
    if (g_result.state == State::kIdle)
    {
      g_logged = true;
      REXLOG_INFO("pdz_update: no update check this start (the settings window wasn't shown, or Check for updates is "
                  "off: no request was made)");
    }
    else if (!g_line.empty())
    {
      g_logged = true;
      REXLOG_INFO("{}", g_line);
    }
  }
} // namespace pdz::update_check
