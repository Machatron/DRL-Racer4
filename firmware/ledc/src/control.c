#include "ledc_abi.h"
#include <stdint.h>
#include "drl_led_protocol.h"
#define CALL(a, t) ((t)((a) | 1u))
#define BYTE(a) (*(volatile uint8_t *)(a))
#define ADVANCE_ACK ((volatile uint8_t *)LED_ADVANCE_ACK_CACHE)
extern int advance_pattern(void), active_gif_slot(void);
static unsigned active_value(void)
{
    return BYTE(LED_RENDERER_STATE) == 1 ? (unsigned)active_gif_slot() : BYTE(LED_ACTIVE_COLOR);
}

static void snapshot(uint8_t *r)
{
    r[6] = BYTE(LED_RENDERER_STATE);
    r[7] = BYTE(LED_ACTIVE_COLOR);
    r[8] = BYTE(LED_GIF_INDEX);
    r[9] = BYTE(LED_GIF_ENUM_COUNT);
    r[10] = BYTE(LED_OPERATING);
    r[11] = BYTE(LED_ACTIVE_COLOR);
    r[12] = CALL(LED_GET_SPINE_MAP, unsigned (*)(void))();
}

static int name_is(const uint8_t *s, unsigned n, const char *name)
{
    unsigned i = 0;
    while (name[i]) {
        if (i >= n || s[i] != (uint8_t)name[i])
            return 0;
        ++i;
    }
    return i == n;
}

// The stock parser treats missing arguments as zero. States 0 and 6 also use an uninitialized register.
static int setting_valid(const uint8_t *s, unsigned n)
{
    if (n < 7 || n > 58 || s[0] != 's' || s[1] != 'e' || s[2] != 't' || s[3] != ' ')
        return 0;
    unsigned end = 4;
    while (end < n && s[end] != ' ')
        ++end;
    unsigned v[8], count = 0, i = end;
    while (i < n) {
        while (i < n && s[i] == ' ')
            ++i;
        if (i == n)
            break;
        if (count == 8 || s[i] < '0' || s[i] > '9')
            return 0;
        unsigned a = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            a = a * 10 + s[i++] - '0';
            if (a > 65535)
                return 0;
        }
        if (i < n && s[i] != ' ')
            return 0;
        v[count++] = a;
    }
    const uint8_t *name = s + 4;
    unsigned length = end - 4;
    if (name_is(name, length, "custom_color")) {
        if (count != 8 || v[1] < 1 || v[1] > 12)
            return 0;
        for (i = 0; i < 8; i++)
            if (v[i] > 255)
                return 0;
        return 1;
    }
    if (name_is(name, length, "custom_animation"))
        return count == 3 && v[0] <= 69 && v[1] >= 1 && v[1] <= 12 && v[2] <= 255;
    if (count != 1)
        return 0;
    if (name_is(name, length, "color"))
        return v[0] <= 69;
    if (name_is(name, length, "gif_animation_index"))
        return v[0] <= 32;
    if (name_is(name, length, "animation_state"))
        return v[0] >= 1 && v[0] <= 5;
    if (name_is(name, length, "led_overlay"))
        return v[0] >= 1 && v[0] <= 5;
    if (name_is(name, length, "render_from_sd"))
        return v[0] <= 1;
    if (name_is(name, length, "gif_post_blank") || name_is(name, length, "led_brightness"))
        return v[0] <= 255;
    if (name_is(name, length, "animation_frame_rate"))
        return v[0] >= 1;
    if (name_is(name, length, "gif_frame_delay") || name_is(name, length, "led_color_map"))
        return 1;
    return 0;
}

unsigned asset_console(const uint8_t *text, unsigned n)
{
    char line[65];
    uint32_t first[3] = {0}, copy[3] = {0};
    for (unsigned i = 0; i < n; i++)
        line[i] = text[i];
    line[n] = ' ';
    line[n + 1] = 0;

    CALL(LED_STRING_CONSTRUCTOR, void (*)(void *, const char *))(first, line);
    CALL(LED_STRING_COPY, void (*)(void *, const void *))(copy, first);
    unsigned result = CALL(LED_CONSOLE_PARSE, unsigned (*)(void *))(copy);
    CALL(LED_STRING_DESTRUCTOR, void (*)(void *))(copy);
    CALL(LED_STRING_DESTRUCTOR, void (*)(void *))(first);
    return result;
}

