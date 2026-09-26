#include "support/rendered_dataset.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace sphanorama::test {
namespace {

/**
 * The quoting survives a real shell, not a shell we imagine.
 *
 * **Asserted by running one**, because the failure being guarded against is the shell's parse and
 * not the string's contents — a test comparing `Quoted(x)` against an expected literal would pass
 * for an escape that `sh` reads differently from the author. `printf %s` writes the argument back
 * with nothing added, so what comes out is exactly what the shell decided the argument was.
 */
TEST(ShellQuoting, APathWithAnApostropheReachesTheCommandIntact) {
  const std::string awkward = "/home/o'brien/my repo/$(touch pwned)/`id`; rm -rf x";
  const std::string command = "printf %s " + Quoted(awkward);
  std::FILE* pipe = ::popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string seen;
  char chunk[256];
  while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) seen += chunk;
  ASSERT_EQ(::pclose(pipe), 0);
  EXPECT_EQ(seen, awkward);
}

}  // namespace
}  // namespace sphanorama::test
