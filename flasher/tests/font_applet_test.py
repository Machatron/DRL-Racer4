"""Run after building the flasher. Requires unicorn and capstone."""

import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR
from unicorn.arm_const import UC_ARM_REG_R0


ROOT = Path(__file__).resolve().parents[2]
CODE = 0x20020000
MAILBOX = 0x20022000
RETURN = 0x20010000
PORT = 0x41008000
BINARY = (ROOT / "flasher/build/font_applet.bin").read_bytes()
STACK, ENTRY = struct.unpack_from("<II", BINARY)


class OSD:
    def __init__(self, absent=False, busy=False, drop_write=False):
        self.absent, self.busy, self.drop_write = absent, busy, drop_write
        self.font = bytearray((i * 13 + 7) & 255 for i in range(256 * 54))
        self.shadow = bytearray(54)
        self.registers = {0: 8, 2: 32, 3: 16, 9: 0, 10: 0}
        self.writes = []
        self.commits = 0

    def read(self, address):
        if self.absent:
            return 255
        if address == 0xa0:
            return 0x1a | (0x20 if self.busy else 0)
        if address == 0xc0:
            return self.shadow[self.registers[10]]
        return self.registers.get(address & 0x7f, 0)

    def write(self, address, value):
        self.writes.append((address, value))
        self.registers[address] = value
        index = self.registers[9] * 54
        if address == 11:
            self.shadow[self.registers[10]] = value
        elif address == 8 and value == 0x50:
            self.shadow[:] = self.font[index:index + 54]
        elif address == 8 and value == 0xa0:
            assert not self.registers[0] & 8
            self.commits += 1
            if not self.drop_write:
                self.font[index:index + 54] = self.shadow


class Machine:
    def __init__(self, osd=None):
        self.osd = osd or OSD()
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.mem_map(0x20000000, 0x30000)
        self.cpu.mem_map(PORT, 0x1000)
        self.cpu.mem_write(CODE, BINARY)
        self.direction = [0x10301022, 0x11000031]
        self.output = [0x55008000, 0xaa000220]
        self.config = bytes((i * 3) & 7 for i in range(32))
        for port in range(2):
            self.cpu.mem_write(PORT + port * 0x80, struct.pack("<I", self.direction[port]))
            self.cpu.mem_write(PORT + port * 0x80 + 0x10, struct.pack("<I", self.output[port]))
            self.cpu.mem_write(PORT + port * 0x80 + 0x40, self.config)
        self.initial = self.direction[:], self.output[:]
        self.bits, self.command, self.value, self.miso = 0, 0, 0, 1
        self.mmio = 0
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write)
        self.cpu.hook_add(UC_HOOK_MEM_READ, self.read, begin=PORT, end=PORT + 0xff)
        # Skip only busy delay loops. SPI edges and the 2000 poll limit still execute.
        disassembler = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        for offset in range(8, len(BINARY) - 4, 2):
            pair = list(disassembler.disasm(BINARY[offset:offset + 4], CODE + offset))
            if len(pair) == 2 and pair[0].mnemonic == "subs" and pair[1].mnemonic == "bne":
                if pair[1].op_str == hex(pair[0].address).replace("0x", "#0x"):
                    reg = pair[0].op_str.split(",")[0]
                    if reg.startswith("r") and pair[0].op_str.endswith("#1"):
                        self.cpu.hook_add(UC_HOOK_CODE, self.delay, UC_ARM_REG_R0 + int(reg[1:]),
                                          begin=pair[0].address, end=pair[0].address)

    @staticmethod
    def delay(cpu, address, size, register):
        cpu.reg_write(register, 1)

    def read(self, cpu, access, address, size, value, user):
        self.mmio += 1
        if address == PORT + 0x20:
            cpu.mem_write(address, struct.pack("<I", self.miso << 14))

    def write(self, cpu, access, address, size, value, user):
        if 0x20000000 <= address < 0x20030000:
            assert MAILBOX <= address < MAILBOX + 88 or 0x20023000 <= address < STACK
            return
        assert PORT <= address < PORT + 0x100
        self.mmio += 1
        port, offset = divmod(address - PORT, 0x80)
        if 0x40 <= offset < 0x60:
            assert (port, offset - 0x40) in {(0, 12), (0, 13), (0, 14), (0, 15), (1, 9), (1, 5)}
            return
        allowed = 0xf000 if port == 0 else 0x220
        assert value & ~allowed == 0
        if offset in (4, 8):
            self.direction[port] = (self.direction[port] & ~value if offset == 4
                                    else self.direction[port] | value)
            cpu.mem_write(PORT + port * 0x80, struct.pack("<I", self.direction[port]))
        elif offset in (0x14, 0x18):
            previous = self.output[port]
            self.output[port] = previous & ~value if offset == 0x14 else previous | value
            cpu.mem_write(PORT + port * 0x80 + 0x10, struct.pack("<I", self.output[port]))
            if port == 1 and value == 1 << 9:
                if not self.output[1] & (1 << 9):
                    self.bits = self.command = self.value = 0
                elif not previous & (1 << 9) and self.bits == 16 and self.command < 0x80:
                    self.osd.write(self.command, self.value)
            if (port == 0 and not previous & (1 << 13) and self.output[0] & (1 << 13)
                    and not self.output[1] & (1 << 9)):
                assert self.output[0] & (1 << 15) and self.output[1] & (1 << 5)
                bit = (self.output[0] >> 12) & 1
                if self.bits < 8:
                    self.command = (self.command << 1) | bit
                    self.miso = 1
                elif self.bits < 16:
                    self.value = (self.value << 1) | bit
                    self.miso = (self.osd.read(self.command) >> (15 - self.bits)) & 1
                self.bits += 1
        else:
            raise AssertionError(hex(address))

    def request(self, command, index=0, glyph=b"", magic=0x464c5244):
        box = bytearray(88)
        struct.pack_into("<5I", box, 0, magic, command, 42, 0, index)
        box[24:24 + len(glyph)] = glyph
        self.cpu.mem_write(MAILBOX, bytes(box))
        self.cpu.reg_write(UC_ARM_REG_SP, STACK)
        self.cpu.reg_write(UC_ARM_REG_LR, RETURN | 1)
        self.cpu.emu_start(ENTRY, RETURN, count=3000000)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == RETURN
        assert self.cpu.reg_read(UC_ARM_REG_SP) == STACK
        assert (self.direction, self.output) == self.initial
        for port in range(2):
            assert self.cpu.mem_read(PORT + port * 0x80 + 0x40, 32) == self.config
        reply = bytes(self.cpu.mem_read(MAILBOX, 88))
        assert struct.unpack_from("<II", reply, 80) == (42, 1)
        return struct.unpack_from("<I", reply, 12)[0], reply[24:78]


