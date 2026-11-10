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
    # settings in a byte of their own (no data block needed)
    "SetPoolDelta": (
        58,
        [("0", 128), ("7", 135), ("-127", 1), ("127", 255)],
        ["-128", "128"],
    ),
    # 5..127 degC, and below the heating OFF temperature (actData[83] = 0x98 = 24 degC) while a data block exists
    "SetHeatingOnOutdoorTemp": (
        108,
        [("15", 143), ("14", 142), ("5", 133), ("23", 151)],
        ["4", "24", "25", "127", "-127", "0", "-128", "128"],
    ),
    "SetHeatingOnDelay": (
        109,
        [("30", 2), ("60", 3), ("7620", 255)],
        ["0", "29", "31", "45", "7650"],
    ),
    # 20 min to 3 h in 5 min steps
    "SetDHWBoosterDelay": (
        60,
        [("90", 91), ("95", 96), ("20", 21), ("180", 181)],
        ["0", "1", "15", "19", "21", "92", "185", "254", "255", "-1"],
    ),
    # single-byte temperatures: no other field in the byte
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
# Read-modify-write setters for bytes with several fields (and commands that need a state of the data
# block). Each accepted case is (actData overrides, payload, expected value of the byte); the rest of
# the command must stay the empty template. Blocked cases are valid payloads in a state where the
# command must refuse. Optional last element: {"skip": generic payloads that are valid for this command,
# "gate": byte that must not be 0 (default: the written byte)}.
RMW_SETTERS = {
    # byte 20 bits 3-2 (b01 off, b10 on); the other bits stay as read
    "SetAntiFreezeMode": (
        20,
        [
            ({20: 0x6A}, "0", 0x66),
            ({20: 0x66}, "1", 0x6A),
            ({20: 0xEA}, "0", 0xE6),
            ({20: 0x2A}, "1", 0x2A),
        ],
        [],
        ["2", "-1"],
    ),
    # byte 27 bits 1-0 (b01 disabled, b10 enabled); SG ready bits 3-2, bivalent bits 7-4 stay as read
    "SetDemandControlEnable": (
        27,
        [
            ({27: 0x55}, "1", 0x56),
            ({27: 0x56}, "0", 0x55),
            ({27: 0x5D}, "1", 0x5E),
            ({27: 0x95}, "1", 0x96),
            ({27: 0xAA}, "0", 0xA9),
        ],
        [],
        ["2", "-1"],
    ),
    # byte 30 bits 1-0 (b01 no, b10 yes); the other bits stay as read
    "SetDHWDefrost": (
        30,
        [
            ({30: 0x5A}, "0", 0x59),
            ({30: 0x59}, "1", 0x5A),
            ({30: 0x5E}, "0", 0x5D),
            ({30: 0x1A}, "0", 0x19),
        ],
        [],
        ["2", "-1"],
    ),
    # byte 31 bits 5-4 (b01 no, b10 yes); bits 7-6 are sent as 0 (cloud frame 0x2A for read 0x6A), low nibble as read
    "SetStaticPressure": (
        31,
        [
            ({31: 0x5A}, "1", 0x2A),
            ({31: 0x6A}, "0", 0x1A),
            ({31: 0x6A}, "1", 0x2A),
            ({31: 0x5B}, "1", 0x2B),
            ({31: 0x57}, "0", 0x17),
            ({31: 0xDA}, "1", 0x2A),
            ({31: 0x55}, "0", 0x15),
        ],
        [],
        ["2", "-1"],
    ),
    # byte 33 low nibble, minutes / 5 + 1; the high nibble (stage 2) comes from the data block and must be 1..15
    "SetEfficiencyStage1Time": (
        33,
        [
            ({}, "25", 0x76),
            ({}, "20", 0x75),
            ({}, "0", 0x71),
            ({}, "60", 0x7D),
            ({33: 0x85}, "25", 0x86),
        ],
        [({33: 0x05}, "25"), ({33: 0x0F}, "20")],
        ["-5", "3", "65", "70", "75"],
    ),
    "SetEfficiencyStage2Time": (
        33,
        [
            ({}, "35", 0x85),
            ({}, "30", 0x75),
            ({}, "0", 0x15),
            ({}, "60", 0xD5),
            ({33: 0x76}, "35", 0x86),
        ],
        [({33: 0x70}, "35"), ({33: 0x70}, "0")],
        ["-5", "3", "65", "70", "75"],
    ),
    # byte 34 low nibble, 0..30 min; the high nibble (anti-stick bits) is sent as 0 like the cloud frame
    # (write 0x07 where the read frame shows 0xA7), but a data block with a nonzero byte is still required
    "SetEfficiencyStage3Time": (
        34,
        [
            ({}, "15", 0x04),
            ({}, "30", 0x07),
            ({}, "0", 0x01),
            ({34: 0x67}, "15", 0x04),
            ({34: 0x67}, "30", 0x07),
            ({34: 0x0F}, "20", 0x05),
        ],
        [],
        ["-5", "3", "35"],
    ),
    # byte 35 nibbles, K / 0.5 + 8; the other nibble comes from the data block and must be 2..14
    "SetSensorOffsetRemoteTh1": (
        35,
        [
            ({}, "1", 0xA6),
            ({}, "0.5", 0x96),
            ({}, "0", 0x86),
            ({}, "-3", 0x26),
            ({}, "3.0", 0xE6),
            ({}, "-0.5", 0x76),
            ({35: 0x8E}, "1", 0xAE),
            ({35: 0x82}, "-3", 0x22),
        ],
        [({35: 0x80}, "1"), ({35: 0x81}, "1"), ({35: 0x8F}, "1")],
        ["-3.5", "3.5", "4", "-4", "0.25", "1.", ".5", "1.55", "10"],
        {"skip": {"1.0"}},
    ),
    "SetSensorOffsetOutdoor": (
        35,
        [
            ({}, "-1", 0x86),
            ({}, "2", 0x8C),
            ({}, "0", 0x88),
            ({}, "-3", 0x82),
            ({}, "3", 0x8E),
            ({}, "0.5", 0x89),
            ({35: 0xE6}, "-1.5", 0xE5),
            ({35: 0x26}, "3", 0x2E),
        ],
        [({35: 0x06}, "1"), ({35: 0x16}, "1"), ({35: 0xF6}, "1")],
        ["-3.5", "3.5", "4", "-4", "0.25", "1.", ".5", "1.55", "10"],
        {"skip": {"1.0"}},
    ),
    # byte 26 bits 5-4 (b01 no, b10 yes); needs bits 3-2 = b11 (advanced parallel) and bivalent control = yes (bits 1-0 = b10)
    "SetBivalentAPHeat": (
        26,
        [
            ({26: 0x5E}, "1", 0x6E),
            ({26: 0x6E}, "0", 0x5E),
            ({26: 0xDE}, "1", 0xEE),
        ],
        [
            ({26: 0x5A}, "1"),
            ({26: 0x56}, "1"),
            ({26: 0x52}, "0"),
            ({26: 0x5D}, "1"),  # bivalent control = no
            ({26: 0x5C}, "1"),
        ],
        ["2", "-1"],
    ),
    # byte 27 bits 5-4 (b01 auto, b10 SG ready, b11 intelligent); bits 1-0, 3-2 and 7-6 stay as read; needs bivalent = yes
    "SetBivalentOperatingMode": (
        27,
        [
            ({26: 0x5E, 27: 0x55}, "1", 0x65),
            ({26: 0x5E, 27: 0x55}, "2", 0x75),
            ({26: 0x5E, 27: 0x75}, "0", 0x55),
            ({26: 0x5E, 27: 0x95}, "1", 0xA5),
            ({26: 0x5E, 27: 0x59}, "2", 0x79),
            ({26: 0x5E, 27: 0x76}, "0", 0x56),
            ({26: 0x56, 27: 0x55}, "1", 0x65),
        ],
        [({26: 0x5D, 27: 0x55}, "1"), ({26: 0x5C, 27: 0x55}, "1"), ({26: 0x5F, 27: 0x55}, "1")],
        ["3", "-1"],
    ),
    # byte 27 bits 7-6 (b01 off, b10 on); needs byte 26 bits 3-2 = b01 (alternative) and bivalent = yes
    "SetBivalentExternalPump": (
        27,
        [
            ({26: 0x56, 27: 0x55}, "1", 0x95),
            ({26: 0x56, 27: 0x95}, "0", 0x55),
            ({26: 0x56, 27: 0x5D}, "1", 0x9D),
            ({26: 0x56, 27: 0x65}, "1", 0xA5),
        ],
        [
            ({26: 0x5E, 27: 0x55}, "1"),
            ({26: 0x5A, 27: 0x55}, "1"),
            ({26: 0x52, 27: 0x55}, "0"),
            ({26: 0x55, 27: 0x55}, "1"),  # alternative, but bivalent control = no
            ({26: 0x54, 27: 0x55}, "1"),
        ],
        ["2", "-1"],
    ),
    # bivalent delays, byte 67 / 69, minutes + 1 in 5 min steps; need bivalent control = yes
    "SetBivalentAPStartDelay": (
        67,
        [
            ({26: 0x5E}, "30", 31),
            ({26: 0x5E}, "35", 36),
            ({26: 0x5E}, "5", 6),
            ({26: 0x5E}, "250", 251),
            ({26: 0x56}, "35", 36),
        ],
        [({26: 0x5D}, "35"), ({26: 0x5C}, "35"), ({26: 0x5F}, "35")],
        ["0", "1", "4", "7", "254", "255", "-5"],
        {"gate": 26},
    ),
    "SetBivalentAPStopDelay": (
        69,
        [
            ({26: 0x5E}, "30", 31),
            ({26: 0x5E}, "35", 36),
            ({26: 0x5E}, "5", 6),
            ({26: 0x5E}, "250", 251),
            ({26: 0x56}, "35", 36),
        ],
        [({26: 0x5D}, "35"), ({26: 0x5C}, "35"), ({26: 0x5F}, "35")],
        ["0", "1", "4", "7", "254", "255", "-5"],
        {"gate": 26},
    ),
    # byte 31 low nibble: 0 -> 5 (not used), 1 -> A (heating), 2 -> B (DHW efficiency) or 7 if already DHW comfort;
    # bits 5-4 stay as read, bits 7-6 are sent as 0 (cloud frames 0x1A/0x1B/0x17/0x15 for reads 0x5A/0x5B/0x57/0x55);
    # needs a connected tank (byte 24 bits 1-0 = b10) and one of the four observed low nibbles 5, A, B, 7
    "SetExtraPump": (
        31,
        [
            ({24: 0x56, 31: 0x5A}, "2", 0x1B),
            ({24: 0x56, 31: 0x5B}, "0", 0x15),
            ({24: 0x56, 31: 0x57}, "0", 0x15),
            ({24: 0x56, 31: 0x55}, "1", 0x1A),
            ({24: 0x56, 31: 0x5B}, "1", 0x1A),
            ({24: 0x56, 31: 0x55}, "2", 0x1B),
            ({24: 0x56, 31: 0x57}, "2", 0x17),
            ({24: 0x56, 31: 0x5B}, "2", 0x1B),
            ({24: 0x96, 31: 0x6A}, "0", 0x25),
            ({24: 0x56, 31: 0x6B}, "1", 0x2A),
            ({24: 0x56, 31: 0xDA}, "2", 0x1B),
            ({24: 0x56, 31: 0x1A}, "0", 0x15),
        ],
        [
            ({24: 0x55, 31: 0x5A}, "2"),
            ({24: 0x55, 31: 0x5A}, "0"),
            ({24: 0x57, 31: 0x5A}, "1"),
            # low nibble not one of the four observed values: nothing is sent, in particular not the current nibble again
            ({24: 0x56, 31: 0x5F}, "2"),
            ({24: 0x56, 31: 0x53}, "2"),
            ({24: 0x56, 31: 0x5F}, "0"),
            ({24: 0x56, 31: 0x59}, "1"),
            ({24: 0x56, 31: 0x58}, "2"),
            ({24: 0x56, 31: 0x50}, "0"),
            ({24: 0x56, 31: 0x56}, "1"),
        ],
        ["3", "-1"],
    ),
    # byte 31 bits 3-2 (0 comfort -> 7, 1 efficiency -> B); needs the extra pump on DHW (bits 1-0 = b11) and a tank
    "SetExtraPumpMode": (
        31,
        [
            ({24: 0x56, 31: 0x5B}, "0", 0x17),
            ({24: 0x56, 31: 0x57}, "1", 0x1B),
            ({24: 0x56, 31: 0x5B}, "1", 0x1B),
            ({24: 0x56, 31: 0x6B}, "0", 0x27),
            ({24: 0x56, 31: 0xDB}, "0", 0x17),
        ],
        [
            ({24: 0x56, 31: 0x5A}, "0"),
            ({24: 0x56, 31: 0x55}, "1"),
            ({24: 0x55, 31: 0x5B}, "0"),
            ({24: 0x56, 31: 0x5F}, "0"),
            ({24: 0x56, 31: 0x53}, "1"),
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
# commands that existed in earlier drafts and must stay out: unverified side effects or value mapping
REMOVED_COMMANDS = ("SetZoneType", "SetHeatingWaterMode", "SetLiquidType")

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
unsigned long fake_millis = 0;
unsigned long millis() { return fake_millis; }
"""


# last data block from the heat pump, used by the shared-byte setters for the unchanged other bits
ACT_DATA = """
char actData[DATASIZE] = { 0x71, (char)0xc8, 0x01, 0x10 };
void reset_act_data() {
  fake_millis += 100000; // everything remembered by earlier commands has expired
  memset(actData + 4, 0, sizeof(actData) - 4);
  actData[0] = 0x71; actData[1] = (char)0xc8; actData[2] = 0x01; actData[3] = 0x10;
  actData[20] = 0x6A; actData[30] = 0x5A; actData[83] = 0x98;
  actData[24] = 0x55; actData[25] = 0x25; actData[26] = 0x5D; actData[27] = 0x55; actData[31] = 0x5A;
  actData[33] = 0x75; actData[34] = (char)0xA7; actData[35] = (char)0x86;
}
struct ActDataInit {
  ActDataInit() { reset_act_data(); }
} actDataInit;
"""
RUN_HELPER = r"""
static bool run_command(const char *topicName, const char *payloadText) {
  char topic[64], payload[64];
  strcpy(topic, topicName); strcpy(payload, payloadText);
  sent.clear(); send_count = 0;
  send_heatpump_command(topic, payload, send_command, log_message, false);
  return send_count == 1;
}
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
    def setup(overrides):
        return "reset_act_data(); " + " ".join(
            f"actData[{b}] = (char){v};" for b, v in overrides.items()
        )

    for topic, (address, accepted, blocked, rejected, *options) in RMW_SETTERS.items():
        options = options[0] if options else {}
        gate = options.get("gate", address)
        skip_generic = options.get("skip", set())
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
        for payload in rejected + [p for p in GENERIC_REJECTED if p not in skip_generic]:
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
        for broken in ("actData[0] = 0;", f"actData[{gate}] = 0;"):
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
    # --- removed commands, numbering of Modbus IDs / SET numbers / register table -----------------
    modbus_map = (ROOT / "HeishaMon/ModbusRegisterMap.h").read_text()
    modbus_doc = (ROOT / "Modbus-Register-Mapping.md").read_text()
    for name in REMOVED_COMMANDS:
        for text in (header, docs, modbus_map, modbus_doc, source):
            assert name not in text, f"{name} must not appear in the draft"
        checks.append(f'  assert(!run_command("{name}", "0") && !run_command("{name}", "1"));\n')
        cases += 1
    ids = [(int(i), n) for i, n in re.findall(r'\{(\d+), "(\w+)", (?:true|false)\}', modbus_map)]
    new_ids = [(i, n) for i, n in ids if i >= 55]
    assert [i for i, _ in new_ids] == list(range(55, 55 + len(new_ids))), "Modbus IDs must be contiguous"
    command_names = set(re.findall(r'\{\s*"(Set\w+)",\s*\w+\s*\}', header))
    assert {n for _, n in new_ids} <= command_names, "every Modbus entry needs a command"
    for i, name in new_ids:
        assert f"SET{i + 1} | {name} |" in docs, f"SET{i + 1} row missing or misnumbered for {name}"
        assert f"| {4999 + i} | `{name}` |" in modbus_doc, f"register row missing for {name}"
    assert len(re.findall(r"^SET(?:5[6-9]|[6-9]\d) \|", docs, re.M)) == len(new_ids)
    assert f"| {4999 + 55 + len(new_ids)} |" not in modbus_doc, "no register row without a command"
    cases += 3 + len(new_ids)

    # --- sequences (shadow of the bytes sent by shared-byte commands) -------------------------------
    sequences = r"""
  { // two nibble commands in a row on byte 33: the second one keeps the first
    reset_act_data();
    assert(run_command("SetEfficiencyStage1Time", "25") && sent[33] == 0x76);
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x86);
    assert(run_command("SetEfficiencyStage1Time", "30") && sent[33] == 0x87);
    assert(actData[33] == 0x75); // actData itself is never changed
  }
  { // the memory expires after 30 s: back to the data block
    reset_act_data();
    assert(run_command("SetEfficiencyStage1Time", "25") && sent[33] == 0x76);
    fake_millis += 29999;
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x86);
    reset_act_data();
    assert(run_command("SetEfficiencyStage1Time", "25") && sent[33] == 0x76);
    fake_millis += 30000;
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x85);
  }
  { // bivalent operating mode, then external pump (both byte 27)
    reset_act_data(); actData[26] = (char)0x56;
    assert(run_command("SetBivalentOperatingMode", "1") && sent[27] == 0x65);
    assert(run_command("SetBivalentExternalPump", "1") && sent[27] == 0xA5);
    assert(run_command("SetDemandControlEnable", "1") && sent[27] == 0xA6);
    assert(run_command("SetBivalentOperatingMode", "0") && sent[27] == 0x96);
  }
  { // both sensor offsets (byte 35)
    reset_act_data();
    assert(run_command("SetSensorOffsetRemoteTh1", "1") && sent[35] == 0xA6);
    assert(run_command("SetSensorOffsetOutdoor", "-1.5") && sent[35] == 0xA5);
    assert(actData[35] == (char)0x86);
  }
  { // byte 31: extra pump, static pressure and extra pump mode in a row, bits 7-6 always 0
    reset_act_data(); actData[24] = (char)0x56; actData[31] = (char)0x5A;
    assert(run_command("SetExtraPump", "2") && sent[31] == 0x1B);
    assert(run_command("SetStaticPressure", "1") && sent[31] == 0x2B);
    assert(run_command("SetExtraPumpMode", "0") && sent[31] == 0x27);
    assert(run_command("SetStaticPressure", "0") && sent[31] == 0x17);
  }
  { // the remembered byte is also used for the state checks of other commands
    reset_act_data(); actData[24] = (char)0x56; actData[31] = (char)0x5B;
    assert(run_command("SetExtraPump", "0") && sent[31] == 0x15);
    assert(!run_command("SetExtraPumpMode", "0")); // extra pump is not on DHW any more, although actData still says so
    reset_act_data(); actData[26] = (char)0x5E; actData[27] = (char)0x55;
    assert(run_command("SetBivalentAPHeat", "1") && sent[26] == 0x6E);
    assert(run_command("SetBivalentAPStartDelay", "35") && sent[67] == 36);
  }
  { // a rejected command, a setter call that only validates (Modbus) and a command for another byte leave no memory
    reset_act_data();
    assert(!run_command("SetEfficiencyStage1Time", "3"));
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x85);
    reset_act_data();
    { byte command[256]; char log[256]; char payload[] = "25";
      assert(set_efficiency_stage1_time(payload, command, log) > 0 && command[33] == 0x76); }
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x85);
    reset_act_data();
    assert(run_command("SetEfficiencyStage1Time", "25") && sent[33] == 0x76);
    assert(run_command("SetSensorOffsetRemoteTh1", "1") && sent[35] == 0xA6);
    assert(run_command("SetHeatingOnDelay", "60") && sent[109] == 3);
    assert(run_command("SetEfficiencyStage3Time", "15") && sent[34] == 0x04);
    assert(run_command("SetEfficiencyStage2Time", "35") && sent[33] == 0x86);
  }
  { // SetHeatingOnOutdoorTemp: below the heating OFF temperature while the data block reports it, otherwise only 5..127
    reset_act_data();
    assert(run_command("SetHeatingOnOutdoorTemp", "23") && sent[108] == 151);
    assert(!run_command("SetHeatingOnOutdoorTemp", "24"));
    actData[83] = 0; // not reported
    assert(run_command("SetHeatingOnOutdoorTemp", "30") && sent[108] == 158);
    reset_act_data(); actData[0] = 0; // no data block
    assert(run_command("SetHeatingOnOutdoorTemp", "30") && sent[108] == 158);
    assert(!run_command("SetHeatingOnOutdoorTemp", "4"));
  }
  { // shared-byte commands never send a byte they could not complete: no data block, byte 0
    reset_act_data(); actData[0] = 0;
    assert(!run_command("SetEfficiencyStage3Time", "15") && !run_command("SetBivalentAPStartDelay", "35"));
    assert(!run_command("SetAntiFreezeMode", "0") && !run_command("SetDHWDefrost", "0"));
    reset_act_data(); actData[34] = 0; actData[35] = 0; actData[33] = 0;
    assert(!run_command("SetEfficiencyStage3Time", "15") && !run_command("SetSensorOffsetOutdoor", "1"));
    assert(!run_command("SetSensorOffsetRemoteTh1", "1") && !run_command("SetEfficiencyStage1Time", "25"));
    reset_act_data();
  }
"""
    checks.append(sequences)
    cases += 10

    program = (
        SHIM
        + header
        + ACT_DATA
        + template
        + additions
        + "\n".join(stubs)
        + dispatcher
        + RUN_HELPER
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
