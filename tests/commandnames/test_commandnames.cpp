// Host test for commandNamesJsonPart() (HeishaMon/commandnames.h), the streamed
// body of /commandnames. Prints the joined output for optionalPCB off and on;
// run_tests.py checks it against the names in commands.h.
#include "Arduino.h"
#include <cassert>
#include <string>
#define strncpy_P std::strncpy
#include "../../HeishaMon/commands.h"
#include "generated_stubs.h"

// commandnames.h uses the tables from commands.h, so it must come after it
#include "../../HeishaMon/commandnames.h"

static std::string joined(bool optionalPCB, unsigned int *parts) {
  std::string out;
  char buf[40];
  unsigned int index = 0, len;
  while ((len = commandNamesJsonPart(index, optionalPCB, buf, sizeof(buf))) >
         0) {
    assert(len == std::strlen(buf));
    out.append(buf, len);
    index++;
    assert(index < 1000);
  }
  *parts = index;
  return out;
}

int main() {
  const unsigned int nrCommands = sizeof(commands) / sizeof(commands[0]);
  const unsigned int nrOptional =
      sizeof(optionalCommands) / sizeof(optionalCommands[0]);
  unsigned int parts;

  std::string off = joined(false, &parts);
  assert(parts ==
         nrCommands + 1); // one piece per name plus the closing bracket
  std::string on = joined(true, &parts);
  assert(parts == nrCommands + nrOptional + 1);

  char buf[40];
  // past the end nothing is written, so the web server ends the chunked
  // response
  assert(commandNamesJsonPart(nrCommands + 1, false, buf, sizeof(buf)) == 0);
  assert(commandNamesJsonPart(nrCommands + nrOptional + 5, true, buf,
                              sizeof(buf)) == 0);
  // a too small buffer is truncated, never overrun, and the returned length
  // matches it
  char small[6];
  assert(commandNamesJsonPart(0, false, small, sizeof(small)) == 5 &&
         std::strlen(small) == 5);
  assert(commandNamesJsonPart(0, false, small, 0) == 0);

  std::printf("%s\n%s\n", off.c_str(), on.c_str());
  return 0;
}