for command, index, magic, result in [(1, 0, 0x464c5244, 1), (0, 0, 0x464c5244, 5),
                                      (4, 256, 0x464c5244, 5), (3, 0, 0, 5)]:
    machine = Machine()
    assert machine.request(command, index, magic=magic)[0] == result
    assert machine.mmio == 0

for index in (0, 127, 255):
    machine = Machine()
    original = bytes(machine.osd.font)
    assert machine.request(2)[0] == 1
    assert machine.request(3, index) == (1, original[index * 54:(index + 1) * 54])
    assert machine.osd.font == original and machine.osd.commits == 0
    glyph = bytes(i ^ 0x5a for i in range(54))
    assert machine.request(4, index, glyph)[0] == 1
    assert machine.request(3, index) == (1, glyph)
    assert machine.osd.font[:index * 54] == original[:index * 54]
    assert machine.osd.font[(index + 1) * 54:] == original[(index + 1) * 54:]
    assert machine.osd.commits == 1 and machine.osd.registers[0] == 8

for osd, result in [(OSD(absent=True), 2), (OSD(busy=True), 3), (OSD(drop_write=True), 4)]:
    machine = Machine(osd)
    assert machine.request(4, 17, bytes(54))[0] == result
    assert osd.registers[0] == 8


for name, start, pointer_literal in [("ec_v21.bin", 0x1c34, 0x1d24),
                                      ("ec_v23.bin", 0x1c60, 0x1d50),
                                      ("ec_v25.bin", 0x1c60, 0x1d50)]:
    baseline = (ROOT / "firmware/baselines" / name).read_bytes()
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.mem_map(0, len(baseline))
    cpu.mem_write(0, baseline)
    cpu.mem_map(0x20000000, 0x30000)
    cpu.mem_write(CODE, BINARY)
    pointer = struct.unpack_from("<I", baseline, pointer_literal)[0]
    cpu.mem_write(pointer, struct.pack("<I", CODE))
    box = bytearray(88)
    struct.pack_into("<3I", box, 0, 0x464c5244, 1, 57)
    cpu.mem_write(MAILBOX, bytes(box))
    cpu.reg_write(UC_ARM_REG_SP, 0x2000c000)
    cpu.emu_start(start | 1, start + 0x20, count=1000)
    assert cpu.reg_read(UC_ARM_REG_PC) == start + 0x20
    assert cpu.reg_read(UC_ARM_REG_SP) == 0x2000c000
    assert struct.unpack("<I", cpu.mem_read(MAILBOX + 12, 4))[0] == 1
    assert struct.unpack("<I", cpu.mem_read(MAILBOX + 80, 4))[0] == 57
    assert cpu.mem_read(0, len(baseline)) == baseline
