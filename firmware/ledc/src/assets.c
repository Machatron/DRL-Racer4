#include "ledc_abi.h"
#define BYTE(a) (*(volatile uint8_t *)(a))
#define WORD(a) (*(volatile uint32_t *)(a))

#define GIF_SLOT_BITMAP WORD(LED_SLOT_BITMAP)

static void asset_path(char *out, unsigned slot, unsigned bank, const char *extension)
{
    out[0] = '/';
    out[1] = 'E';
    out[2] = 'C';
    out[3] = '0' + slot / 10;
    out[4] = '0' + slot % 10;
    out[5] = 'A' + bank;
    out[6] = '.';
    out[7] = extension[0];
    out[8] = extension[1];
    out[9] = extension[2];
    out[10] = 0;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, unsigned n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return crc;
}

static int read_exact(const char *path, uint8_t *data, unsigned size)
{
    File f;
    int ok = open_file(&f, path, 1);
    if (ok)
        ok = file_size(&f) == size && read_file(&f, data, size) == size;
    close_file(&f);
    return ok;
}

static int save_exact(const char *path, const uint8_t *data, unsigned size)
{
    // Recreating an existing file reallocates its FAT chain and makes every selection slow.
    File f;
    int ok = open_file(&f, path, 3);
    if (!ok || file_size(&f) != size) {
        close_file(&f);
        ok = open_file(&f, path, 0x0b);
    }
    if (ok)
        ok = write_file(&f, data, size) == size;
    close_file(&f);
    uint8_t verify[32];
    if (!ok || size > sizeof(verify) || !read_exact(path, verify, size))
        return 0;
    for (unsigned i = 0; i < size; i++)
        if (verify[i] != data[i])
            return 0;
    return 1;
}

static int bank_record(unsigned slot, unsigned bank, uint8_t *record, const char *kind)
{
    char path[11];
    asset_path(path, slot, bank, kind);
    if (!read_exact(path, record, 16) || crc32(record, 12) != u32(record + 12) || !u32(record) ||
        !u32(record + 4) || u32(record + 4) > MAX_FILE)
        return 0;
    return 1;
}

static int newest_bank(unsigned slot, uint8_t *record, const char *kind)
{
    uint8_t other[16];
    int a = bank_record(slot, 0, record, kind), b = bank_record(slot, 1, other, kind);
    if (!a && !b)
        return -1;
    if (b && (!a || (int32_t)(u32(other) - u32(record)) > 0)) {
        for (unsigned i = 0; i < 16; i++)
            record[i] = other[i];
        return 1;
    }
    return 0;
}

static int selection_record(unsigned bank, uint8_t *r)
{
    char path[] = "/ECPAT0.BIN";
    path[6] = '0' + bank;
    if (!read_exact(path, r, 16) || !u32(r) || r[6] != 0x73 || r[7] != 1 ||
        crc32(r, 12) != u32(r + 12))
        return 0;
    return (r[4] == 1 && r[5] < 32) || (r[4] == 2 && r[5] <= 69) || (r[4] == 5 && r[5] == 0);
}

static int latest_qspi_selection(uint8_t *r)
{
    uint8_t other[16];
    int a = selection_record(0, r), b = selection_record(1, other);
    if (!a && !b)
        return -1;
    if (b && (!a || (int32_t)(u32(other) - u32(r)) > 0)) {
        for (unsigned i = 0; i < 16; i++)
            r[i] = other[i];
        return 1;
    }
    return 0;
}
#include "selection_journal.c"

// The decoder uses consecutive indexes. Saved selections use slot numbers so removing a GIF cannot shift them.
static int slot_from_index(unsigned index)
{
    for (unsigned slot = 0; slot < 32; slot++)
        if ((GIF_SLOT_BITMAP & (1u << slot)) && index-- == 0)
            return (int)slot;
    return -1;
}

int persist_renderer(unsigned state)
{
    if (state != 1 && state != 2 && state != 5)
        return -1;
    unsigned value = state == 2 ? WORD(LED_ACTIVE_COLOR) : 0;
    if (state == 1) {
        int slot = slot_from_index(WORD(LED_GIF_INDEX));
        if (BYTE(LED_RENDER_FROM_SD) || slot < 0)
            return -1;
        value = (unsigned)slot;
    }
    if (!BYTE(LED_FILESYSTEM_MOUNTED) || value > 69)
        return -1;
    uint8_t old[16] = {0}, next[16] = {0};
    int bank = latest_selection(old);
    if (bank >= 0 && old[4] == state && old[5] == value) {
        ANIMATION_STATE = state;
        return 0;
    }
    unsigned generation = bank < 0 ? 1 : u32(old) + 1;
    if (!generation)
        generation = 1;
    put32(next, generation);
    next[4] = state;
    next[5] = value;
    next[6] = 0x73;
    next[7] = 1;
    put32(next + 12, crc32(next, 12));
    if (!save_selection(next))
        return -1;
    ANIMATION_STATE = state;
    return 0;
}

