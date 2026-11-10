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
    # delays and single-byte temperatures: no other field in the byte
    "SetBivalentAPStartDelay": (
        67,
        [("30", 31), ("35", 36), ("1", 2), ("254", 255)],
        ["0", "255", "-1"],
    ),
    "SetBivalentAPStopDelay": (
        69,
        [("30", 31), ("35", 36), ("1", 2), ("254", 255)],
        ["0", "255", "-1"],
    ),
    "SetSolarOnDelta": (
        61,
        [("10", 138), ("11", 139), ("-127", 1), ("127", 255), ("0", 128)],
        ["-128", "128"],
    ),
    "SetSolarOffDelta": (
        62,
        [("5", 133), ("6", 134), ("-127", 1), ("127", 255), ("0", 128)],
        ["-128", "128"],
    ),
    "SetSolarFrostProtection": (
        63,
        [("5", 133), ("6", 134), ("-127", 1), ("127", 255), ("0", 128)],
        ["-128", "128"],
    ),
    "SetRoomHolidayShiftTemp": (
        43,
        [("-10", 118), ("-9", 119), ("-127", 1), ("127", 255), ("0", 128)],
        ["-128", "128"],
    ),
}
# Read-modify-write setters for bytes with several fields. Each accepted case is
# (actData overrides, payload, expected value of the byte); the rest of the command must stay the
# empty template. Blocked cases are valid payloads in a state where the cloud did not offer the field.
RMW_SETTERS = {
    # byte 26 bits 5-4 (b01 no, b10 yes); needs bits 3-2 = b11 (advanced parallel)
    "SetBivalentAPHeat": (
        26,
        [
            ({26: 0x5D}, "1", 0x6D),
            ({26: 0x5E}, "1", 0x6E),
            ({26: 0x6E}, "0", 0x5E),
            ({26: 0xDD}, "1", 0xED),
        ],
        [({26: 0x5A}, "1"), ({26: 0x56}, "1"), ({26: 0x51}, "0")],
        ["2", "-1"],
    ),
    # byte 27 bits 5-4 (b01 auto, b10 SG ready, b11 intelligent); bits 1-0, 3-2 and 7-6 stay as read
    "SetBivalentOperatingMode": (
        27,
        [
            ({27: 0x55}, "1", 0x65),
            ({27: 0x55}, "2", 0x75),
            ({27: 0x75}, "0", 0x55),
            ({27: 0x95}, "1", 0xA5),
            ({27: 0x59}, "2", 0x79),
            ({27: 0x76}, "0", 0x56),
        ],
        [],
        ["3", "-1"],
    ),
    # byte 27 bits 7-6 (b01 off, b10 on); needs byte 26 bits 3-2 = b01 (alternative)
    "SetBivalentExternalPump": (
        27,
        [
            ({26: 0x56, 27: 0x55}, "1", 0x95),
            ({26: 0x56, 27: 0x95}, "0", 0x55),
            ({26: 0x56, 27: 0x5D}, "1", 0x9D),
            ({26: 0x56, 27: 0x65}, "1", 0xA5),
        ],
        [({26: 0x5E, 27: 0x55}, "1"), ({26: 0x5A, 27: 0x55}, "1"), ({26: 0x52, 27: 0x55}, "0")],
        ["2", "-1"],
    ),
    # byte 31 low nibble: 0 -> 5 (not used), 1 -> A (heating), 2 -> B (DHW efficiency) or the current DHW value;
    # bits 7-4 stay as read; needs a connected tank (byte 24 bits 1-0 = b10)
    "SetExtraPump": (
        31,
        [
            ({24: 0x56, 31: 0x5A}, "2", 0x5B),
            ({24: 0x56, 31: 0x5B}, "0", 0x55),
            ({24: 0x56, 31: 0x57}, "0", 0x55),
            ({24: 0x56, 31: 0x55}, "1", 0x5A),
            ({24: 0x56, 31: 0x5B}, "1", 0x5A),
            ({24: 0x56, 31: 0x55}, "2", 0x5B),
            ({24: 0x56, 31: 0x57}, "2", 0x57),
            ({24: 0x56, 31: 0x5B}, "2", 0x5B),
            ({24: 0x96, 31: 0x6A}, "0", 0x65),
            ({24: 0x56, 31: 0x6B}, "1", 0x6A),
            ({24: 0x56, 31: 0xDA}, "2", 0xDB),
        ],
        [({24: 0x55, 31: 0x5A}, "2"), ({24: 0x55, 31: 0x5A}, "0"), ({24: 0x57, 31: 0x5A}, "1")],
        ["3", "-1"],
    ),
    # byte 31 bits 3-2 (0 comfort -> 7, 1 efficiency -> B); needs the extra pump on DHW (bits 1-0 = b11) and a tank
    "SetExtraPumpMode": (
        31,
        [
            ({24: 0x56, 31: 0x5B}, "0", 0x57),
            ({24: 0x56, 31: 0x57}, "1", 0x5B),
            ({24: 0x56, 31: 0x5B}, "1", 0x5B),
            ({24: 0x56, 31: 0x6B}, "0", 0x67),
            ({24: 0x56, 31: 0xDB}, "0", 0xD7),
        ],
        [
            ({24: 0x56, 31: 0x5A}, "0"),
            ({24: 0x56, 31: 0x55}, "1"),
            ({24: 0x55, 31: 0x5B}, "0"),
        ],
        ["2", "-1"],
    ),
    # byte 25 bits 3-2 (b01 3 kW, b10 6 kW); bits 1-0 (tank heater type) and 7-4 stay as read; 9 kW is not accepted
    "SetInternalHeaterCapacity": (
        25,
        [
            ({25: 0x25}, "6", 0x29),
            ({25: 0x29}, "3", 0x25),
            ({25: 0x26}, "6", 0x2A),
            ({25: 0x5E}, "3", 0x56),
        ],
        [],
        ["0", "4", "5", "7", "9", "12", "-3"],
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


# last data block from the heat pump, used by the shared-byte setters for the unchanged other bits
ACT_DATA = """
char actData[DATASIZE] = { 0x71, (char)0xc8, 0x01, 0x10 };
void reset_act_data() {
  memset(actData + 4, 0, sizeof(actData) - 4);
  actData[0] = 0x71; actData[1] = (char)0xc8; actData[2] = 0x01; actData[3] = 0x10;
  actData[24] = 0x55; actData[25] = 0x25; actData[26] = 0x5D; actData[27] = 0x55; actData[31] = 0x5A;
  actData[33] = 0x75; actData[34] = (char)0xA7; actData[35] = (char)0x86;
}
struct ActDataInit {
  ActDataInit() { reset_act_data(); }
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
    def setup(overrides):
        return "reset_act_data(); " + " ".join(
            f"actData[{b}] = (char){v};" for b, v in overrides.items()
        )

    for topic, (address, accepted, blocked, rejected) in RMW_SETTERS.items():
        assert f"| {topic} |" in docs
        function = re.search(r'\{\s*"' + topic + r'",\s*(\w+)\s*\}', header)[1]
        for overrides, payload, encoded in accepted:
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    {setup(overrides)}
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 1 && sent.size() == sizeof(panasonicSendQuery));
    for (unsigned int i = 0; i < sent.size(); ++i)
      assert(sent[i] == (i == {address} ? {encoded} : panasonicSendQuery[i]));
    reset_act_data();
  }}
"""
            )
            cases += 1
        for overrides, payload in blocked:
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    {setup(overrides)}
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 0 && sent.empty());
    byte command[256]; memset(command, 0xa5, sizeof(command));
    char log[256];
    assert({function}(payload, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
    reset_act_data();
  }}
