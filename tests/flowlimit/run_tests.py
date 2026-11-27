"""Host test of the flow temperature limit.

1. Builds test_flowlimit.cpp with the real HeishaMon/src/common/flowlimit.cpp
   (g++, no Arduino toolchain) and runs it: clamping of each byte, the shift
   against the curve from the datagram or from the received data, the unknown
   state, the cooling minimum, the limit off, the watchdog (rate limit, give up,
   zones, millis() wrap) and the settings (defaults, ranges).
2. Static checks of the glue that cannot run on the host: every main command
   goes through the limit (heatpump_command(), both send_command() variants),
   the received data feeds it, the watchdog is quiet in listen only mode, the
   settings are stored, loaded and on the settings page with the same ranges,
   the MQTT topics are published and documented, CI runs this test. Each check
   is also run on a changed copy, to show that it can fail.

Run: python3 tests/flowlimit/run_tests.py
"""

import re
import shutil
import subprocess
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

executable = OUT / "test_flowlimit"
subprocess.run(
    [compiler, "-std=c++17", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
     "-I" + str(COMMON), str(HERE / "test_flowlimit.cpp"), str(COMMON / "flowlimit.cpp"), "-o", str(executable)],
    check=True,
)
subprocess.run([str(executable)], check=True)


def function(text: str, head: str) -> str:
    """the body of a function that starts in column 0 and ends with a '}' in column 0"""
    start = text.index(head)
    return text[start:text.index("\n}\n", start) + 3]


def read():
    return {
        "commands": (SKETCH / "commands.cpp").read_text(),
        "ino": (SKETCH / "HeishaMon.ino").read_text(),
        "wf": (SKETCH / "webfunctions.cpp").read_text(),
        "wfh": (SKETCH / "webfunctions.h").read_text(),
        "html": (SKETCH / "htmlcode.h").read_text(encoding="utf-8"),
        "header": (COMMON / "flowlimit.h").read_text(),
        "docs": (ROOT / "MQTT-Topics.md").read_text(encoding="utf-8"),
        "ci": (ROOT / ".github/workflows/main.yml").read_text(),
    }


