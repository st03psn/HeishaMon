"""Host test of the web login logic (sessions bound to the client address).

Run: python3 tests/webauth/run_tests.py
"""

import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SHIM = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
static uint32_t fake_millis = 0;
static uint32_t millis() { return fake_millis; }
static size_t copy_string(char *dst, const char *src, size_t size) { snprintf(dst, size, "%s", src); return strlen(src); }
#define strlcpy copy_string
struct IPAddress {
  uint32_t value = 0;
  bool fromString(const char *text) {  // like Arduino: first octet in the lowest byte
    unsigned o[4]; char rest;
    if (sscanf(text, "%u.%u.%u.%u%c", &o[0], &o[1], &o[2], &o[3], &rest) != 4) return false;
    for (unsigned x : o) if (x > 255) return false;
    value = o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24);
    return true;
  }
  operator uint32_t() const { return value; }
};
struct settingsStruct {
  char webPassword[40] = "";
  bool webLogin = false;
  char webTrusted[64] = "";
};
static uint32_t ip(const char *text) { IPAddress a; assert(a.fromString(text)); return a; }
"""

TESTS = r"""
int main() {
  settingsStruct s;
  const uint32_t a = ip("192.168.1.20"), b = ip("192.168.1.21"), ha = ip("192.168.1.10");

  // login off: everybody is allowed
  assert(webAuthAllowed(a, &s) && webAuthAllowed(0, &s));

  // login on: nobody without a session
  s.webLogin = true; strcpy(s.webPassword, "secret-1");
  assert(!webAuthAllowed(a, &s) && !webAuthAllowed(b, &s) && !webAuthAllowed(0, &s));

  // wrong, empty, too short, too long passwords
  assert(!webAuthLogin(a, "secret-2", 8, &s));
  assert(!webAuthLogin(a, "", 0, &s));
  assert(!webAuthLogin(a, NULL, 0, &s));
  assert(!webAuthLogin(a, "secret-1x", 9, &s));
  assert(!webAuthLogin(a, "secret-1", 7, &s));
  assert(!webAuthAllowed(a, &s));

  // five wrong passwords lock the login for a minute, also for the right password
  assert(!webAuthLogin(a, "secret-1", 8, &s));
  fake_millis += 59999;
  assert(!webAuthLogin(b, "secret-1", 8, &s));
  fake_millis += 1;
  assert(webAuthLogin(a, "secret-1", 8, &s));

  // the session belongs to that address only, and a request keeps it alive
  assert(webAuthAllowed(a, &s) && !webAuthAllowed(b, &s));
  fake_millis += 1799999; assert(webAuthAllowed(a, &s));
  fake_millis += 1799999; assert(webAuthAllowed(a, &s));
  fake_millis += 1800000; assert(!webAuthAllowed(a, &s));   // idle for 30 minutes: ended
  assert(!webAuthAllowed(a, &s));

  // logout
  assert(webAuthLogin(a, "secret-1", 8, &s) && webAuthAllowed(a, &s));
  webAuthLogout(a);
  assert(!webAuthAllowed(a, &s));

  // an empty password never logs in
  s.webPassword[0] = 0;
  assert(!webAuthLogin(a, "", 0, &s));
  strcpy(s.webPassword, "secret-1");
  fake_millis += 60000;

  // trusted addresses need no login; others still do
  strcpy(s.webTrusted, "192.168.1.10, 192.168.1.11,bad, 300.1.1.1");
  assert(webAuthAllowed(ha, &s) && webAuthAllowed(ip("192.168.1.11"), &s));
  assert(!webAuthAllowed(a, &s) && !webAuthAllowed(ip("192.168.1.1"), &s) && !webAuthAllowed(0, &s));
  s.webTrusted[0] = 0;
  assert(!webAuthAllowed(ha, &s));

  // more logins than session slots: the session that was idle longest is replaced
  for (int i = 0; i < 4; i++) { assert(webAuthLogin(100 + i, "secret-1", 8, &s)); fake_millis += 10; }
  assert(webAuthAllowed(100, &s));              // 100 was used just now, 101 is the oldest
  assert(webAuthLogin(200, "secret-1", 8, &s));
  assert(webAuthAllowed(200, &s) && webAuthAllowed(100, &s) && !webAuthAllowed(101, &s));
  assert(webAuthAllowed(102, &s) && webAuthAllowed(103, &s));

  // host of an Origin header
  char host[32];
  assert(webOriginHost("http://HeishaMon.local", 22, host, sizeof(host)) && strcmp(host, "heishamon.local") == 0);
  assert(webOriginHost("http://192.168.1.50:80", 22, host, sizeof(host)) && strcmp(host, "192.168.1.50") == 0);
  assert(webOriginHost("https://evil.example/x", 22, host, sizeof(host)) && strcmp(host, "evil.example") == 0);
  assert(!webOriginHost("null", 4, host, sizeof(host)));
  assert(!webOriginHost("http://", 7, host, sizeof(host)));
  assert(!webOriginHost("http://a-very-long-host-name-that-does-not-fit.example", 53, host, sizeof(host)));

  // millis() wraps: a session survives the wrap
  fake_millis = 0xFFFFFF00u;
  assert(webAuthLogin(a, "secret-1", 8, &s));
  fake_millis += 0x200;
  assert(webAuthAllowed(a, &s));
  return 0;
}
"""


def main():
    source = (ROOT / "HeishaMon/webfunctions.cpp").read_text()
    logic = source[source.index("#define WEB_SESSIONS") : source.index("int handleLogin(")]
    program = SHIM + logic.replace("unsigned long", "uint32_t") + TESTS
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text(program)
        subprocess.run(["g++", "-std=c++17", "-Wall", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    # the check runs before any route action, and the routes that stay open are the login itself
    ino = (ROOT / "HeishaMon/HeishaMon.ino").read_text()
    uri = ino[ino.index("case WEBSERVER_CLIENT_REQUEST_URI:") : ino.index("case WEBSERVER_CLIENT_ARGS:")]
    assert uri.index("webAuthAllowed(remote, &heishamonSettings)") < uri.index('"/togglelog"')
    assert uri.index("webAuthAllowed(remote, &heishamonSettings)") < uri.index('PSTR("/command")')
    assert uri.index("webAuthAllowed(remote, &heishamonSettings)") < uri.index("} else if (client->route == 110)")
    assert uri.index("webAuthAllowed(remote, &heishamonSettings)") < uri.index('if (strcmp_P((char *)dat, PSTR("/secure")) == 0) {')
    assert uri.count("webAuthAllowed(") == 1
    print("PASS: web login logic (sessions, lock, trusted addresses) and position of the check")


if __name__ == "__main__":
    main()