"""
            )
            cases += 1
        valid_state = accepted[0][0]
        for payload in rejected + GENERIC_REJECTED:
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    {setup(valid_state)}
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 0 && sent.empty());
    byte command[256]; memset(command, 0xa5, sizeof(command));
    char log[256];
    assert({function}(payload, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
    reset_act_data();
  }}
"""
            )
            cases += 1
        checks.append(
            f"""
  {{ byte command[256]; memset(command, 0xa5, sizeof(command)); char log[256];
    {setup(valid_state)}
    assert({function}(nullptr, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
    reset_act_data();
  }}
"""
        )
        cases += 1
        # no data block yet, or the byte itself still 0: refuse instead of guessing the other bits
        payload = accepted[0][1]
        for broken in ("actData[0] = 0;", f"actData[{address}] = 0;"):
            checks.append(
                f"""
  {{ char topic[] = "{topic}", payload[] = "{payload}";
    {setup(valid_state)} {broken}
    sent.clear(); send_count = 0;
    send_heatpump_command(topic, payload, send_command, log_message, false);
    assert(send_count == 0 && sent.empty());
    byte command[256]; memset(command, 0xa5, sizeof(command));
    char log[256];
    assert({function}(payload, command, log) == 0);
    for (byte value : command) assert(value == 0xa5);
    reset_act_data();
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