def glue_errors(f: dict) -> list:
    errors = []

    def need(text, pattern, what):
        if not re.search(pattern, text, re.S):
            errors.append(what)

    try:
        hc = function(f["commands"], "unsigned int heatpump_command(")
        need(hc, r"if \(command->type == CMD_FUNC\) \{\s*unsigned int length = command->func\(msg, cmd, log_msg\);\s*if \(length > 0\) flowlimit_apply\(&flowlimit, cmd, length, log_msg, 256\);\s*return length;",
             "heatpump_command(): a command built by its own function (SetCurves) must go through flowlimit_apply()")
        need(hc, r"flowlimit_apply\(&flowlimit, cmd, sizeof\(panasonicSendQuery\), log_msg, 256\);\s*return sizeof\(panasonicSendQuery\);\s*\}\s*$",
             "heatpump_command(): a table command must go through flowlimit_apply() right before it is returned")
        if hc.count("return sizeof(panasonicSendQuery)") != 1:
            errors.append("heatpump_command(): one return of a datagram only, after the limit")
    except ValueError:
        errors.append("heatpump_command() is missing")

    ino = f["ino"]
    senders = [m.start() for m in re.finditer(r"^bool send_command\(byte\* command, int length\) \{", ino, re.M)]
    if len(senders) != 2:
        errors.append("expected two send_command() variants (ESP32 queue, ESP8266 serial)")
    for start in senders:
        body = ino[start:ino.index("\n}\n", start)]
        guard = body.find("flowlimitGuard(command, length);")
        sent = min([i for i in (body.find("xQueueSend("), body.find("heatpumpSerial.write("), body.find("pushCommandBuffer(")) if i >= 0] or [-1])
        if guard < 0 or sent < 0 or guard > sent:
            errors.append("send_command(): flowlimitGuard() must run before the command is queued, buffered or written")
    try:
        guard = function(ino, "void flowlimitGuard(")
        need(guard, r"flowlimit_apply\(&flowlimit, command, length,", "flowlimitGuard() must apply the limit to the command")
        received = function(ino, "void flowlimitReceived(")
        need(received, r"flowlimit_update\(&flowlimit, \(const uint8_t \*\)block, DATASIZE\);\s*if \(heishamonSettings\.listenonly\) return;.*flowlimit_watchdog\(",
             "flowlimitReceived(): update from the data block, no watchdog in listen only mode")
        need(received, r"if \(len > 0\) send_command\(cmd, len\);", "flowlimitReceived(): the correction is sent with send_command()")
    except ValueError:
        errors.append("flowlimitGuard() / flowlimitReceived() are missing")
    need(ino, r"if  \(data\[3\] == 0x10\) \{ //decode the normal data block\s*decode_heatpump_data\([^;]*\);\s*flowlimitReceived\(data\);",
         "readSerial(): every main data block must go to flowlimitReceived(), not only the decoded topics")
    need(ino, r"publishRulesState\(true\);\s*publishFlowLimit\(true\);", "the flow limit topics must be published on (re)connect")
    need(ino, r"publishRulesState\(false\);\s*publishFlowLimit\(false\);", "the flow limit topics must be published from loop()")
    try:
        publish = function(ino, "void publishFlowLimit(")
        need(publish, r'"state", "surface", "heat_max", "cool_min"', "publishFlowLimit(): the four topics")
        need(publish, r'PSTR\("%s/flowlimit/%s"\)', "publishFlowLimit(): topic <base>/flowlimit/<name>")
        need(publish, r"mqtt_client\.publish\(topic, value, true\)", "publishFlowLimit(): retained")
    except ValueError:
        errors.append("publishFlowLimit() is missing")

    # settings: struct, stored, loaded, on the page (both boards) with the ranges of flowlimit.h
    for key, field in (("flowlimit_surface", "flowlimitSurface"), ("flowlimit_heat_max", "flowlimitHeatMax"), ("flowlimit_cool_min", "flowlimitCoolMin")):
        need(f["wfh"], r"uint8_t %s = " % field, f"settingsStruct.{field} is missing")
        try:
            to_json = function(f["wf"], "void settingsToJson(")
            need(to_json, r'jsonDoc\[F\("%s"\)\] = heishamonSettings->%s;' % (key, field), f"settingsToJson(): {key}")
            save = function(f["wf"], "int saveSettings(")
            need(save, r'strcmp\(tmp->name\.c_str\(\), "%s"\) == 0\) \{\s*jsonDoc\[F\("%s"\)\] = atoi\(tmp->value\.c_str\(\)\);' % (key, key), f"saveSettings(): {key} as a number")
            load = function(f["wf"], "void loadSettings(")
            need(load, r'jsonDoc\[F\("%s"\)\]' % key, f"loadSettings(): {key}")
            need(load, r"heishamonSettings->%s = " % field, f"loadSettings(): {field}")
        except ValueError:
            errors.append("settingsToJson() / saveSettings() / loadSettings() are missing")
        if len(re.findall(r"name='%s'" % key, f["html"])) != 2:
            errors.append(f"the settings page needs {key} on both boards")
    try:
        load = function(f["wf"], "void loadSettings(")
        need(load, r"\.isNull\(\) \? FLOWLIMIT_OFF :", "loadSettings(): a missing surface is off (an update changes nothing)")
        need(load, r"flowlimit_normalize\(&flowSurface, &heatMax, &coolMin\);.*flowlimit_configure\(&flowlimit, flowSurface, heatMax, coolMin\);",
             "loadSettings(): normalize, then configure the limit")
        if re.search(r'if \(\s*jsonDoc\[F\("flowlimit_', load):
            errors.append('loadSettings(): "if (jsonDoc[key])" skips 0 = off')
    except ValueError:
        pass
    defines = dict(re.findall(r"#define (FLOWLIMIT_\w+) (\d+)\b", f["header"]))
    for name, low, high in (("flowlimit_heat_max", "FLOWLIMIT_HEAT_MAX_LOW", "FLOWLIMIT_HEAT_MAX_HIGH"), ("flowlimit_cool_min", "FLOWLIMIT_COOL_MIN_LOW", "FLOWLIMIT_COOL_MIN_HIGH")):
        ranges = re.findall(r"name='%s' class='setting-input' min='(\d+)' max='(\d+)'" % name, f["html"])
        if len(ranges) != 2 or any(r != (defines.get(low), defines.get(high)) for r in ranges):
            errors.append(f"settings page: {name} range must be {defines.get(low)}..{defines.get(high)} as in flowlimit.h")
    surfaces = re.findall(r"var m=\{1:(\d+),2:(\d+)\}", f["html"])
    if len(surfaces) != 2 or any(s != (defines.get("FLOWLIMIT_FLOOR_MAX"), defines.get("FLOWLIMIT_RADIATOR_MAX")) for s in surfaces):
        errors.append("settings page: the prefilled maximum per surface must be the one of flowlimit.h")
    if len(re.findall(r"<option value='0'>Off \(no limit\)</option>", f["html"])) != 2:
        errors.append("settings page: surface 0 = off on both boards")

    for topic in ("flowlimit/state", "flowlimit/surface", "flowlimit/heat_max", "flowlimit/cool_min"):
        if topic not in f["docs"]:
            errors.append(f"MQTT-Topics.md must document {topic}")
    if "python3 tests/flowlimit/run_tests.py" not in f["ci"]:
        errors.append("the CI workflow must run tests/flowlimit/run_tests.py")
    return errors


