"""Build commandnames.h with the real command tables on the host and check the JSON it streams."""

from pathlib import Path
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / "build"
OUT.mkdir(parents=True, exist_ok=True)

commands = (ROOT / "HeishaMon/commands.h").read_text(encoding="utf-8")
# the tables only take the setters' addresses, so empty definitions are enough
stubs = [
    f"unsigned int {sig} {{ return 0; }}"
    for sig in re.findall(r"^unsigned int (set_[^;\n]+);", commands, re.M)
]
(OUT / "generated_stubs.h").write_text("\n".join(stubs) + "\n", encoding="utf-8")


def names(table):
    body = re.search(
        table + r"\[\]\s*PROGMEM\s*=\s*\{(.*?)\n\};", commands, re.S
    ).group(1)
    return re.findall(r'\{\s*"([^"]+)"', body)


main = names(r"const cmdStruct commands")
optional = names(r"const optCmdStruct optionalCommands")

compiler = shutil.which("g++")
if not compiler:
    raise SystemExit("g++ is required")
executable = OUT / "test_commandnames"
for board in ("-DESP8266", "-DESP32"):
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            board,
            "-I" + str(ROOT / "tests/modbus/stubs"),
            "-I" + str(OUT),
            str(HERE / "test_commandnames.cpp"),
            "-o",
            str(executable),
        ],
        check=True,
    )
    output = subprocess.run(
        [str(executable)], check=True, capture_output=True, text=True
    ).stdout.splitlines()
    assert (
        json.loads(output[0]) == main
    ), "optionalPCB off: names differ from commands[]"
    assert (
        json.loads(output[1]) == main + optional
    ), "optionalPCB on: names differ from commands[] + optionalCommands[]"
    print(
        f"PASS: /commandnames JSON ({board[2:]}): {len(main)} commands, {len(optional)} optional PCB commands"
    )
