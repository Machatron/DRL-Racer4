#include "ledc_abi.h"
#include <stdint.h>

#define MAX_REPLACE 1536u
#define MAX_FILE 65535u
#define MAX_DATA 128u
#define FS ((void *)LED_FILESYSTEM_OBJECT)
#define ORIGINAL(address, type) ((type)((address) | 1u))
#define RENDER_COUNTER (*(volatile uint32_t *)LED_RENDER_GUARD)
#define ANIMATION_STATE (*(volatile uint8_t *)LED_RENDERER_STATE)
#define LEASE_TAG 0xa5000000u
#define LEASE_MASK 0x00ffffffu
#define LEASE_MS 3000u
typedef union {
    uint32_t alignment;
    uint8_t b[396];
} File;

static uint32_t millis(void)
{
    return ORIGINAL(LED_MILLIS, uint32_t(*)(void))();
}

uint32_t receive_render_guard(void)
{
    extern uint32_t gif_receive_deadline(void);
    if (ANIMATION_STATE == 1)
        return gif_receive_deadline();
    uint32_t value = RENDER_COUNTER;
    return ANIMATION_STATE == 2 && (value & ~LEASE_MASK) == LEASE_TAG ? value : 50000u;
}

void complete_render_guard(void)
{
    if (ANIMATION_STATE != 2 || (RENDER_COUNTER & ~LEASE_MASK) != LEASE_TAG)
        RENDER_COUNTER = 0;
}

int render_with_lease(void)
{
    uint32_t value = RENDER_COUNTER;
    if ((value & ~LEASE_MASK) == 0xa6000000u) {
        extern int gif_show_allowed(void);
        if (!gif_show_allowed())
            return -1;

    }
    if ((value & ~LEASE_MASK) == LEASE_TAG) {
        uint32_t remaining = (value - millis()) & LEASE_MASK;
        if (ANIMATION_STATE == 2 && remaining && remaining <= LEASE_MS)
            return -1;
        RENDER_COUNTER = 0;
    }
    return ORIGINAL(LED_RENDER_COLOR, int (*)(void))();
}
enum { OK, BAD_REQUEST, UNAVAILABLE, BUSY, OPEN_FAILED, IO_FAILED, VERIFY_FAILED };

static uint32_t u16(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p)
{
    return u16(p) | (u16(p + 2) << 16);
}

static void put16(uint8_t *p, uint32_t n)
{
    p[0] = n;
    p[1] = n >> 8;
}

static void put32(uint8_t *p, uint32_t n)
{
    put16(p, n);
    put16(p + 2, n >> 16);
}

static uint32_t crc32(const uint8_t *p, unsigned n)
{
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}

static int open_file(File *f, const char *path, unsigned flags)
{
    ORIGINAL(LED_FILE_OPEN, void (*)(void *, void *, const char *, unsigned))(f, FS, path, flags);
    return f->b[0x10] != 0;
}

static void close_file(File *f)
{
    // The stock destructor does not close the file. Call close explicitly or the last write is lost.
    if (f->b[0x10])
        ORIGINAL(LED_FILE_CLOSE, void (*)(void *))(f);
    ORIGINAL(LED_FILE_DESTRUCTOR, void (*)(void *))(f);
}

static uint32_t file_size(File *f)
{
    return ORIGINAL(LED_FILE_SIZE, uint32_t(*)(void *))(f);
}

static uint32_t read_file(File *f, uint8_t *p, unsigned n)
{
    return ORIGINAL(LED_FILE_READ, uint32_t(*)(void *, void *, unsigned))(f, p, n);
}

static uint32_t write_file(File *f, const uint8_t *p, unsigned n)
{
    return ORIGINAL(LED_FILE_WRITE, uint32_t(*)(void *, const void *, unsigned))(f, p, n);
}

// Included here because the asset code uses the private stock File wrapper above.
#include "assets.c"

