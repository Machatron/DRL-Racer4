#include "ec_abi.h"
#include <stdint.h>
#include "drl_led_protocol.h"
#define CALL(a, t) ((t)((a) | 1u))
#define CTX ((volatile uint8_t *)EC_DAN_CONTEXT)
#define UART ((void *)EC_LED_UART_OBJECT)
static uint16_t get16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

int file_tx_busy(void)
{
    return *(volatile uint32_t *)(CTX + 8) != 0;
}

// Check the final CRC byte before the stock parser dispatches and frees the packet.
static int completes_reply(const uint8_t *request, unsigned byte)
{
    unsigned state = CTX[0x1e];
    if (state & 0x80) {
        if (byte != 0xba && byte != 0xda)
            return 0;
        state &= 0x7f;
    } else if (byte == 0xba || byte == 0xda)
        return 0;
    if (state != 10)
        return 0;
    const uint8_t *p = *(const uint8_t *volatile *)(CTX + 0x14);
    if (!p)
        return 0;
    unsigned size = ((unsigned)p[0] << 8) | p[1];
    uint8_t address = *(volatile uint8_t *)EC_EC_ADDRESS;
    const int control = request[0] == 136;
    if ((control ? size != 12 || p[2] != 0xe3 : size < 16 || size > 48 || p[2] != 0xe1) ||
        p[3] != address || p[4] != (uint8_t)(0x50 | (address & 15)))
        return 0;
    for (unsigned i = 0; i < (control ? 4u : 5u); i++)
        if (p[5 + i] != request[1 + i])
            return 0;
    unsigned wire = (get16(p + 0x826) << 8) | byte;
    return (wire & 0xffff) ==
           CALL(EC_DAN_CRC16, unsigned (*)(const void *, unsigned, unsigned))(p, size + 5, 0);
}

int file_wait_reply(const uint8_t *request)
{
    // LED output masks RX interrupts. Wait for the driver to consume the wake bytes before sending.
    uint32_t wake = CALL(EC_MILLIS, uint32_t(*)(void))();
    while ((uint32_t)(CALL(EC_MILLIS, uint32_t(*)(void))() - wake) < 20) {
    }
    uint32_t started = CALL(EC_MILLIS, uint32_t(*)(void))();

    const unsigned timeout = request[0] == 136 ? DRL_RPC_DRIVER_WAIT_MS
                             : request[4] == 3 ? DRL_FILE_PUBLISH_WAIT_MS
                                               : 1500;
    while ((uint32_t)(CALL(EC_MILLIS, uint32_t(*)(void))() - started) < timeout) {
        CALL(EC_DAN_TX_PUMP, void (*)(void *))((void *)CTX);
        if (CALL(EC_UART_AVAILABLE, int (*)(void *))(UART)) {
            int byte = CALL(EC_UART_READ, int (*)(void *))(UART);
            if (byte < 0)
                continue;
            int complete = completes_reply(request, (unsigned)byte & 255);
            CALL(EC_DAN_RX_BYTE, void (*)(void *, unsigned))((void *)CTX, (unsigned)byte & 255);
            if (complete)
                return 1;
        }
    }
    return 0;
}
