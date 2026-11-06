// A guest function by address for our own indirect calls (vtable slots), or
// nullptr when the address is no recompiled function.
//
// Log 101 (Plant the Flag, split screen set up, map pack map: "[FATAL] Call to
// invalid or unregistered function at guest address 0x00000000 (LR
// 0x00000000)"): rex::runtime::ResolveIndirectFunction never returns null; for
// an address that isn't code it returns the SDK's trap, which aborts the game
// when called (thirdparty/rexglue-sdk/src/system/function_dispatcher.cpp). So
// every `if (!fn)` guard after it in our mods could never fire, and a call from
// one of our CallFrames reports target 0 and LR 0 whatever the slot held.
// GuestFunction turns the trap back into nullptr, so those guards work, and
// logs each refused address once (with who asked) for the next log.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/ppc/func.h>

namespace pdz
{
  inline PPCFunc *GuestFunction(uint32_t address, const char *who = "a vtable slot")
  {
    static PPCFunc *const trap = rex::runtime::ResolveIndirectFunction(0);
    PPCFunc *fn = rex::runtime::ResolveIndirectFunction(address);
    if (fn != trap)
    {
      return fn;
    }
    static std::mutex lock;
    static std::unordered_set<uint32_t> logged;
    std::lock_guard<std::mutex> hold(lock);
    if (logged.size() < 64 && logged.insert(address).second)
    {
      REXLOG_WARN("pdz: refused an indirect call to {:08X} ({}): not a recompiled function (log 101)", address, who);
    }
    return nullptr;
  }
} // namespace pdz
