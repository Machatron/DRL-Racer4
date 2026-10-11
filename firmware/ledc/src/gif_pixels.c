#include "ledc_abi.h"
#include <stdint.h>
#define CALL(a, t) ((t)((a) | 1u))
#define BYTE(a) (*(volatile const uint8_t *)(a))
#define HALF(a) (*(volatile const uint16_t *)(a))

static void arm_sample(uint32_t object, unsigned sample, unsigned r, unsigned g, unsigned b)
{
    unsigned count = HALF(object + 2);

    if (count != 52 && count != 60)
        return;
    unsigned brightness = BYTE(LED_BRIGHTNESS);
    r = r * brightness / 255;
    g = g * brightness / 255;
    b = b * brightness / 255;
    // Map the endpoints to the strip ends. Equal width bins left the scan lingering at each end.
    for (unsigned i = 0; i < count; i++)
        if ((i * 12 + (count - 1) / 2) / (count - 1) == sample)
            CALL(LED_SET_PIXEL, void (*)(uint32_t, unsigned, unsigned, unsigned, unsigned))
            (object, i, r, g, b);
}

// The stock callback fills the arms white. Leave the previous frame intact for delta GIFs.
void gif_frame_begin(void)
{
}

void gif_pixel(int x, int y, unsigned r, unsigned g, unsigned b)
{
    if (x < 0 || x >= 42 || y < 0 || y >= 13)
        return;
    if (x < 39) {
        if (*(volatile const uint32_t *)LED_SPINE_MAP_POINTER == LED_RACE_SPINE_TABLE) {
            // The stock table maps x0 and x1 to LED 0. Use only its 25 physical rows at x12..36.
            for (unsigned row = 0; row < 25; row++)
                if ((row * 38 + 12) / 24 == (unsigned)x)
                    CALL(LED_STOCK_GIF_PIXEL, void (*)(int, int, unsigned, unsigned, unsigned))
                    (12 + row, y, r, g, b);
        } else
            CALL(LED_STOCK_GIF_PIXEL, void (*)(int, int, unsigned, unsigned, unsigned))
            (x, y, r, g, b);
    } else if (x == 39) {
        arm_sample(LED_ARM_OBJECT_3, 12 - (unsigned)y, r, g, b);
        arm_sample(LED_ARM_OBJECT_1, 12 - (unsigned)y, r, g, b);
    } else if (x == 40) {
        arm_sample(LED_ARM_OBJECT_2, 12 - (unsigned)y, r, g, b);
        arm_sample(LED_ARM_OBJECT_0, 12 - (unsigned)y, r, g, b);
    }
}
