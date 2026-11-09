"""Test the new setters and real command dispatch with g++ on the host.

Run: python3 Tools/test_commands.py
"""

import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETTERS = {
    "SetHeatToCoolTemp": (
        95,
        [("21", 149), ("-127", 1), ("127", 255), ("0", 128)],
        ["-128", "128"],
    ),
    "SetCoolToHeatTemp": (
        96,
        [("12", 140), ("-127", 1), ("127", 255)],
        ["-128", "128"],
    ),
    "SetRoomOperationMaxTime": (
        97,
        [("90", 4), ("30", 2), ("7620", 255)],
        ["0", "29", "31", "7650"],
    ),
    "SetDHWHeatupMaxTime": (
        98,
        [("105", 106), ("1", 2), ("254", 255)],
        ["0", "255", "-1"],
    ),
    "SetQuietModeSchedule": (7, [("0", 64), ("1", 128)], ["2", "-1"]),
    # bit pairs: payload 0/1 -> b01/b10 in the pair, every other byte stays 0
    "SetAntiFreezeMode": (20, [("0", 4), ("1", 8)], ["2", "-1"]),
    "SetLiquidType": (20, [("0", 64), ("1", 128)], ["2", "-1"]),
    "SetZoneType": (21, [("0", 21), ("1", 25)], ["2", "-1"]),
    "SetDemandControlEnable": (27, [("0", 1), ("1", 2)], ["2", "-1"]),
    "SetHeatingWaterMode": (28, [("0", 1), ("1", 2)], ["2", "-1"]),
    "SetDHWDefrost": (30, [("0", 1), ("1", 2)], ["2", "-1"]),
    "SetStaticPressure": (31, [("0", 16), ("1", 32)], ["2", "-1"]),
    "SetPoolDelta": (
        58,
        [("0", 128), ("7", 135), ("-127", 1), ("127", 255)],
        ["-128", "128"],
    ),
    "SetHeatingOnOutdoorTemp": (
        108,
        [("15", 143), ("14", 142), ("-127", 1), ("127", 255)],
        ["-128", "128"],
    ),
    "SetHeatingOnDelay": (
        109,
        [("30", 2), ("60", 3), ("7620", 255)],
        ["0", "29", "31", "45", "7650"],
    ),
    "SetDHWBoosterDelay": (
        60,
        [("90", 91), ("95", 96), ("1", 2), ("254", 255)],
        ["0", "255", "-1"],
    ),
    # nibble fields: the other nibble comes from ACT_DATA (byte 33 = 0x75, 34 = 0xA7, 35 = 0x86)
    "SetEfficiencyStage1Time": (
        33,
        [("25", 0x76), ("20", 0x75), ("0", 0x71), ("70", 0x7F)],
        ["-5", "3", "75"],
    ),
    "SetEfficiencyStage2Time": (
        33,
        [("35", 0x85), ("30", 0x75), ("0", 0x15), ("70", 0xF5)],
        ["-5", "3", "75"],
    ),
    "SetEfficiencyStage3Time": (
        34,
        [("15", 0xA4), ("30", 0xA7), ("0", 0xA1)],
        ["-5", "3", "35"],
    ),
    "SetSensorOffsetRemoteTh1": (
        35,
        [("1", 0xA6), ("0.5", 0x96), ("0", 0x86), ("-3", 0x26), ("3.0", 0xE6), ("-0.5", 0x76)],
        ["-3.5", "3.5", "4", "-4", "0.25", "1.", ".5", "1.55", "10"],
        {"1.0"},
    ),
    "SetSensorOffsetOutdoor": (
        35,
        [("-1", 0x86), ("2", 0x8C), ("0", 0x88), ("-3", 0x82), ("3", 0x8E), ("0.5", 0x89)],
        ["-3.5", "3.5", "4", "-4", "0.25", "1.", ".5", "1.55", "10"],
        {"1.0"},
    ),
}
NEEDS_DATA = {
    "SetEfficiencyStage1Time",
    "SetEfficiencyStage2Time",
    "SetEfficiencyStage3Time",
    "SetSensorOffsetRemoteTh1",
    "SetSensorOffsetOutdoor",
}