unsigned journal_color(void)
{
    uint8_t r[16];
    if (latest_selection(r) >= 0 && r[4] == 2)
        return r[5];
    unsigned color = ORIGINAL(LED_GET_SAVED_COLOR, unsigned (*)(void))();
    return color <= 69 ? color : 69;
}

unsigned boot_renderer(void)
{
    uint8_t r[16];
    if (latest_selection(r) >= 0) {
        if (r[4] == 1) {
            if (!(GIF_SLOT_BITMAP & (1u << r[5])))
                return 2;
            unsigned index = 0;
            for (unsigned s = 0; s < r[5]; s++)
                if (GIF_SLOT_BITMAP & (1u << s))
                    index++;
            WORD(LED_GIF_INDEX) = index;
            BYTE(LED_RENDER_FROM_SD) = 0;
        }
        return r[4];
    }
    unsigned old = ORIGINAL(LED_GET_SAVED_RENDERER, unsigned (*)(void))();
    return old == 1 || old == 2 || old == 5 ? old : 2;
}

static int asset_crc(const char *path, unsigned length, uint32_t expected)
{
    File f;
    uint8_t block[128];
    if (!open_file(&f, path, 1)) {
        close_file(&f);
        return 0;
    }
    int ok = file_size(&f) == length;
    uint32_t crc = 0xffffffff;
    while (ok && length) {
        unsigned n = length > sizeof(block) ? sizeof(block) : length;
        if (read_file(&f, block, n) != n) {
            ok = 0;
            break;
        }
        crc = crc_update(crc, block, n);
        length -= n;
    }
    close_file(&f);
    return ok && ~crc == expected;
}
typedef struct {
    File file;
    uint8_t buf[128];
    unsigned at, used, left;
    int failed;
} GifReader;
static int gif_byte(GifReader *g)
{
    if (!g->left) {
        g->failed = 1;
        return -1;
    }
    if (g->at == g->used) {
        g->used = read_file(&g->file, g->buf, g->left > 128 ? 128 : g->left);
        g->at = 0;
        if (!g->used) {
            g->failed = 1;
            return -1;
        }
    }
    --g->left;
    return g->buf[g->at++];
}

static unsigned gif_word(GifReader *g)
{
    unsigned a = gif_byte(g), b = gif_byte(g);
    return a | (b << 8);
}

static int gif_skip(GifReader *g, unsigned n)
{
    while (n-- && !g->failed)
        gif_byte(g);
    return !g->failed;
}

static int gif_blocks(GifReader *g)
{
    int n;
    while ((n = gif_byte(g)) > 0)
        if (!gif_skip(g, n))
            return 0;
    return n == 0 && !g->failed;
}

static int gif_lzw(GifReader *g, unsigned minimum, unsigned pixels, unsigned palette)
{
    // A 42 by 13 frame needs at most 546 new entries plus 256 colors and two control codes.
    uint16_t lengths[804];
    unsigned clear = 1u << minimum, end = clear + 1, next = end + 1, width = minimum + 1;
    unsigned bitcount = 0, bits = 0, remaining = 0, decoded = 0, previous = 0;
    int first = 1;
    for (unsigned i = 0; i < clear; i++)
        lengths[i] = 1;
    for (;;) {
        while (bitcount < width) {
            if (!remaining) {
                int n = gif_byte(g);
                if (n <= 0)
                    return 0;
                remaining = n;
            }
            int byte = gif_byte(g);
            if (byte < 0)
                return 0;
            --remaining;
            bits |= (unsigned)byte << bitcount;
            bitcount += 8;
        }
        unsigned code = bits & ((1u << width) - 1);
        bits >>= width;
        bitcount -= width;
        if (first && code != clear)
            return 0;
        first = 0;
        if (code == clear) {
            next = end + 1;
            width = minimum + 1;
            previous = 0;
            continue;
        }
        if (code == end) {
            if (decoded != pixels || !gif_skip(g, remaining) || !gif_blocks(g))
                return 0;
            return 1;
        }
        unsigned length;
        if (code < clear) {
            if (code >= palette)
                return 0;
            length = 1;
        } else if (code < next) {
            if (code >= 804)
                return 0;
            length = lengths[code];
        } else if (code == next && previous)
            length = previous + 1;
        else
            return 0;
        if (!previous && code >= clear)
            return 0;
        if (decoded + length > pixels)
            return 0;
        decoded += length;
        if (previous) {
            if (next >= 804)
                return 0;
            lengths[next++] = previous + 1;
            if (next == (1u << width) && width < 12)
                ++width;
        }
        previous = length;
    }
}