files = read()
errors = glue_errors(files)
if errors:
    raise SystemExit("glue checks failed:\n  " + "\n  ".join(errors))

# each check must be able to fail: break the glue in a copy and expect an error
MUTATIONS = [
    ("commands", "if (length > 0) flowlimit_apply(&flowlimit, cmd, length, log_msg, 256);", ""),
    ("commands", "  flowlimit_apply(&flowlimit, cmd, sizeof(panasonicSendQuery), log_msg, 256);\n", ""),
    ("ino", "  flowlimitGuard(command, length);\n", ""),
    ("ino", "          flowlimitReceived(data);\n", ""),
    ("ino", "  if (heishamonSettings.listenonly) return;\n  uint8_t cmd[FLOWLIMIT_DATAGRAM_SIZE];", "  uint8_t cmd[FLOWLIMIT_DATAGRAM_SIZE];"),
    ("ino", "      publishFlowLimit(true);\n", ""),
    ("ino", "mqtt_client.publish(topic, value, true)", "mqtt_client.publish(topic, value, false)"),
    ("wf", '  jsonDoc[F("flowlimit_cool_min")] = heishamonSettings->flowlimitCoolMin;\n', ""),
    ("wf", 'jsonDoc[F("flowlimit_heat_max")] = atoi(tmp->value.c_str());', 'jsonDoc[F("flowlimit_heat_max")] = tmp->value;'),
    ("wf", "surfaceValue.isNull() ? FLOWLIMIT_OFF :", "surfaceValue.isNull() ? FLOWLIMIT_FLOOR :"),
    ("html", "name='flowlimit_heat_max' class='setting-input' min='20' max='65'", "name='flowlimit_heat_max' class='setting-input' min='20' max='70'"),
    ("html", "var m={1:35,2:55}", "var m={1:40,2:55}"),
    ("docs", "flowlimit/cool_min", "flowlimit/coolmin"),
    ("ci", "python3 tests/flowlimit/run_tests.py", "true"),
]
for name, old, new in MUTATIONS:
    if old not in files[name]:
        raise SystemExit(f"mutation not applicable to {name}: {old!r}")
    changed = dict(files)
    changed[name] = files[name].replace(old, new, 1)
    if not glue_errors(changed):
        raise SystemExit(f"glue check did not notice: {name}: {old!r} -> {new!r}")

print(f"PASS: flow limit glue ({len(MUTATIONS)} mutations caught)")
