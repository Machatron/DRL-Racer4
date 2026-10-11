#pragma once
#include "osd.hpp"

namespace drl
{
class Samba
{
    SerialUSB serial;

    void command(char operation, uint32_t address, uint32_t count, bool argument = true)
    {
        std::ostringstream text;
        text << operation << std::hex << address;
        if (argument)
            text << ',' << count;
        text << '#';
        auto value = text.str();
        serial.send(Bytes(value.begin(), value.end()));
    }

    Bytes receive(size_t count)
    {
        Bytes result(count);
        size_t offset = 0;
        auto deadline = GetTickCount64() + 5000;
        while (offset < count && GetTickCount64() < deadline)
            offset += serial.transfer(result.data() + offset, DWORD(count - offset), false);
        require(offset == count, "EC bootloader reply was incomplete (" + std::to_string(offset) +
                    " / " + std::to_string(count) + "). Retry the font step.");
        return result;
    }

  public:
    explicit Samba(const std::wstring &port) : serial(port, "EC USB")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        serial.send({'N', '#'});
    }

    Bytes memory(uint32_t address, size_t count)
    {
        require(count && count <= 0x4000, "Invalid bootloader read size.");
        Bytes result;
        while (result.size() < count)
        {
            auto remaining = count - result.size();
            auto offset = address + uint32_t(result.size());
            // The bootloader's USB DMA cannot read flash directly. Word reads use a RAM buffer.
            size_t chunk = offset < 0x80000 ? 4 : std::min<size_t>(remaining, 256);
            require(chunk <= remaining, "Unaligned bootloader flash read.");
            command(offset < 0x80000 ? 'w' : 'R', offset, uint32_t(chunk));
            auto data = receive(chunk);
            result.insert(result.end(), data.begin(), data.end());
        }
        return result;
    }

    void ram(uint32_t address, const Bytes &bytes)
    {
        require(address >= 0x20020000 && address <= 0x20024000 &&
                    bytes.size() <= 0x20024000 - address,
                "Font helper write is outside its RAM area.");
        command('S', address, uint32_t(bytes.size()));
        serial.send(bytes);
        require(memory(address, bytes.size()) == bytes, "Font helper RAM readback differs.");
    }

    void call()
    {
        command('G', 0x20020000, 0, false);
    }
};

inline std::string fontBootloader(const Bytes &image)
{
    require(image.size() >= 0x4000, "EC bootloader read is incomplete.");
    auto digest = hash(Bytes(image.begin(), image.begin() + 0x4000));
    if (digest == "76fe563011c2e2d2dd4d1183ea78885bbe7fac376dcf51373d016bcee259f58f")
        return "2.0.0";
    if (digest == "4c7774475065168443fed2276a5b723380976568a4dd00f88dd77e72ec407520")
        return "3.7.0";
    throw std::runtime_error("This EC bootloader has not been checked for font programming.");
}

class BootFont
{
    Samba connection;
    uint32_t sequence = 0;

  public:
    BootFont(const std::wstring &port, const Bytes &firmware, const Bytes &helper)
        : connection(port)
    {
        fontBootloader(firmware);
        require(connection.memory(0, 0x4000) == Bytes(firmware.begin(), firmware.begin() + 0x4000),
                "The USB bootloader does not match the EC readback.");
        require(helper.size() >= 8 && helper.size() <= 0x2000 &&
                    u32(helper, 0) == 0x20024000 && (u32(helper, 4) & 1) &&
                    u32(helper, 4) >= 0x20020009 && u32(helper, 4) < 0x20020000 + helper.size(),
                "Invalid font helper.");
        connection.ram(0x20020000, helper);
        auto reply = request(1);
        require(u32(reply, 84) == 1, "Font helper version differs.");
    }

    Bytes request(uint32_t operation, uint8_t index = 0, const Bytes &glyph = {})
    {
        require(operation >= 1 && operation <= 4 &&
                    (operation == 4 ? glyph.size() == 54 : glyph.empty()),
                "Invalid font helper request.");
        Bytes box(88);
        auto word = [&](size_t offset, uint32_t value)
        {
            for (unsigned i = 0; i < 4; ++i)
                box[offset + i] = uint8_t(value >> (8 * i));
        };
        word(0, 0x464c5244);
        word(4, operation);
        word(8, ++sequence);
        word(16, index);
        std::copy(glyph.begin(), glyph.end(), box.begin() + 24);
        connection.ram(0x20022000, box);
        connection.call();
        auto reply = connection.memory(0x20022000, box.size());
        require(u32(reply, 0) == 0x464c5244 && u32(reply, 8) == sequence &&
                    u32(reply, 80) == sequence && u32(reply, 84) == 1,
                "EC font helper did not complete its request.");
        auto status = u32(reply, 12);
        require(status == 1, status == 2 ? "OSD did not respond. Connect battery power and retry."
                           : status == 3 ? "OSD stayed busy. Retry the font step."
                           : status == 4 ? "OSD character readback differs. Retry the font step."
                                         : "EC font helper rejected the request.");
        return reply;
    }

    Bytes readGlyph(uint8_t index)
    {
        auto reply = request(3, index);
        return Bytes(reply.begin() + 24, reply.begin() + 78);
    }

    void writeGlyph(uint8_t index, const Bytes &glyph)
    {
        request(4, index, glyph);
    }
};
} // namespace drl