SHIM = r"""
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
using byte = unsigned char;
#define PROGMEM
#define memcpy_P memcpy
#define snprintf_P snprintf
#define PSTR(x) x
std::vector<byte> sent;
int send_count;
bool send_command(byte* data, int length) {
  ++send_count; sent.assign(data, data + length); return true;
}
void log_message(char*) {}
"""


# last data block from the heat pump, used by the nibble setters for the unchanged other nibble
ACT_DATA = """
char actData[DATASIZE] = { 0x71, (char)0xc8, 0x01, 0x10 };
struct ActDataInit {
  ActDataInit() { actData[33] = 0x75; actData[34] = (char)0xA7; actData[35] = (char)0x86; }
} actDataInit;
"""
GENERIC_REJECTED = ["", "abc", "1x", "1.0", " 1", "+1", "1 ", "-", "999999999999999"]


def main():
    header = (ROOT / "HeishaMon/commands.h").read_text()
    source = (ROOT / "HeishaMon/commands.cpp").read_text()
    docs = (ROOT / "MQTT-Topics.md").read_text()
    additions = source[
        source.index("static unsigned int set_integer_field") : source.index(
            "unsigned int set_sterilization_temp"
        )
    ]
    real_functions = set(re.findall(r"unsigned int (\w+)\(", additions))
    stubs = []
    for name, args in re.findall(r"unsigned int (set_\w+)\(([^;]+)\);", header):
        if name not in real_functions:
            stubs.append(f"unsigned int {name}({args}) {{ return 0; }}")
    template = re.search(r"byte panasonicSendQuery\[\].*?\};", source, re.S)[0]
    dispatcher = source[
        source.index("void send_heatpump_command(") : source.index(
            "bool saveOptionalPCB("
        )
    ]
    header = re.sub(r"^#include.*$", "", header, flags=re.M)
    checks = []
    cases = 0
    for topic, (address, accepted, rejected, *skip) in SETTERS.items():
        assert f"| {topic} |" in docs
        function = re.search(r'\{\s*"' + topic + r'",\s*(\w+)\s*\}', header)[1]
        for payload, encoded in accepted:
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 1 && sent.size() == sizeof(panasonicSendQuery));
    for (unsigned int i = 0; i < sent.size(); ++i)
      assert(sent[i] == (i == {address} ? {encoded} : panasonicSendQuery[i]));
  }}
"""
            )
            cases += 1
        for payload in rejected + [
            p for p in GENERIC_REJECTED if p not in (skip[0] if skip else ())
        ]:
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 0 && sent.empty());
    byte command[256]; memset(command, 0xa5, sizeof(command));
    char log[256];
    assert({function}(payload, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
  }}
"""
            )
            cases += 1
        checks.append(
            f"""
  {{ byte command[256]; memset(command, 0xa5, sizeof(command)); char log[256];
    assert({function}(nullptr, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
  }}
"""
        )
        cases += 1
        if topic in NEEDS_DATA:
            payload = accepted[0][0]
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    char saved = actData[0];
    actData[0] = 0; // no data block received yet: refuse instead of guessing the other nibble
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 0 && sent.empty());
    byte command[256]; memset(command, 0xa5, sizeof(command));
    char log[256];
    assert({function}(payload, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
    actData[0] = saved;
  }}
"""
            )
            cases += 1
    program = (
        SHIM
        + header
        + ACT_DATA
        + template
        + additions
        + "\n".join(stubs)
        + dispatcher
    )
    program += "\nint main() {\n" + "".join(checks) + "\n}\n"
    with tempfile.TemporaryDirectory() as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text(program)
        subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print(f"PASS: {cases} setter/dispatch cases (bytes, boundaries, rejected payloads)")


if __name__ == "__main__":
    main()