int control_service(void *context, const uint8_t *packet)
{
    (void)context;
    if (!packet || packet[2] != 0xe2)
        return 0;
    if (!CALL(LED_IS_DESTINATION, int (*)(unsigned))(packet[3]))
        goto release;
    unsigned n = ((unsigned)packet[0] << 8) | packet[1];
    const uint8_t *p = packet + 5;
    uint8_t answer[5 + DRL_RPC_REPLY_SIZE - 1] = {0};
    uint8_t full[DRL_RPC_REPLY_SIZE] = {DRL_RPC, DRL_RPC_VERSION, 0, 0, 0, DRL_BAD_REQUEST};
    if (n >= 4 && n <= 63 && p[0] == DRL_RPC_VERSION) {
        full[2] = p[1];
        full[3] = p[2];
        full[4] = p[3];
        snapshot(full);

        if (p[3] != 132 && p[3] != 133 && !(p[3] == 137 && n == 6 && (p[4] == 3 || p[4] == 4)))
            ADVANCE_ACK[0] = 0;
        if (p[3] == 133 && n == 4)
            full[5] = DRL_OK;
        else if (p[3] == 130 && n == 5 && p[4] <= 69) {
            full[5] = CALL(LED_SELECT_COLOR_HOOK, int (*)(unsigned))(p[4]) == 0 ? DRL_OK
                                                                                : DRL_VERIFY_FAILED;
        } else if (p[3] == 131 && n == 5 && p[4] >= 1 && p[4] <= 5) {
            uint8_t text[] = "set led_overlay 0";
            text[16] = '0' + p[4];
            full[5] = asset_console(text, 17) == 0 ? DRL_OK : DRL_VERIFY_FAILED;
            if (CALL(LED_GET_SPINE_MAP, unsigned (*)(void))() != p[4])
                full[5] = DRL_VERIFY_FAILED;
        } else if (p[3] == 132 && n == 7) {
            int duplicate = ADVANCE_ACK[0] == 0x73 && ADVANCE_ACK[1] == packet[4];
            for (unsigned i = 1; i < 7; i++)
                if (ADVANCE_ACK[i + 1] != p[i])
                    duplicate = 0;
            // A lost reply must not advance the pattern twice when the FC repeats the request.
            if (duplicate)
                full[5] =
                    (BYTE(LED_RENDERER_STATE) == ADVANCE_ACK[8] && active_value() == ADVANCE_ACK[9])
                        ? DRL_OK
                        : DRL_CONFLICT;
            else if (full[6] != p[4] || full[10] != 1)
                full[5] = DRL_CONFLICT;
            else if ((p[4] == 2 && p[5] <= 69 && p[6] == (p[5] == 69 ? 3 : p[5] + 1)) ||
                     (p[4] == 1 && full[9] && p[5] < full[9] && full[9] <= 32 &&
                      p[6] == (p[5] + 1 < full[9] ? p[5] + 1 : 0))) {
                unsigned current = p[4] == 2 ? full[7] : full[8];
                if (current != p[5])
                    full[5] = DRL_CONFLICT;
                else {

                    full[5] = advance_pattern() == 0 ? DRL_OK : DRL_VERIFY_FAILED;
                    if (full[5] == DRL_OK) {
                        ADVANCE_ACK[1] = packet[4];
                        for (unsigned i = 1; i < 7; i++)
                            ADVANCE_ACK[i + 1] = p[i];
                        ADVANCE_ACK[8] = BYTE(LED_RENDERER_STATE);
                        ADVANCE_ACK[9] = active_value();
                        ADVANCE_ACK[0] = 0x73;
                    }
                }
            }
        } else if (p[3] == 134 && setting_valid(p + 4, n - 4)) {
            if (name_is(p + 4, 16, "set custom_color") && BYTE(LED_RENDERER_STATE) != 2)
                CALL(LED_SET_RENDERER, int (*)(unsigned))(2);
            full[5] = asset_console(p + 4, n - 4) == 0 ? DRL_OK : DRL_VERIFY_FAILED;
        } else if (p[3] == 137 && n == 6 && (p[4] == 3 || p[4] == 4) && p[5] == 0) {

            full[5] = DRL_OK;
        } else if (p[3] == 137 && n == 6 && p[4] == 5 && p[5] < 32) {
            extern int remove_gif(unsigned);
            int result = remove_gif(p[5]);
            full[5] = result == 0   ? DRL_OK
                      : result == 1 ? DRL_BAD_REQUEST
                      : result == 3 ? DRL_BUSY
                                    : DRL_VERIFY_FAILED;
        } else if (p[3] == 137 && n == 6 && p[5] < 32 && (p[4] == 1 || p[4] == 2)) {
            extern int play_gif(unsigned), load_effect(unsigned);
            full[5] =
                (p[4] == 1 ? play_gif(p[5]) : load_effect(p[5])) == 0 ? DRL_OK : DRL_VERIFY_FAILED;
        }
        snapshot(full);
        if (p[3] == 137 && n == 6 && p[4] == 3 && p[5] == 0 && full[5] == DRL_OK) {
            extern int active_gif_slot(void);
            int slot = active_gif_slot();
            full[7] = slot < 0 ? 255 : (uint8_t)slot;
            full[8] = BYTE(LED_BRIGHTNESS);
        }
    }
    if (n == 6 && p[3] == 137 && p[4] == 4 && p[5] == 0 && full[5] == DRL_OK) {
        extern uint32_t uploaded_gifs(void);
        uint32_t bitmap = uploaded_gifs();
        for (unsigned i = 0; i < 4; i++)
            full[6 + i] = bitmap >> (i * 8);
        full[10] = 1;
    }
    answer[2] = 0xe3;
    answer[3] = packet[4];
    answer[4] = BYTE(LED_DAN_ADDRESS);
    for (unsigned i = 1; i < DRL_RPC_REPLY_SIZE; i++)
        answer[4 + i] = full[i];
    CALL(LED_DAN_SEND, void (*)(const void *, unsigned))(answer, DRL_RPC_REPLY_SIZE - 1);
release:
    CALL(LED_RELEASE_PACKET, void (*)(const void *))(packet);
    return 1;
}
