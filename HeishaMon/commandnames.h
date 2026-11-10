#ifndef COMMANDNAMES_H
#define COMMANDNAMES_H

// Names of the commands the /command route accepts, as a JSON array for the
// console page (route /commandnames). The array is written one piece per call
// so the response can be streamed without a buffer for the whole list: piece 0
// is `["FirstName"`, the following pieces are `,"Name"`, the piece after the
// last name is `]`, and any later index returns 0 (nothing left). The names are
// read from the firmware's own commands[] table, followed by optionalCommands[]
// when the optional PCB is enabled (the only case /command accepts them).
//
// Include after commands.h, and from one source file only: the command tables
// are defined in commands.h with internal linkage, so this uses the copy the
// including file already has.
static unsigned int commandNamesJsonPart(unsigned int index, bool optionalPCB,
                                         char *buf, unsigned int buflen) {
  const unsigned int nrCommands = sizeof(commands) / sizeof(commands[0]);
  const unsigned int nrOptional =
      optionalPCB ? sizeof(optionalCommands) / sizeof(optionalCommands[0]) : 0;
  const char *name = NULL; // points into PROGMEM on ESP8266
  int len = 0;

  if (buflen == 0) {
    return 0;
  }
  if (index < nrCommands) {
    name = commands[index].name;
  } else if (index < nrCommands + nrOptional) {
    name = optionalCommands[index - nrCommands].name;
  } else if (index == nrCommands + nrOptional) {
    len = snprintf(buf, buflen, "]");
  }
  if (name != NULL) {
    char tmp[sizeof(commands[0].name)];
    strncpy_P(tmp, name, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = snprintf(buf, buflen, "%s\"%s\"", index == 0 ? "[" : ",", tmp);
  }
  if (len < 0) {
    return 0;
  }
  return ((unsigned int)len < buflen) ? (unsigned int)len : buflen - 1;
}

#endif
