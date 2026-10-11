#include "ec_abi.h"
#include <stdint.h>
#include "drl_led_protocol.h"
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
#define send_packet ((int (*)(const void *, unsigned))EC_DAN_SEND_PACKET)
extern int file_tx_busy(void);
extern int file_wait_reply(const uint8_t *);
#endif

void control_forward_request(const uint8_t *p, unsigned n)
{
    if (n < 5 || n > 64 || p[1] != DRL_RPC_VERSION)
        return;
    uint8_t error[DRL_RPC_REPLY_SIZE] = {DRL_RPC, DRL_RPC_VERSION, p[2], p[3],
                                         p[4],    DRL_BAD_REQUEST};
    if ((p[4] < 130 || p[4] > 134) && p[4] != 137)
        goto fail;
    if (LOCK || file_tx_busy()) {
        error[5] = DRL_BUSY;
        goto fail;
    }
    uint8_t packet[68] = {0};
    packet[2] = 0xe2;
    packet[3] = 0x50 | (ADDRESS & 15);
    packet[4] = ADDRESS;
    for (unsigned i = 1; i < n; i++)
        packet[4 + i] = p[i];
    if (send_packet(packet, n - 1) < 0) {
        error[5] = DRL_BUSY;
        goto fail;
    }
    LOCK = 1;
    int done = file_wait_reply(p);
    LOCK = 0;
    if (done)
        return;
    error[5] = DRL_TIMEOUT;
fail:
    reply_payload(error, sizeof(error));
}

void control_forward_response(const uint8_t *packet)
{
    if (!packet || packet[2] != 0xe3 || packet[3] != ADDRESS ||
        packet[4] != (uint8_t)(0x50 | (ADDRESS & 15)) || packet[0] ||
        packet[1] != DRL_RPC_REPLY_SIZE - 1 || packet[5] != DRL_RPC_VERSION)
        return;
    uint8_t answer[DRL_RPC_REPLY_SIZE] = {DRL_RPC};
    for (unsigned i = 1; i < sizeof(answer); i++)
        answer[i] = packet[4 + i];
    reply_payload(answer, sizeof(answer));
}
