// PDZ Restored: the US Platinum Hits disc (media ID 40C97944, version 0.0.0.15), by translating it at load (route B;
// docs/RELEASE_FIRST_RUN.md "The US Platinum Hits").
//
// Its game code is Title Update 3's, one build earlier (12.09.06.0081 against TU3's 19.09.06.0082): the same 31,886
// functions in the same order; only addresses moved, plus online-only differences. The recompiled program never runs
// the disc's code bytes, so it runs the Platinum disc once the disc's data sections (.rdata / .data / .tls / BINK
// data / the PE and .pdata headers) are laid out exactly as TU3's. The SDK's loader step (XexModule::SetImageTranslator)
// does it right after the image is loaded and its imports are patched, from an address-only table
// (platinum_translation.inc, made by tools/platinum/gen_translation.py), and sets TU3's entry point. The result is
// checked against TU3's own image outside the code sections (SHA-1); anything else is refused at the image check
// (first_run::VerifyImage) with "This version of the game is not supported.".
#pragma once

#include <cstdint>
#include <string>

namespace pdz::platinum
{
  enum class State
  {
    kNone,       // not the Platinum disc's image (or pdz_platinum is off): nothing done
    kTranslated, // the Platinum image, translated; TU3's data checked
    kFailed,     // the Platinum image, but the table or the result didn't check out: the game must not start
  };

  // Registers the loader step with the SDK (before Runtime::LoadXexImage).
  void Register();

  State GetState();
  // One line for the image check / setup.log: what the step did.
  const std::string &Summary();

  // pdz_platinum: the Platinum Hits disc is accepted by setup and translated at load.
  bool Enabled();
} // namespace pdz::platinum
