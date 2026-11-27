"""Host test of the boot button and the setup hotspot.

1. Builds test_setupap.cpp with the real HeishaMon/src/common/setupap.cpp
   (g++, no Arduino toolchain) and runs it: short press / factory reset /
   bounce / gaps in the polling, and "should the hotspot be up now?" for
   the factory state, a configured device, the automatic setting, the
   button request with its timeouts, and a WiFi change made on the hotspot.
2. Static checks of the glue that cannot run on the host (HeishaMon.ino,
   webfunctions.cpp, htmlcode.h): the access point is only started where
   setupap_update() was asked, the settings default to "not by itself",
   the WiFi retry does not depend on the hotspot. Each check is also run
   on a changed copy, to show that it can fail.

Run: python3 tests/setupap/run_tests.py
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / "build"
OUT.mkdir(parents=True, exist_ok=True)
SKETCH = ROOT / "HeishaMon"
COMMON = SKETCH / "src" / "common"

compiler = shutil.which("g++")
if not compiler:
    raise SystemExit("g++ is required")

executable = OUT / "test_setupap"
subprocess.run(
    [compiler, "-std=c++17", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
     "-I" + str(COMMON), str(HERE / "test_setupap.cpp"), str(COMMON / "setupap.cpp"), "-o", str(executable)],
    check=True,
)
subprocess.run([str(executable)], check=True)

ino = (SKETCH / "HeishaMon.ino").read_text()
wf = (SKETCH / "webfunctions.cpp").read_text()
wfh = (SKETCH / "webfunctions.h").read_text()
html = (SKETCH / "htmlcode.h").read_text(encoding="utf-8")
module = (COMMON / "setupap.cpp").read_text()


def function(text: str, head: str) -> str:
    """the body of a function that starts in column 0 and ends with a '}' in column 0"""
    start = text.index(head)
    return text[start:text.index("\n}\n", start) + 3]


def glue_errors(ino: str, wf: str, wfh: str, html: str) -> list:
    errors = []

    def need(text, pattern, what):
        if not re.search(pattern, text, re.S):
            errors.append(what)

    sketch = {p.name: p.read_text(encoding="utf-8", errors="replace") for p in SKETCH.glob("*") if p.suffix in (".ino", ".cpp", ".h")}
    sketch["HeishaMon.ino"], sketch["webfunctions.cpp"], sketch["webfunctions.h"], sketch["htmlcode.h"] = ino, wf, wfh, html

    # --- the access point is started in one place only ---
    starts = [(name, m.group(0)) for name, text in sketch.items() for m in re.finditer(r"softAP(Config)?\s*\(", text)]
    if sorted(starts) != [("HeishaMon.ino", "softAP("), ("HeishaMon.ino", "softAPConfig(")]:
        errors.append(f"softAP()/softAPConfig() must only be called once, in setupApStart(): {starts}")
    try:
        start_fn = function(ino, "void setupApStart() {")
        if "softAPConfig(" not in start_fn or 'softAP(_F("HeishaMon-Setup"))' not in start_fn:
            errors.append("setupApStart() must start the access point")
    except ValueError:
        errors.append("setupApStart() is missing")
    # a bare access point mode (ESP32: starts an access point with the default name) is never set
    for name, text in sketch.items():
        for m in re.finditer(r"WiFi\.mode\(([^;]*)\);", text):
            arg = m.group(1)
            if re.search(r"\bWIFI_AP\b|\bWIFI_MODE_AP\b", arg):
                errors.append(f"{name}: WiFi.mode({arg}) sets a bare access point mode")
            if "WIFI_AP_STA" in arg and arg.strip() not in ("WIFI_AP_STA", "apUp ? WIFI_AP_STA : WIFI_STA"):
                errors.append(f"{name}: unexpected WiFi.mode({arg})")
    # every start is behind the decision of setupap_update()
    calls = len(re.findall(r"^\s+setupApStart\(\);", ino, re.M))
    guarded = len(re.findall(r"if \(!apUp && [^\n]*apReason [!=]= SETUPAP_[^\n]*\) \{\n\s+setupApStart\(\);", ino))
    if calls < 4 or calls != guarded:
        errors.append(f"setupApStart() is called {calls} times, {guarded} of them behind 'if (!apUp && ... apReason ...)'")
    # setupWifi() (boot, settings change) never starts it
    try:
        if re.search(r"softAP\s*\(|softAPConfig", function(wf, "void setupWifi(settingsStruct *heishamonSettings) {")):
            errors.append("setupWifi() must not start the access point")
    except ValueError:
        errors.append("setupWifi() not found")

    # --- the decision comes from setupap.cpp, with the settings as they are stored ---
    try:
        wanted = function(ino, "uint8_t setupApWanted(bool netUp, bool ethLinked) {")
    except ValueError:
        wanted = ""
        errors.append("setupApWanted() is missing")
    need(wanted, r"in\.wifi_configured = \(heishamonSettings\.wifi_ssid\[0\] != '\\0'\);", "setupApWanted(): wifi_configured must be 'a WiFi name is stored'")
    need(wanted, r"in\.net_up = netUp;", "setupApWanted(): net_up")
    need(wanted, r"in\.allow_factory = heishamonSettings\.hotspot;", "setupApWanted(): allow_factory must be the setting hotspot")
    need(wanted, r"in\.auto_on_loss = heishamonSettings\.hotspot_auto;", "setupApWanted(): auto_on_loss must be the setting hotspot_auto")
    need(wanted, r"bootbutton_update\(&bootButton, digitalRead\(BOOTPIN\) == LOW, now\)", "setupApWanted() must poll the boot button")
    need(wanted, r"if \(button == BOOTBUTTON_FACTORY_RESET\) \{\s*factoryReset\(\);", "the factory reset must come from bootbutton_update()")
    need(wanted, r"\} else if \(button == BOOTBUTTON_SHORT\) \{\s*switch \(setupap_button\(&setupAp, &in, now\)\)", "a short press must go to setupap_button()")
    need(wanted, r"uint8_t reason = setupap_update\(&setupAp, &in, now\);.*return reason;", "setupApWanted() must return what setupap_update() says")
    if len(re.findall(r"\bfactoryReset\(\);", ino)) != 2 or "checkBootButton" in ino or "bootButtonNotPressed" in ino:
        errors.append("factoryReset() must only be called by setupApWanted() (and the unused doubleResetDetect())")
    for board, call in (("ESP8266", r"setupApWanted\(WiFi\.localIP\(\) \? true : false, false\)"), ("ESP32", r"setupApWanted\(wifiUp \|\| ethUp, ETH\.linkUp\(\)\)")):
        need(ino, r"void check_wifi\(\) \{.{0,700}?uint8_t apReason = " + call + r";\s*bool apUp = setupApIsUp\(\);", f"check_wifi() ({board}) must ask setupApWanted()")
    need(ino, r"void loop\(\) \{\s*//[^\n]*\n\s*webserver_loop\(\);\s*//[^\n]*\n\s*check_wifi\(\);", "loop() must call check_wifi() (boot button, hotspot) every time")
    # a hotspot that is not wanted any more is stopped, on both boards, and always when the network is up
    if len(re.findall(r"if \(apUp && \(apReason == SETUPAP_OFF\)\) \{\s*setupApStop\(\);", ino)) != 2:
        errors.append("check_wifi() must stop a hotspot that is not wanted any more (both boards)")
    need(ino, r"if \(WiFi\.localIP\(\)\) \{  //WiFi connected\s*if \(apUp\) \{[^}]*setupApStop\(\);", "ESP8266: the hotspot must stop when the WiFi is connected")
    need(ino, r"if \(wifiUp \|\| ethUp\) \{.*?if \(apUp\) \{[^}]*setupApStop\(\);", "ESP32: the hotspot must stop when WiFi or ethernet is up")
    # the captive portal DNS only answers while the hotspot is up
    if len(re.findall(r"dnsServer\.processNextRequest\(\)", ino)) != len(re.findall(r"if \(apUp\) \{\s*dnsServer\.processNextRequest\(\);", ino)):
        errors.append("dnsServer.processNextRequest() must only run while the hotspot is up")

    # --- the WiFi retry goes on without the hotspot ---
    need(ino, r"\} else if \(\(\(unsigned long\)\(millis\(\) - lastWifiRetryTimer\) > \(2 \* WIFIRETRYTIMER\)\) && !sending\) \{[^}]*lastWifiRetryTimer = millis\(\);[^}]*"
              r"wifiBeginStrongest\(heishamonSettings\.wifi_ssid, heishamonSettings\.wifi_password\);",
         "ESP8266: without the hotspot the configured WiFi must be retried by the timer alone (and not while an answer of the heat pump is awaited)")
    need(ino, r"if \(WiFi\.getMode\(\) & WIFI_MODE_STA\) \{\s*if \(WiFi\.scanComplete\(\) == WIFI_SCAN_RUNNING\) \{\s*return;[^}]*\}\s*log_message\([^\n]*\n\s*WiFi\.disconnect\(true\);\s*return;\s*\}"
              r".*?if \(heishamonSettings\.wifi_ssid\[0\] != '\\0'\) \{\s*log_message\([^\n]*\n\s*WiFi\.mode\(apUp \? WIFI_AP_STA : WIFI_STA\);\s*delay\(50\);\s*wifiBeginConfigured\(\);",
         "ESP32: the retry must alternate 'STA off' and 'begin again' by the STA mode bit, with or without the hotspot")

    # --- settings: not by itself unless switched on ---
    need(wfh, r"bool hotspot_auto = false;", "settingsStruct: hotspot_auto must default to false")
    need(wf, r'heishamonSettings->hotspot_auto = \( jsonDoc\[F\("hotspot_auto"\)\] == "enabled" \) \? true : false;',
         "loadSettings(): hotspot_auto must only be on when it is stored as enabled (an old config.json has no such key)")
    need(wf, r'jsonDoc\[F\("hotspot_auto"\)\] = String\("disabled"\);', "saveSettings(): an unticked hotspot_auto must be stored as disabled")
    need(wf, r'strcmp\(tmp->name\.c_str\(\), "hotspot_auto"\) == 0\) \{\s*jsonDoc\[F\("hotspot_auto"\)\] = tmp->value;', "saveSettings(): hotspot_auto")
    need(wf, r'jsonDoc\[F\("hotspot_auto"\)\] = heishamonSettings->hotspot_auto \? "enabled" : "disabled";', "settingsToJson(): hotspot_auto")
    if len(re.findall(r"<input type='checkbox' name='hotspot_auto' value='enabled'>", html)) != 2 or len(re.findall(r"<input type='checkbox' name='hotspot' value='enabled'>", html)) != 2:
        errors.append("both settings forms (ESP8266, ESP32) need the checkboxes hotspot and hotspot_auto")
    if re.search(r"name='hotspot(_auto)?'[^>]*checked", html):
        errors.append("the hotspot checkboxes must not be ticked by the page itself")

    # --- hostname field: an old invalid name must not block the form, the check itself stays ---
    need(html, r"name='wifi_hostname' id='wifi_hostname_id' maxlength='32' pattern='\[A-Za-z0-9\]\(\[A-Za-z0-9\\-\]\*\[A-Za-z0-9\]\)\?'", "the hostname field must keep its pattern")
    need(html, r"id='wifi_hostname_msg'", "the hostname field needs a place for its message")
    need(html, r"if\(stored!==''&&!re\.test\(stored\)\)\{\s*el\.removeAttribute\('pattern'\);.*?if\(el\.value===stored\)\{\s*el\.setCustomValidity\(''\);.*?"
               r"\}else if\(re\.test\(el\.value\)\)\{\s*el\.setCustomValidity\(''\);.*?\}else\{\s*el\.setCustomValidity\(\"[^\"]+\"\);",
         "checkStoredHostname(): only the unchanged stored name may pass, a changed one is checked against the pattern")
    if html.count("removeAttribute('pattern')") != 1:
        errors.append("the pattern of the hostname field may only be dropped in checkStoredHostname()")
    need(html, r"\n\s*checkStoredHostname\(\);\s*\n", "the settings page must call checkStoredHostname() after it filled the form")
    need(wf, r"strcmp\(tmp->name\.c_str\(\), \"wifi_hostname\"\) == 0\) \{\s*if \(webauth_hostname_valid\(", "saveSettings() must still validate the hostname")
    return errors


errors = glue_errors(ino, wf, wfh, html)
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
if re.search(r"#include\s*<|WiFi\.|millis\(|digitalRead|pinMode", module):
    sys.exit("FAIL: setupap.cpp must stay free of Arduino calls (it is the part the host tests run)")

# the checks must be able to fail: each change below has to be noticed
mutations = [
    ("an access point started somewhere else",
     lambda i, w, h, t: (i.replace("  setupApLed(apUp); // purple", '  WiFi.softAP("HeishaMon-Setup");\n  setupApLed(apUp); // purple', 1), w, h, t)),
    ("setupWifi() starts the access point",
     lambda i, w, h, t: (i, w.replace("  // the setup hotspot is not started here", '  WiFi.softAP("HeishaMon-Setup");\n  // the setup hotspot is not started here', 1), h, t)),
    ("a bare access point mode",
     lambda i, w, h, t: (i.replace("    WiFi.disconnect(true);\n    return;\n  }\n\n  // Step 2", "    WiFi.disconnect(true);\n    WiFi.mode(WIFI_AP);\n    return;\n  }\n\n  // Step 2", 1), w, h, t)),
    ("a start that does not ask the decision",
     lambda i, w, h, t: (i.replace("  if (!apUp && (apReason != SETUPAP_OFF)) {\n    setupApStart();", "  if (!apUp) {\n    setupApStart();", 1), w, h, t)),
    ("the automatic hotspot follows the old setting",
     lambda i, w, h, t: (i.replace("in.auto_on_loss = heishamonSettings.hotspot_auto;", "in.auto_on_loss = heishamonSettings.hotspot;", 1), w, h, t)),
    ("hotspot_auto on by default",
     lambda i, w, h, t: (i, w, h.replace("bool hotspot_auto = false;", "bool hotspot_auto = true;", 1), t)),
    ("hotspot_auto on unless stored as disabled",
     lambda i, w, h, t: (i, w.replace('jsonDoc[F("hotspot_auto")] == "enabled" ) ? true : false', 'jsonDoc[F("hotspot_auto")] == "disabled" ) ? false : true', 1), h, t)),
    ("the old boot button code",
     lambda i, w, h, t: (i.replace("void loop() {", "void checkBootButton() {\n  factoryReset();\n}\n\nvoid loop() {", 1), w, h, t)),
    ("the ESP8266 retry needs the hotspot again",
     lambda i, w, h, t: (i.replace("> (2 * WIFIRETRYTIMER)) && !sending) {", "> (2 * WIFIRETRYTIMER)) && !sending && (wifistatus == WL_DISCONNECTED)) {", 1), w, h, t)),
    ("the hotspot is not stopped when it is not wanted any more",
     lambda i, w, h, t: (i.replace("if (apUp && (apReason == SETUPAP_OFF)) {\n    setupApStop();", "if (false) {\n    setupApStop();", 1), w, h, t)),
    ("the hostname pattern is dropped for every name",
     lambda i, w, h, t: (i, w, h, t.replace("if(stored!==''&&!re.test(stored)){", "if(true){", 1))),
    ("a hotspot checkbox ticked by the page",
     lambda i, w, h, t: (i, w, h, t.replace("name='hotspot_auto' value='enabled'>", "name='hotspot_auto' value='enabled' checked>", 1))),
]
for what, change in mutations:
    changed = change(ino, wf, wfh, html)
    if changed == (ino, wf, wfh, html):
        sys.exit(f"FAIL: the self-test '{what}' did not change anything")
    if not glue_errors(*changed):
        sys.exit(f"FAIL: the static checks do not notice: {what}")
print(f"PASS: firmware glue of the setup hotspot ({len(mutations)} changed copies were all noticed)")

print("PASS: setup hotspot host tests")