static int valid_gif(const char *path, unsigned size)
{
    GifReader g;
    g.at = g.used = g.failed = 0;
    g.left = size;
    if (!open_file(&g.file, path, 1)) {
        close_file(&g.file);
        return 0;
    }
    int ok = 0;
    unsigned frames = 0;
    const uint8_t prefix[] = {'G', 'I', 'F', '8'};
    for (unsigned i = 0; i < 4; i++)
        if (gif_byte(&g) != prefix[i])
            goto done;
    int v = gif_byte(&g);
    if ((v != '7' && v != '9') || gif_byte(&g) != 'a')
        goto done;
    if (gif_word(&g) != 42 || gif_word(&g) != 13)
        goto done;
    unsigned flags = gif_byte(&g), palette = flags & 128 ? 1u << ((flags & 7) + 1) : 0;
    gif_skip(&g, 2);
    if (palette)
        gif_skip(&g, 3 * palette);
    while (!g.failed && g.left) {
        int type = gif_byte(&g);
        if (type == 0x3b) {
            ok = frames && !g.left;
            break;
        }
        if (type == 0x21) {
            gif_byte(&g);
            if (!gif_blocks(&g))
                break;
        } else if (type == 0x2c) {
            unsigned x = gif_word(&g), y = gif_word(&g), w = gif_word(&g), h = gif_word(&g);
            flags = gif_byte(&g);
            if (!w || !h || x + w > 42 || y + h > 13)
                break;
            unsigned local = flags & 128 ? 1u << ((flags & 7) + 1) : palette;
            if (flags & 128)
                gif_skip(&g, 3 * local);
            int bits = gif_byte(&g);
            if (!local || bits < 2 || bits > 8 || !gif_lzw(&g, bits, w * h, local))
                break;
            ++frames;
        } else
            break;
    }
done:
    close_file(&g.file);
    return ok && !g.failed;
}

int custom_gif_count(unsigned storage);
static int publish_gif(unsigned slot, unsigned length, uint32_t crc)
{
    GIF_ENUM_READY = 0;
    for (unsigned i = 0; i < 3; i++)
        GIF_CHECKED[slot * 3 + i] = 0;
    char source[] = "/BF00.TMP", dest[11], marker[11];
    uint8_t record[16] = {0}, block[512];
    source[3] = '0' + slot / 10;
    source[4] = '0' + slot % 10;
    if (!length || !asset_crc(source, length, crc) || !valid_gif(source, length))
        return VERIFY_FAILED;
    // Write the inactive bank and verify it before committing its marker. Keep the previous bank as a fallback.
    int old = newest_bank(slot, record, "IDX"), bank = old == 0 ? 1 : 0;
    uint32_t generation = old < 0 ? 1 : u32(record) + 1;
    if (!generation)
        generation = 1;
    asset_path(dest, slot, bank, "BIN");
    asset_path(marker, slot, bank, "IDX");
    File in, out;
    int a = open_file(&in, source, 1), b = open_file(&out, dest, 0x0b), ok = a && b;
    unsigned left = length;
    while (ok && left) {
        unsigned n = left > sizeof(block) ? sizeof(block) : left;
        ok = read_file(&in, block, n) == n && write_file(&out, block, n) == n;
        left -= n;
    }
    close_file(&out);
    close_file(&in);
    if (!ok || !asset_crc(dest, length, crc))
        return IO_FAILED;
    put32(record, generation);
    put32(record + 4, length);
    put32(record + 8, crc);
    put32(record + 12, crc32(record, 12));
    if (!save_exact(marker, record, 16))
        return IO_FAILED;
    custom_gif_count(0);
    return OK;
}

int custom_gif_count(unsigned storage)
{
    if (storage || !BYTE(LED_FILESYSTEM_MOUNTED)) {
        GIF_ENUM_READY = 0;
        GIF_SLOT_BITMAP = 0;
        return -1;
    }
    if (GIF_ENUM_READY) {
        unsigned count = 0;
        for (unsigned i = 0; i < 32; i++)
            if (GIF_SLOT_BITMAP & (1u << i))
                count++;
        WORD(LED_GIF_COUNT) = count;
        WORD(LED_GIF_ENUM_COUNT) = count;
        return count ? (int)count : -1;
    }
    unsigned count = 0;
    uint32_t bitmap = 0;
    uint8_t record[16];
    for (unsigned i = 0; i < 32; i++)
        if (newest_bank(i, record, "IDX") >= 0) {
            count++;
            bitmap |= 1u << i;
        }
    GIF_SLOT_BITMAP = bitmap;
    GIF_ENUM_READY = 1;
    WORD(LED_GIF_ENUM_COUNT) = count;
    WORD(LED_GIF_COUNT) = count;
    if (!count)
        return -1;
    WORD(LED_GIF_COUNT) = count;
    return count;
}

