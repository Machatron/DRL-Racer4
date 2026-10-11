#include <stdint.h>

#define MAILBOX_ADDRESS 0x20022000u
#define MAGIC 0x464c5244u
#define PORT_A 0x41008000u
#define PORT_B 0x41008080u
#define REG32(address) (*(volatile uint32_t *)(address))
#define REG8(address) (*(volatile uint8_t *)(address))

struct Mailbox
{
    uint32_t magic, command, sequence, result, index;
    uint8_t registers[4], glyph[54], padding[2];
    uint32_t completed, version;
};

struct Pin
{
    uint32_t port, bit;
};

// All three EC baselines use PA12 MOSI, PA13 clock, PA14 MISO and PB09 OSD select.
static const struct Pin pins[] = {
    {PORT_B, 9}, {PORT_A, 12}, {PORT_A, 13}, {PORT_A, 14},
    {PORT_A, 15}, {PORT_B, 5}
};

static void delay_us(uint32_t us)
{
    uint32_t ticks = us * 40;
    __asm__ volatile("1: subs %0, %0, #1\n bne 1b" : "+r"(ticks) : : "cc");
}

static void output(unsigned pin, unsigned value)
{
    REG32(pins[pin].port + (value ? 0x18 : 0x14)) = 1u << pins[pin].bit;
}

static uint8_t spi_byte(uint8_t data)
{
    uint8_t received = 0;
    for (unsigned bit = 0; bit < 8; ++bit)
    {
        output(1, data & 0x80);
        data <<= 1;
        delay_us(1);
        output(2, 1);
        delay_us(1);
        received = (received << 1) | ((REG32(PORT_A + 0x20) >> 14) & 1);
        output(2, 0);
    }
    return received;
}

static uint8_t transfer(uint8_t reg, uint8_t value)
{
    output(0, 0);
    delay_us(1);
    spi_byte(reg);
    uint8_t result = spi_byte(value);
    output(0, 1);
    delay_us(1);
    return result;
}

static uint8_t chip_read(uint8_t reg)
{
    return transfer(reg, 0xff);
}

static void chip_write(uint8_t reg, uint8_t value)
{
    transfer(reg, value);
}

static int ready(void)
{
    for (unsigned i = 0; i < 2000; ++i)
    {
        uint8_t status = chip_read(0xa0);
        if (status == 0xff)
            return 0;
        if (!(status & 0x20))
            return 1;
        delay_us(10);
    }
    return 0;
}

static int read_glyph(uint8_t index, volatile uint8_t *glyph)
{
    if (!ready())
        return 0;
    chip_write(9, index);
    chip_write(8, 0x50);
    delay_us(1000);
    if (!ready())
        return 0;
    for (unsigned i = 0; i < 54; ++i)
    {
        chip_write(10, i);
        glyph[i] = chip_read(0xc0);
    }
    return 1;
}

static uint32_t font_command(volatile struct Mailbox *box)
{
    box->registers[0] = chip_read(0x80);
    box->registers[1] = chip_read(0x82);
    box->registers[2] = chip_read(0x83);
    box->registers[3] = chip_read(0xa0);
    if (box->registers[0] == 0xff || box->registers[3] == 0xff)
        return 2;
    if (!ready())
        return 3;
    if (box->command == 2)
        return 1;
    uint8_t vm0 = box->registers[0];
    chip_write(0, vm0 & ~8u);
    uint32_t result = 3;
    if (box->command == 3)
        result = read_glyph(box->index, box->glyph) ? 1 : 3;
    else
    {
        chip_write(9, box->index);
        for (unsigned i = 0; i < 54; ++i)
        {
            chip_write(10, i);
            chip_write(11, box->glyph[i]);
        }
        chip_write(8, 0xa0);
        delay_us(25000);
        uint8_t readback[54];
        if (read_glyph(box->index, readback))
        {
            result = 1;
            for (unsigned i = 0; i < 54; ++i)
                if (readback[i] != box->glyph[i])
                    result = 4;
        }
    }
    chip_write(0, vm0);
    return result;
}

void font_applet(void)
{
    volatile struct Mailbox *box = (void *)MAILBOX_ADDRESS;
    box->result = 5;
    box->version = 1;
    if (box->magic != MAGIC || box->command < 1 || box->command > 4 || box->index > 255)
        goto complete;
    if (box->command == 1)
    {
        box->result = 1;
        goto complete;
    }

    uint8_t config[6];
    uint32_t direction[6], level[6];
    for (unsigned i = 0; i < 6; ++i)
    {
        config[i] = REG8(pins[i].port + 0x40 + pins[i].bit);
        direction[i] = REG32(pins[i].port) & (1u << pins[i].bit);
        level[i] = REG32(pins[i].port + 0x10) & (1u << pins[i].bit);
    }
    // Deselect the IMU and barometer before driving their shared SPI bus.
    for (unsigned i = 0; i < 6; ++i)
    {
        output(i, i != 1 && i != 2);
        REG8(pins[i].port + 0x40 + pins[i].bit) = i == 3 ? 6 : 2;
        REG32(pins[i].port + (i == 3 ? 4 : 8)) = 1u << pins[i].bit;
    }
    box->result = font_command(box);
    for (unsigned i = 0; i < 6; ++i)
    {
        REG32(pins[i].port + 4) = 1u << pins[i].bit;
        output(i, level[i] != 0);
        REG32(pins[i].port + (direction[i] ? 8 : 4)) = 1u << pins[i].bit;
        REG8(pins[i].port + 0x40 + pins[i].bit) = config[i];
    }
complete:
    box->completed = box->sequence;
    __asm__ volatile("dsb" : : : "memory");
}
