"""Host test of the boot button and the setup hotspot.

1. Builds test_setupap.cpp with the real HeishaMon/src/common/setupap.cpp
   (g++, no Arduino toolchain) and runs it: short press / factory reset /
   bounce / gaps in the polling, and "should the hotspot be up now?" for
   the factory state, a configured device, the automatic setting, the
   button request with its timeouts, new WiFi data as a request, and the
   WiFi retry (when, which step, how long it waits for the heat pump line).
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
            if "WIFI_AP_STA" in arg and arg.strip() != "WIFI_AP_STA":
                errors.append(f"{name}: unexpected WiFi.mode({arg})")
    # every start is behind the decision of setupap_update()
    calls = len(re.findall(r"^\s+setupApStart\(\);", ino, re.M))
    guard = (r"if \(!apUp && \(\(apReason == SETUPAP_FACTORY\) \|\| \(\(apReason != SETUPAP_OFF\) && "
             r"\(setupApStartNow \|\| wifiretry_ap_may_start\(&wifiRetry, millis\(\)\)\)\)\)\) \{\n\s+setupApStart\(\);")
    guarded = len(re.findall(guard, ino))
    if calls != 2 or guarded != 2:
        errors.append(f"setupApStart() is called {calls} times, {guarded} of them behind the decision of setupap_update() (expected: once per board)")
    # setupWifi() (boot, settings change) never starts it
    try:
        if re.search(r"softAP\s*\(|softAPConfig", function(wf, "void setupWifi(settingsStruct *heishamonSettings) {")):
            errors.append("setupWifi() must not start the access point")
    except ValueError:
        errors.append("setupWifi() not found")

    # --- the decision comes from setupap.cpp, with the settings as they are stored ---
    try:
        wanted = function(ino, "uint8_t setupApWanted(bool netUp) {")
    except ValueError:
        wanted = ""
        errors.append("setupApWanted() is missing")
    need(wanted, r"in\.wifi_configured = \(heishamonSettings\.wifi_ssid\[0\] != '\\0'\);", "setupApWanted(): wifi_configured must be 'a WiFi name is stored'")
    need(wanted, r"in\.net_up = netUp;", "setupApWanted(): net_up")
    need(wanted, r"in\.eth_seen = ethSeen;", "setupApWanted(): eth_seen must be 'ethernet had an address' (a linked cable without an address is not enough)")
    need(wanted, r"in\.allow_factory = heishamonSettings\.hotspot;", "setupApWanted(): allow_factory must be the setting hotspot")
    need(wanted, r"in\.auto_on_loss = heishamonSettings\.hotspot_auto;", "setupApWanted(): auto_on_loss must be the setting hotspot_auto")
    need(wanted, r"bootbutton_update\(&bootButton, digitalRead\(BOOTPIN\) == LOW, now\)", "setupApWanted() must poll the boot button")
    need(wanted, r"if \(button == BOOTBUTTON_FACTORY_RESET\) \{\s*factoryReset\(\);", "the factory reset must come from bootbutton_update()")
    need(wanted, r"\} else if \(button == BOOTBUTTON_SHORT\) \{\s*switch \(setupap_button\(&setupAp, &in, now\)\)", "a short press must go to setupap_button()")
    need(wanted, r"uint8_t reason = setupap_update\(&setupAp, &in, now\);.*return reason;", "setupApWanted() must return what setupap_update() says")
    if len(re.findall(r"\bfactoryReset\(\);", ino)) != 2 or "checkBootButton" in ino or "bootButtonNotPressed" in ino:
        errors.append("factoryReset() must only be called by setupApWanted() (and the unused doubleResetDetect())")
    for board, call in (("ESP8266", r"setupApWanted\(WiFi\.localIP\(\) \? true : false\)"), ("ESP32", r"setupApWanted\(wifiUp \|\| ethUp\)")):
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

    # --- the WiFi retry: when and how is decided by wifiretry_update(), on both boards ---
    if "lastWifiRetryTimer" in ino or "WIFIRETRYTIMER" in ino:
        errors.append("the WiFi retry timing must come from wifiretry_*() (setupap.cpp), not from a timer in HeishaMon.ino")
    if len(re.findall(r"switch \(wifiretry_update\(&wifiRetry, &retryIn, millis\(\)\)\) \{", ino)) != 2:
        errors.append("check_wifi() must ask wifiretry_update() on both boards")
    need(ino, r"retryIn\.busy = sending;", "ESP8266: the retry must know when an answer of the heat pump is awaited")
    need(ino, r"case WIFIRETRY_BEGIN:\s*log_message\([^\n]*\n\s*wifiBeginStrongest\(heishamonSettings\.wifi_ssid, heishamonSettings\.wifi_password\);",
         "ESP8266: WIFIRETRY_BEGIN must scan and begin again")
    need(ino, r"case WIFIRETRY_BEGIN:\s*log_message\([^\n]*\n\s*WiFi\.disconnect\(false\);[^\n]*\n\s*wifiBeginConfigured\(\);",
         "ESP32: WIFIRETRY_BEGIN must be disconnect(false) and begin again (the station stays on)")
    # the station is only ever switched off where wifiretry_update() asked for it (it only does while the hotspot is up):
    # on the ESP32 "no interface left" stops and deinits the whole WiFi driver
    offs = len(re.findall(r"WiFi\.disconnect\(true\)", ino)) - len(re.findall(r"startupFile\.close\(\);\s*WiFi\.disconnect\(true\);\s*timerqueue_insert\(1, 0, -2\);", ino))  # not the factory reset (restart follows)
    asked = len(re.findall(r"case WIFIRETRY_STA_OFF:\s*log_message\([^\n]*\n\s*WiFi\.disconnect\(true\);", ino))
    if offs != 2 or asked != 2:
        errors.append(f"WiFi.disconnect(true) appears {offs} times in HeishaMon.ino, {asked} of them for WIFIRETRY_STA_OFF (expected: once per board, nowhere else)")
    need(ino, r"void setupApStop\(\) \{[^}]*#if defined\(ESP32\)\s*WiFi\.enableSTA\(true\);[^\n]*\n#endif\s*WiFi\.softAPdisconnect\(true\);",
         "ESP32: setupApStop() must keep the station interface, or stopping the hotspot stops the WiFi driver")
    need(ino, r"bool beginWifi = \(wifiretry_net_up\(&wifiRetry, &retryIn, millis\(\)\) == WIFIRETRY_BEGIN\);.*?"
              r"if \(beginWifi && !wifiUp && \(heishamonSettings\.wifi_ssid\[0\] != '\\0'\)\) \{\s*wifiBeginConfigured\(\);",
         "ESP32: when ethernet comes back while the station is off, the WiFi must be started again")
    need(ino, r"wifiretry_net_up\(&wifiRetry, &retryIn, millis\(\)\);\s*\n\s*// Allow MDNS processing", "ESP8266: wifiretry_net_up() while the WiFi is connected")
    # new WiFi data counts as a hotspot request (a typo must not leave the device unreachable)
    need(ino, r"case -3: \{\s*setupWifi\(&heishamonSettings\);\s*wifiretry_started\(&wifiRetry, millis\(\)\);.*?setupap_wifi_saved\(&setupAp, millis\(\)\);",
         "the WiFi reconnect after new settings (timer -3) must call wifiretry_started() and setupap_wifi_saved()")
    need(html, r"webBodySettingsNewWifiWarning\[\].*?cannot be reached[^<]*, HeishaMon opens the open <strong[^>]*>Heishamon-Setup</strong> hotspot after about 15 seconds, also when you made this change from your network",
         "the page shown after a WiFi change must say that the hotspot comes back when the new WiFi cannot be reached")

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
     lambda i, w, h, t: (i.replace("      WiFi.disconnect(true); // STA off, the hotspot stays", "      WiFi.disconnect(true); // STA off, the hotspot stays\n      WiFi.mode(WIFI_AP);", 1), w, h, t)),
    ("a start that does not ask the decision",
     lambda i, w, h, t: (i.replace("if (!apUp && ((apReason == SETUPAP_FACTORY) || ((apReason != SETUPAP_OFF) && (setupApStartNow || wifiretry_ap_may_start(&wifiRetry, millis()))))) {",
                                   "if (!apUp && (heishamonSettings.wifi_ssid[0] == '\\0')) {", 1), w, h, t)),
    ("the automatic hotspot follows the old setting",
     lambda i, w, h, t: (i.replace("in.auto_on_loss = heishamonSettings.hotspot_auto;", "in.auto_on_loss = heishamonSettings.hotspot;", 1), w, h, t)),
    ("hotspot_auto on by default",
     lambda i, w, h, t: (i, w, h.replace("bool hotspot_auto = false;", "bool hotspot_auto = true;", 1), t)),
    ("hotspot_auto on unless stored as disabled",
     lambda i, w, h, t: (i, w.replace('jsonDoc[F("hotspot_auto")] == "enabled" ) ? true : false', 'jsonDoc[F("hotspot_auto")] == "disabled" ) ? false : true', 1), h, t)),
    ("the old boot button code",
     lambda i, w, h, t: (i.replace("void loop() {", "void checkBootButton() {\n  factoryReset();\n}\n\nvoid loop() {", 1), w, h, t)),
    ("the station switched off without the hotspot (ESP32: WiFi driver deinit)",
     lambda i, w, h, t: (i.replace("      WiFi.disconnect(false); // ends a connection attempt", "      WiFi.disconnect(true); // ends a connection attempt", 1), w, h, t)),
    ("a second place that switches the station off",
     lambda i, w, h, t: (i.replace("  setupApLed(apUp); // purple", "  if (!apUp) WiFi.disconnect(true);\n  setupApLed(apUp); // purple", 1), w, h, t)),
    ("stopping the hotspot leaves the ESP32 without an interface",
     lambda i, w, h, t: (i.replace("  WiFi.enableSTA(true); //", "  //", 1), w, h, t)),
    ("the ESP8266 retry does not know about the heat pump line",
     lambda i, w, h, t: (i.replace("retryIn.busy = sending;", "retryIn.busy = false;", 1), w, h, t)),
    ("a retry timer of its own in check_wifi()",
     lambda i, w, h, t: (i.replace("void loop() {", "unsigned long lastWifiRetryTimer = 0;\n\nvoid loop() {", 1), w, h, t)),
    ("new WiFi data is not a hotspot request",
     lambda i, w, h, t: (i.replace("          setupap_wifi_saved(&setupAp, millis());\n", "", 1), w, h, t)),
    ("ethernet back with the station off: WiFi not started again",
     lambda i, w, h, t: (i.replace("bool beginWifi = (wifiretry_net_up(&wifiRetry, &retryIn, millis()) == WIFIRETRY_BEGIN);", "bool beginWifi = false;\n    wifiretry_net_up(&wifiRetry, &retryIn, millis());", 1), w, h, t)),
    ("a linked ethernet cable counts as configured",
     lambda i, w, h, t: (i.replace("in.eth_seen = ethSeen;", "in.eth_seen = ethSeen || ETH.linkUp();", 1), w, h, t)),
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
