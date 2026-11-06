// pdz_fix_log (tu3-base): counts of what the ported game / AI fixes did.
#pragma once

namespace fix_log
{
  // One more time `what` happened (a fix changed something, or the event it
  // acts on, for context). Cheap when pdz_fix_log is off. Every 10 s, if
  // anything was counted, logs each name with its count since the last line
  // and its total.
  void Count(const char *what);
} // namespace fix_log
