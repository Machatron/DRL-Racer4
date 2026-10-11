"""Build the LEDC v22 image."""

from pathlib import Path
import os
from link_check import check_sections
import subprocess
import struct
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ledc/src"
OUTPUT = ROOT / "build/ledc_v22"
OUTPUT.mkdir(parents=True, exist_ok=True)
TOOLCHAIN = Path(os.environ["ARM_TOOLCHAIN"]) / "bin"


def run(tool, *args):
    p = subprocess.run(
        [str(TOOLCHAIN / ("arm-none-eabi-" + tool + ".exe")), *map(str, args)],
        cwd=OUTPUT,
        capture_output=True,
        text=True,
    )
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout


base = (ROOT / "baselines/ledc_v22.bin").read_bytes()
assert (
    hashlib.sha256(base).hexdigest()
    == "efb6bd96cecaaeafd0ded02f5c68c30b6c6d7b2af0115f62fd33224808910dbb"
)
out = bytearray(base)
patches = []


def asm(address, body):
    (OUTPUT / "patch.S").write_text(
        ".syntax unified\n.thumb\n.cpu cortex-m4\n.section .text\n" + body + "\n"
    )
    run("gcc", "-c", "-mthumb", "-mcpu=cortex-m4", "patch.S", "-o", "patch.o")
    run(
        "ld",
        "-Ttext",
        hex(address),
        "-e",
        hex(address | 1),
        "patch.o",
        "-o",
        "patch.elf",
    )
    run("objcopy", "-O", "binary", "-j", ".text", "patch.elf", "patch.bin")
    return (OUTPUT / "patch.bin").read_bytes()


def patch(address, data, reason, expected=None):
    if expected is not None:
        assert base[address : address + len(expected)] == expected, (
            hex(address),
            base[address : address + len(expected)].hex(),
            expected.hex(),
        )
    out[address : address + len(data)] = data
    patches.append(
        {
            "address": hex(address),
            "length": len(data),
            "before": base[address : address + len(data)].hex(),
            "after": data.hex(),
            "reason": reason,
        }
    )


for name, address in [
    ("construct_minimum_frame", 0x4E000),
    ("save_apply_map", 0x4E060),
    ("resize_minimum_frame", 0x4E0A0),
    ("blank_tail_before_show", 0x4E120),
]:
    p = SOURCE / (name + ".S")
    code = asm(address, p.read_text())
    limit = {0x4E000: 0x60, 0x4E060: 0x40, 0x4E0A0: 0x80, 0x4E120: 0xEE0}[address]
    assert len(code) <= limit, name
    patch(address, code, name, b"\xff" * len(code))
patch(0x559C, asm(0x559C, "b.w 0x4e0a0"), "minimum physical frame")
patch(0x55A0, bytes.fromhex("00bf"), "remove x8 allocation", bytes.fromhex("c000"))
patch(
    0x77AC,
    asm(0x77AC, "bl 0x4e000"),
    "saved count constructor",
    asm(0x77AC, "bl 0xcf50"),
)
patch(0xAB86, asm(0xAB86, "bl 0x4e060"), "live map", asm(0xAB86, "bl 0x9f2c"))
patch(
    0xCE38,
    asm(0xCE38, "b.w 0x4e120\nnop"),
    "blank inactive tail",
    bytes.fromhex("83682de9f74f"),
)
flags = [
    "-c",
    "-Os",
    "-mthumb",
    "-mcpu=cortex-m4",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-pic",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-fstack-usage",
    "-I" + str(SOURCE),
    "-I" + str(ROOT / "build/betaflight/src/main/io"),
]
objs = []
for name in (
    "service.c",
    "control.c",
    "selectors.c",
    "gif_pixels.c",
    "gif_transport.c",
    "hook.S",
):
    obj = Path(name).stem + ".o"
    run("gcc", *flags, SOURCE / name, "-o", obj)
    objs.append(obj)

run(
    "ld",
    "-T",
    SOURCE / "service.ld",
    "-e",
    "entry",
    "--defsym",
    "memset=0x1577f",
    *objs,
    "-o",
    "service.elf",
)
run("objcopy", "-O", "binary", "-j", ".text", "service.elf", "service.bin")
code = (OUTPUT / "service.bin").read_bytes()
assert len(code) <= 0x4000
patch(0x50000, code, "compiled services", b"\xff" * len(code))
check_sections(run, "service.elf")
(OUTPUT / "symbols.txt").write_text(run("nm", "-n", "service.elf"), encoding="utf-8")

symbols = {
    r.split()[2]: int(r.split()[0], 16)
    for r in run("nm", "service.elf").splitlines()
    if len(r.split()) == 3
}
hooks = [
    (0x86E0, "entry", "b.w"),
    (0x9CA0, "receive_guard_entry", "bl"),
    (0x8E78, "complete_guard_entry", "bl"),
    (0xB45A, "render_with_lease", "bl"),
    (0x4374, "gif_count_entry", "b.w"),
    (0x4508, "gif_name_entry", "b.w"),
    (0x5DB8, "select_color", "b.w"),
    (0x5EA0, "advance_selected", "b.w"),
    (0x6026, "boot_color", "bl"),
    (0x5C86, "boot_color", "bl"),
    (0x4F02, "gif_uart_service", "bl"),
    (0x4A08, "gif_clock", "bl"),
    (0x5DA0, "gif_delay", "bl"),
    (0x5B64, "gif_show_entry", "b.w"),
    (0x9E0C, "persist_renderer", "b.w"),
    (0x9F54, "select_gif_index", "b.w"),
    (0x5C96, "boot_renderer", "bl"),
    (0x66DA, "boot_renderer", "bl"),
]
for address, n, op in hooks:
    patch(address, asm(address, f"{op} 0x{symbols[n]:x}"), n)
for address, name, old in [
    (0x66FC, "gif_pixel", 0x5379),
    (0x66F0, "gif_frame_begin", 0x5A07),
]:
    patch(address, struct.pack("<I", symbols[name] | 1), name, struct.pack("<I", old))
for address in (0x12498, 0x17BFC):
    patch(
        address,
        struct.pack("<I", 0x20013AA0),
        "reserve cache RAM",
        struct.pack("<I", 0x200138EC),
    )
for address, body in ((0xD1B6, "nop\nnop"), (0xD22C, "bx lr\nnop")):
    patch(address, asm(address, body), "bounded storage wait")
for address in (0x4826, 0x64C8, 0x64D0, 0x64D8):
    patch(address, asm(address, "nop\nnop"), "remove blocking debug print")
patch(0xAB20, bytes.fromhex("00e2"), "brightness success path", bytes.fromhex("60e7"))
assert out[:0x4000] == base[:0x4000]
assert out[0x1C000:0x4E000] == base[0x1C000:0x4E000]
(OUTPUT / "LEDC_v22_candidate.bin").write_bytes(out)
(OUTPUT / "manifest.json").write_text(
    json.dumps(
        {
            "state": "OFFLINE_UNVALIDATED",
            "base_sha256": hashlib.sha256(base).hexdigest(),
            "sha256": hashlib.sha256(out).hexdigest(),
            "code_bytes": len(code),
            "patches": patches,
        },
        indent=2,
    )
)
print(
    "Built LEDC v22",
    len(code),
    "bytes of compiled additions; original settings and bootloader preserved",
)