int file_service(void *context, const uint8_t *packet)
{
    extern int control_service(void *, const uint8_t *);
    if (control_service(context, packet))
        return 1;
    if (!packet || packet[2] != 0xe0)
        return 0;
    if (!ORIGINAL(LED_IS_DESTINATION, int (*)(unsigned))(packet[3])) {
        ORIGINAL(LED_RELEASE_PACKET, void (*)(const void *))(packet);
        return 1;
    }
    const unsigned size = ((unsigned)packet[0] << 8) | packet[1];
    const uint8_t *p = packet + 5;
    uint8_t answer[5 + 16 + MAX_DATA] = {0};
    uint8_t *r = answer + 5;
    answer[2] = 0xe1;
    answer[3] = packet[4];
    answer[4] = *(volatile uint8_t *)LED_DAN_ADDRESS;
    r[0] = 1;
    r[5] = BAD_REQUEST;
    unsigned response_size = 16;
    if (size >= 5 && size <= 13 + MAX_REPLACE) {
        r[1] = p[1];
        r[2] = p[2];
        r[3] = p[3];
        r[4] = p[4];
    }
    if (size < 13 || size > 13 + MAX_REPLACE || p[0] != 1 || p[3] > 7 || p[3] == 4 || p[4] > 31)
        goto reply;
    unsigned offset = u16(p + 5), amount = u16(p + 7);
    put16(r + 8, offset);
    if (p[3] == 0) {
        if (size != 13 || !amount || amount > MAX_DATA || offset > MAX_FILE || u32(p + 9))
            goto reply;
    } else if (p[3] <= 2) {
        if (!amount || size != 13 + amount || amount > MAX_FILE - offset)
            goto reply;
        if (p[3] == 1 && (offset || amount > MAX_REPLACE))
            goto reply;
        if (p[3] == 2 && amount > MAX_DATA)
            goto reply;
        if (crc32(p + 13, amount) != u32(p + 9)) {
            r[5] = VERIFY_FAILED;
            goto reply;
        }
    } else {
        if (p[3] == 3) {
            if (size != 13 || amount || !offset)
                goto reply;
        } else if (p[3] == 5) {
            if (size != 21 || offset || amount != 8)
                goto reply;
            if (crc32(p + 13, 8) != u32(p + 9)) {
                r[5] = VERIFY_FAILED;
                goto reply;
            }
        } else if (size != 13 || offset || amount || u32(p + 9))
            goto reply;
    }
    if (!*(volatile uint8_t *)LED_FILESYSTEM_MOUNTED) {
        r[5] = UNAVAILABLE;
        goto reply;
    }

    if (*(volatile uint8_t *)LED_RENDERER_STATE == 1) {
        r[5] = BUSY;
        goto reply;
    }

    if (ANIMATION_STATE == 2)
        RENDER_COUNTER = LEASE_TAG | ((millis() + LEASE_MS) & LEASE_MASK);
    if (p[3] >= 3) {
        if (p[3] == 3)
            r[5] = publish_gif(p[4], offset, u32(p + 9));
        else if (p[3] == 5)
            r[5] = save_effect(p[4], p + 13);
        else if (p[3] == 6)
            r[5] = load_effect(p[4]);
        else
            r[5] = play_gif(p[4]);
        if (!r[5]) {
            put16(r + 10, amount);
            put32(r + 12, u32(p + 9));
        }
        goto reply;
    }
    char path[] = "/BF00.TMP";
    path[3] = '0' + p[4] / 10;
    path[4] = '0' + p[4] % 10;
    File f;
    if (p[3] == 0) {
        if (!open_file(&f, path, 1)) {
            close_file(&f);
            r[5] = OPEN_FAILED;
            goto reply;
        }
        const unsigned total = file_size(&f);
        if (total > MAX_FILE || offset > total) {
            close_file(&f);
            goto reply;
        }
        put16(r + 6, total);
        if (amount > total - offset)
            amount = total - offset;
        if (offset && !ORIGINAL(LED_FILE_SEEK, int (*)(void *, unsigned))(&f, offset)) {
            close_file(&f);
            r[5] = IO_FAILED;
            goto reply;
        }
        unsigned got = read_file(&f, r + 16, amount);
        close_file(&f);
        if (got != amount) {
            r[5] = IO_FAILED;
            goto reply;
        }
        put16(r + 10, amount);
        put32(r + 12, crc32(r + 16, amount));
        response_size += amount;
        r[5] = OK;
    } else {
        // 0x33 appends even after seeking. Use 0x0b for replacement and 3 for an existing file.
        if (!open_file(&f, path, p[3] == 1 ? 0x0b : 3)) {
            close_file(&f);
            r[5] = OPEN_FAILED;
            goto reply;
        }
        if (p[3] == 2) {
            unsigned total = file_size(&f);
            if (total < offset || total > offset + amount) {
                close_file(&f);
                goto reply;
            }

            if (!ORIGINAL(LED_FILE_SEEK, int (*)(void *, unsigned))(&f, offset)) {
                close_file(&f);
                r[5] = IO_FAILED;
                goto reply;
            }
            uint8_t existing[MAX_DATA];
            unsigned overlap = total - offset;
            if (read_file(&f, existing, overlap) != overlap) {
                close_file(&f);
                r[5] = IO_FAILED;
                goto reply;
            }
            for (unsigned i = 0; i < overlap; i++) {
                if (existing[i] != p[13 + i]) {
                    close_file(&f);
                    r[5] = VERIFY_FAILED;
                    goto reply;
                }
            }
            if (!ORIGINAL(LED_FILE_SEEK, int (*)(void *, unsigned))(&f, offset)) {
                close_file(&f);
                r[5] = IO_FAILED;
                goto reply;
            }
        }
        unsigned wrote = write_file(&f, p + 13, amount);
        close_file(&f);
        if (wrote != amount) {
            r[5] = IO_FAILED;
            goto reply;
        }
        if (!open_file(&f, path, 1)) {
            close_file(&f);
            r[5] = OPEN_FAILED;
            goto reply;
        }
        if (file_size(&f) != offset + amount) {
            close_file(&f);
            r[5] = VERIFY_FAILED;
            goto reply;
        }
        if (offset && !ORIGINAL(LED_FILE_SEEK, int (*)(void *, unsigned))(&f, offset)) {
            close_file(&f);
            r[5] = IO_FAILED;
            goto reply;
        }
        uint8_t block[128];
        unsigned done = 0;
        r[5] = OK;
        while (done < amount) {
            unsigned count = amount - done;
            if (count > sizeof(block))
                count = sizeof(block);
            if (read_file(&f, block, count) != count) {
                r[5] = IO_FAILED;
                break;
            }
            for (unsigned i = 0; i < count; i++)
                if (block[i] != p[13 + done + i]) {
                    r[5] = VERIFY_FAILED;
                    break;
                }
            if (r[5])
                break;
            done += count;
        }
        close_file(&f);
        if (!r[5]) {
            put16(r + 6, offset + amount);
            put16(r + 10, amount);
            put32(r + 12, u32(p + 9));
        }
    }
reply:
    ORIGINAL(LED_DAN_SEND, void (*)(const void *, unsigned))(answer, response_size);
    ORIGINAL(LED_RELEASE_PACKET, void (*)(const void *))(packet);
    return 1;
}
