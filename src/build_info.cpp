// build_info.cpp - the build's time and source commit (pdz_build_info.h, written by cmake/pdz_build_info.cmake at
// build time), for the startup line in the log.
#include "pdz_build_info.h"

const char *PdzBuildCommit()
{
  return PDZ_BUILD_COMMIT;
}

const char *PdzBuildTime()
{
  return PDZ_BUILD_TIME;
}