int custom_gif_name(const char *root, int index, char *out)
{
    (void)root;
    if (BYTE(LED_RENDER_FROM_SD) || index < 0)
        return 0;
    if (!GIF_ENUM_READY && custom_gif_count(0) < 1)
        return 0;
    int slot = slot_from_index(index);
    if (slot < 0)
        return 0;
    uint8_t record[16];
    int bank = newest_bank(slot, record, "IDX");
    if (bank < 0)
        return 0;
    asset_path(out, slot, bank, "BIN");
    return 1;
}

int active_gif_slot(void)
{
    if (ANIMATION_STATE != 1 || BYTE(LED_RENDER_FROM_SD) != 0)
        return -1;
    unsigned index = WORD(LED_GIF_INDEX);
    uint32_t bitmap = GIF_SLOT_BITMAP;
    for (unsigned slot = 0; slot < 32; slot++)
        if ((bitmap & (1u << slot)) && index-- == 0)
            return (int)slot;
    return -1;
}

int play_gif(unsigned slot)
{
    uint8_t rec[16];
    int bank = newest_bank(slot, rec, "IDX");
    if (bank < 0)
        return OPEN_FAILED;
    char path[11];
    asset_path(path, slot, bank, "BIN");

    // Publication already verified the bytes. Repeating the full CRC here made the first Play slow.
    uint32_t *checked = GIF_CHECKED + slot * 3;
    int unchanged =
        checked[0] == u32(rec) && checked[1] == u32(rec + 4) && checked[2] == u32(rec + 8);
    if (unchanged && active_gif_slot() == (int)slot)
        return OK;
    File committed;
    int exists = open_file(&committed, path, 1);
    int intact = exists && file_size(&committed) == u32(rec + 4);
    close_file(&committed);
    if (!intact)
        return VERIFY_FAILED;
    for (unsigned i = 0; i < 3; i++)
        checked[i] = u32(rec + i * 4);
    unsigned was_playing = ANIMATION_STATE == 1;
    extern unsigned asset_console(const uint8_t *, unsigned);
    const uint8_t cmd[] = "set render_from_sd 0";
    if (asset_console(cmd, sizeof(cmd) - 1))
        return IO_FAILED;
    // The stock setter updates flash but leaves the active renderer source unchanged.
    BYTE(LED_RENDER_FROM_SD) = 0;
    int count = custom_gif_count(0);
    if (count < 1)
        return VERIFY_FAILED;
    WORD(LED_GIF_ENUM_COUNT) = count;
    unsigned index = 0;
    for (unsigned i = 0; i < slot; i++)
        if (GIF_SLOT_BITMAP & (1u << i))
            index++;
    BYTE(LED_GIF_ABORT) = 1;
    ORIGINAL(LED_SELECT_GIF_HOOK, void (*)(unsigned))(index);
    ORIGINAL(LED_SET_RENDERER, int (*)(unsigned))(1);
    WORD(LED_GIF_INDEX) = index;
    extern int persist_renderer(unsigned);
    if (persist_renderer(1))
        return VERIFY_FAILED;
    // A leftover abort flag cancels the first Play. Set it only when a decoder is already running.
    if (!was_playing)
        BYTE(LED_GIF_ABORT) = 0;
    return ANIMATION_STATE == 1 ? OK : VERIFY_FAILED;
}

static int effect_data(unsigned slot, uint8_t *effect)
{
    uint8_t rec[16];
    int bank = newest_bank(slot, rec, "FXI");
    if (bank < 0)
        return 0;
    char path[11];
    asset_path(path, slot, bank, "FXB");
    return u32(rec + 4) == 8 && read_exact(path, effect, 8) && crc32(effect, 8) == u32(rec + 8) &&
           effect[1] >= 1 && effect[1] <= 12;
}

