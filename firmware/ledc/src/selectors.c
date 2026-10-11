#include "ledc_abi.h"
#include <stdint.h>
#define CALL(a, t) ((t)((a) | 1u))
#define BYTE(a) (*(volatile uint8_t *)(a))
#define WORD(a) (*(volatile uint32_t *)(a))
extern void restore_custom_effect(void);

extern int persist_renderer(unsigned state);
extern unsigned journal_color(void);

void select_gif_index(unsigned index)
{
    unsigned previous = WORD(LED_GIF_INDEX);
    WORD(LED_GIF_INDEX) = index;
    if (BYTE(LED_RENDERER_STATE) == 1) {
        if (persist_renderer(1))
            WORD(LED_GIF_INDEX) = previous;
    } else {
        CALL(LED_SAVE_SETTING, void (*)(void *, unsigned))((void *)LED_GIF_INDEX_SETTING, index);
    }
}

int select_color(unsigned color)
{
    if (color > 69)
        color = 3;
    unsigned previous = WORD(LED_ACTIVE_COLOR);
    WORD(LED_ACTIVE_COLOR) = color;
    if (persist_renderer(2)) {
        WORD(LED_ACTIVE_COLOR) = previous;
        return -1;
    }
    // The GIF decoder keeps running after the state changes. Set its abort flag too.
    BYTE(LED_GIF_ABORT) = 1;
    CALL(LED_CLEAR_OUTPUTS, void (*)(void))();

    WORD(LED_ACTIVE_COLOR) = color;
    *(volatile uint16_t *)LED_COLOR_PHASE = 0;
    if (color && CALL(LED_GET_SAVED_COLOR, unsigned (*)(void))() != color)
        CALL(LED_SAVE_COLOR, void (*)(unsigned))(color);
    return 0;
}

unsigned boot_color(void)
{
    unsigned color = journal_color();
    if (!color)
        restore_custom_effect();
    return color;
}

extern int active_gif_slot(void), next_uploaded_gif(unsigned), play_gif(unsigned);
int advance_pattern(void)
{
    if (BYTE(LED_OPERATING) != 1)
        return -1;
    if (BYTE(LED_RENDERER_STATE) == 2) {
        unsigned color = WORD(LED_ACTIVE_COLOR);
        if (color < 69)
            return select_color(color + 1);
        int slot = next_uploaded_gif(0);
        return slot >= 0 ? play_gif((unsigned)slot) : select_color(3);
    }
    if (BYTE(LED_RENDERER_STATE) == 1) {
        int current = active_gif_slot();
        if (current < 0)
            return -1;
        int next = next_uploaded_gif((unsigned)current + 1);
        return next >= 0 ? play_gif((unsigned)next) : select_color(3);
    }
    return -1;
}

void advance_selected(void)
{
    (void)advance_pattern();
}
