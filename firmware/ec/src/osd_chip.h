#include "ec_abi.h"
#include <stdint.h>
#ifdef BRIDGE_HOST_TEST
extern uint8_t chip_read(uint8_t reg);
extern void chip_write(uint8_t reg, uint8_t value);
extern void chip_delay(uint32_t ms);
#else
static void chip_delay(uint32_t ms)
{
    ((void (*)(uint32_t))EC_DELAY_MS)(ms);
}

static uint8_t chip_transfer(uint8_t reg, uint8_t value)
{
    void (*pin_write)(uint32_t, uint32_t) = (void *)EC_DIGITAL_WRITE;
    uint8_t (*spi_byte)(void *, uint8_t) = (void *)EC_OSD_SPI_BYTE;
    uint8_t pin = *(volatile uint8_t *)EC_OSD_CS_PIN;
    pin_write(pin, 0);
    spi_byte((void *)EC_OSD_SPI_OBJECT, reg);
    uint8_t result = spi_byte((void *)EC_OSD_SPI_OBJECT, value);
    pin_write(pin, 1);
    return result;
}

static uint8_t chip_read(uint8_t reg)
{
    return chip_transfer(reg, 0xff);
}

static void chip_write(uint8_t reg, uint8_t value)
{
    chip_transfer(reg, value);
}
#endif

static int chip_wait_ready(void)
{
    for (uint32_t i = 0; i < 10000; ++i) {
        if (!(chip_read(0xa0) & 0x20))
            return 1;
    }
    return 0;
}

static int chip_configure(uint8_t pal, uint8_t hos, uint8_t vos)
{
    uint8_t status = chip_read(0xa0), vm0 = chip_read(0x80);

    // The OSD supply can start after the EC. Retry on the next heartbeat without resetting the font.
    if (vm0 == 0xff || (status & 0x60))
        return 0;
    chip_write(2, hos);
    chip_write(3, vos);
    if (chip_read(0x82) != hos || chip_read(0x83) != vos)
        return 0;
    const uint8_t wanted = (pal ? 0x40 : 0) | 0x0c;
    if (vm0 != wanted) {
        chip_write(1, 0x4c);
        for (uint8_t row = 0; row < 16; ++row)
            chip_write(0x10 + row, 2);
        chip_write(4, 0);
        chip_write(0, wanted);
    }
    return chip_read(0x80) == wanted;
}

static int font_read(uint8_t index, uint8_t *data)
{
    if (!chip_wait_ready())
        return 0;
    chip_write(9, index);
    chip_write(8, 0x50);
    chip_delay(1);
    if (!chip_wait_ready())
        return 0;
    for (uint8_t i = 0; i < 54; ++i) {
        chip_write(10, i);
        data[i] = chip_read(0xc0);
    }
    return 1;
}

static int font_write(uint8_t index, const uint8_t *data)
{
    uint8_t verified[54];
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        if (!chip_wait_ready())
            return 0;
        chip_write(9, index);
        for (uint8_t i = 0; i < 54; ++i) {
            chip_write(10, i);
            chip_write(11, data[i]);
        }
        chip_write(8, 0xa0);

        // Readback caught occasional errors in individual font bits. Retry the glyph after the programming delay.
        chip_delay(20);
        if (!chip_wait_ready() || !font_read(index, verified))
            return 0;
        uint8_t mismatch = 0;
        for (uint8_t i = 0; i < 54; ++i)
            mismatch |= data[i] ^ verified[i];
        if (!mismatch)
            return 1;
    }
    return 0;
}

static void draw_glyphs(const uint8_t *data, uint8_t count, uint8_t col, uint8_t row)
{
    // Writing consecutive glyphs treats 0xff as a terminator. Write single cells to support the full font.
    chip_write(4, 0);
    uint16_t address = row * 30 + col;
    while (count--) {
        chip_write(5, address >> 8);
        chip_write(6, address & 255);
        chip_write(7, *data++);
        ++address;
    }
}