static int save_effect(unsigned slot, const uint8_t *effect)
{
    if (effect[1] < 1 || effect[1] > 12)
        return BAD_REQUEST;
    uint8_t rec[16] = {0};
    int old = newest_bank(slot, rec, "FXI"), bank = old == 0 ? 1 : 0;
    uint32_t generation = old < 0 ? 1 : u32(rec) + 1;
    if (!generation)
        generation = 1;
    char data[11], marker[11];
    asset_path(data, slot, bank, "FXB");
    asset_path(marker, slot, bank, "FXI");
    if (!save_exact(data, effect, 8))
        return IO_FAILED;
    put32(rec, generation);
    put32(rec + 4, 8);
    put32(rec + 8, crc32(effect, 8));
    put32(rec + 12, crc32(rec, 12));
    return save_exact(marker, rec, 16) ? OK : IO_FAILED;
}

static int selected_effect(uint8_t *record)
{
    uint8_t other[12];
    int a = read_exact("/ECSEL0.BIN", record, 12) && u32(record) && u32(record + 4) < 32 &&
            crc32(record, 8) == u32(record + 8);
    int b = read_exact("/ECSEL1.BIN", other, 12) && u32(other) && u32(other + 4) < 32 &&
            crc32(other, 8) == u32(other + 8);
    if (!a && !b)
        return -1;
    if (b && (!a || (int32_t)(u32(other) - u32(record)) > 0)) {
        for (unsigned i = 0; i < 12; i++)
            record[i] = other[i];
        return 1;
    }
    return 0;
}

void restore_custom_effect(void)
{
    uint8_t record[12], data[8];
    unsigned slot;
    if (!BYTE(LED_FILESYSTEM_MOUNTED))
        return;
    if (selected_effect(record) >= 0)
        slot = u32(record + 4);
    else {

        if (!read_exact("/ECSEL.BIN", record, 8) || u32(record) > 31 ||
            crc32(record, 4) != u32(record + 4))
            return;
        slot = u32(record);
    }
    if (!effect_data(slot, data))
        return;
    BYTE(LED_COLOR_TABLE + 0x43) = data[0];
    BYTE(LED_COLOR_TABLE + 0x4a) = data[1];
    for (unsigned i = 0; i < 6; i++)
        BYTE(LED_COLOR_TABLE + 0x44 + i) = data[2 + i];
}

int load_effect(unsigned slot)
{
    uint8_t data[8], record[12] = {0};
    if (!effect_data(slot, data))
        return OPEN_FAILED;
    int old = selected_effect(record);
    uint32_t generation = old < 0 ? 1 : u32(record) + 1;
    if (!generation)
        generation = 1;
    if (old < 0 || u32(record + 4) != slot) {
        put32(record, generation);
        put32(record + 4, slot);
        put32(record + 8, crc32(record, 8));
        if (!save_exact(old == 0 ? "/ECSEL1.BIN" : "/ECSEL0.BIN", record, 12))
            return IO_FAILED;
    }
    ORIGINAL(LED_SET_RENDERER, int (*)(unsigned))(2);
    restore_custom_effect();
    if (ORIGINAL(LED_SELECT_COLOR_HOOK, int (*)(unsigned))(0))
        return IO_FAILED;
    if (ORIGINAL(LED_GET_SAVED_COLOR, unsigned (*)(void))() != 0)
        ORIGINAL(LED_SAVE_COLOR, void (*)(unsigned))(0);
    return BYTE(LED_ACTIVE_COLOR) == 0 ? OK : VERIFY_FAILED;
}

uint32_t uploaded_gifs(void)
{
    if (!GIF_ENUM_READY)
        custom_gif_count(0);
    return GIF_SLOT_BITMAP;
}

int remove_gif(unsigned slot)
{
    if (slot > 31)
        return BAD_REQUEST;
    if (!BYTE(LED_FILESYSTEM_MOUNTED))
        return UNAVAILABLE;
    if (ANIMATION_STATE == 1)
        return BUSY;
    uint8_t record[16], empty[16] = {0};
    int status = OK;
    for (unsigned bank = 0; bank < 2; bank++) {
        if (!bank_record(slot, bank, record, "IDX"))
            continue;
        char path[11];
        asset_path(path, slot, bank, "IDX");
        if (!save_exact(path, empty, 16)) {
            status = IO_FAILED;
            break;
        }
    }
    for (unsigned i = 0; i < 3; i++)
        GIF_CHECKED[slot * 3 + i] = 0;
    GIF_ENUM_READY = 0;
    custom_gif_count(0);
    if (status == OK && (GIF_SLOT_BITMAP & (1u << slot)))
        return VERIFY_FAILED;
    return status;
}

int next_uploaded_gif(unsigned start)
{
    uint32_t bitmap = uploaded_gifs();
    for (unsigned slot = start; slot < 32; slot++)
        if (bitmap & (1u << slot))
            return (int)slot;
    return -1;
}
