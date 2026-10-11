#include "ec_abi.h"
#include <stdint.h>
extern void reply_payload(const uint8_t *, uint8_t);
#ifdef BRIDGE_HOST_TEST
extern volatile uint8_t test_uart3_lock;
extern uint8_t test_ec_address;
extern int test_file_send(const uint8_t *, unsigned);
extern int test_file_tx_busy(void);
extern int test_file_wait_reply(const uint8_t *);
#define LOCK test_uart3_lock
#define ADDRESS test_ec_address
#define send_packet test_file_send
#define file_tx_busy test_file_tx_busy
#define file_wait_reply test_file_wait_reply
#else
#define LOCK (*(volatile uint8_t *)EC_UART_MUX_LOCK)
#define ADDRESS (*(volatile uint8_t *)EC_EC_ADDRESS)
#define send_packet ((int (*)(const uint8_t *, unsigned))EC_DAN_SEND_PACKET)
extern int file_tx_busy(void);
extern int file_wait_reply(const uint8_t *);
#endif

void file_forward_request(const uint8_t *data, unsigned size)
{
    uint8_t error[] = {135, 1};
    if (size < 14 || size > 46 || data[1] != 1 || data[4] > 7 || data[4] == 4 || data[5] > 31)
        goto fail;
    const unsigned offset = data[6] | ((unsigned)data[7] << 8);
    const unsigned count = data[8] | ((unsigned)data[9] << 8);
    if (data[4] <= 2) {
        if (!count || count > 32 || offset + count > 65535)
            goto fail;
        if (data[4] == 0) {
            if (size != 14 || data[10] || data[11] || data[12] || data[13])
                goto fail;
        } else if (size != 14 + count || (data[4] == 1 && offset))
            goto fail;
    } else if (data[4] == 3) {
        if (size != 14 || count || !offset)
            goto fail;
    } else if (data[4] == 5) {
        if (size != 22 || count != 8 || offset)
            goto fail;
    } else if (size != 14 || count || offset || data[10] || data[11] || data[12] || data[13])
        goto fail;
    if (LOCK || file_tx_busy()) {
        error[1] = 2;
        goto fail;
    }
    uint8_t packet[5 + 45] = {0};
    packet[2] = 0xe0;
    packet[3] = 0x50 | (ADDRESS & 15);
    packet[4] = ADDRESS;
    for (unsigned i = 1; i < size; i++)
        packet[4 + i] = data[i];
    if (send_packet(packet, size - 1) < 0) {
        error[1] = 2;
        goto fail;
    }
    LOCK = 1;
    int complete = file_wait_reply(data);
    LOCK = 0;
    if (!complete) {
        error[1] = 3;
        goto fail;
    }

    return;
fail:
    reply_payload(error, sizeof(error));
}

void file_forward_response(void *context, const uint8_t *packet)
{
    extern void control_forward_response(const uint8_t *);
    (void)context;
    control_forward_response(packet);
    if (!packet || packet[2] != 0xe1 || packet[3] != ADDRESS ||
        packet[4] != (uint8_t)(0x50 | (ADDRESS & 15)))
        return;
    unsigned size = ((unsigned)packet[0] << 8) | packet[1];
    if (size < 16 || size > 48 || packet[5] != 1)
        return;
    uint8_t answer[49];
    answer[0] = 135;
    for (unsigned i = 0; i < size; i++)
        answer[1 + i] = packet[5 + i];
    reply_payload(answer, size + 1);
}
