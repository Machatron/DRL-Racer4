#include "ledc_abi.h"
#include <stdint.h>
#define ORIGINAL(address, type) ((type)((address) | 1u))
#define UART ((void *)LED_UART_OBJECT)
#define COUNTER (*(volatile uint32_t *)LED_RENDER_GUARD)
#define STATE (*(volatile uint8_t *)LED_RENDERER_STATE)
#define ABORT (*(volatile uint8_t *)LED_GIF_ABORT)
#define GIF_TAG 0xa6000000u
#define MASK 0x00ffffffu
#define RECEIVE_MS 200u

static uint32_t millis(void)
{
    return ORIGINAL(LED_MILLIS, uint32_t(*)(void))();
}

// Hook only the decoder polling and delays. Hooking the global delay would allow recursive command dispatch.
void gif_uart_service(void)
{
    for (unsigned n = 0; n < 128; n++) {
        ORIGINAL(LED_UART_POLL, void (*)(void))();
        if (!ORIGINAL(LED_UART_AVAILABLE, int (*)(void *))(UART))
            break;
    }
}

uint32_t gif_clock(void)
{
    gif_uart_service();
    return millis();
}

void gif_delay(uint32_t ms)
{
    uint32_t start = millis();
    do {
        gif_uart_service();
        if (ABORT)
            break;
    } while ((uint32_t)(millis() - start) < ms);
}

uint32_t gif_receive_deadline(void)
{
    return GIF_TAG | ((millis() + RECEIVE_MS) & MASK);
}

int gif_show_allowed(void)
{
    uint32_t value = COUNTER;
    if ((value & ~MASK) != GIF_TAG)
        return 1;
    uint32_t remaining = (value - millis()) & MASK;
    // LED output masks interrupts and can drop UART bytes. Wait until the packet is complete.
    if (STATE == 1 && remaining && remaining <= RECEIVE_MS)
        return 0;
    COUNTER = 0;
    return 1;
}
