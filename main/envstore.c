#include "envstore.h"

#include <string.h>

#define MAGIC 0x31564E45u        /* "ENV1" little-endian */

/* Serialised by hand rather than by memcpy of a struct: a struct's padding
   and byte order are the compiler's business, and this data has to be read
   back by a different build months later. */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void encode(uint8_t *p, const env_sample_t *s)
{
    put_u32(p, s->minute);
    put_u16(p + 4, (uint16_t)s->temp_c100);
    put_u16(p + 6, s->rh_c100);
    put_u16(p + 8, s->hpa_x10);
    put_u16(p + 10, 0);                  /* reserved; kept zero so it can be used later */
}

static void decode(const uint8_t *p, env_sample_t *s)
{
    s->minute = get_u32(p);
    s->temp_c100 = (int16_t)get_u16(p + 4);
    s->rh_c100 = get_u16(p + 6);
    s->hpa_x10 = get_u16(p + 8);
}

static size_t sector_off(const envstore_t *s, int i)
{
    return (size_t)i * s->f->sector_size;
}

static bool read_header(const envstore_t *s, int i, uint32_t *seq)
{
    uint8_t hdr[ENVSTORE_HDR];
    if (!s->f->read(s->f->ctx, sector_off(s, i), hdr, sizeof hdr)) return false;
    if (get_u32(hdr) != MAGIC) return false;
    *seq = get_u32(hdr + 4);
    return true;
}

static bool write_header(const envstore_t *s, int i, uint32_t seq)
{
    uint8_t hdr[ENVSTORE_HDR];
    put_u32(hdr, MAGIC);
    put_u32(hdr + 4, seq);
    return s->f->write(s->f->ctx, sector_off(s, i), hdr, sizeof hdr);
}

/* The first unwritten slot in a sector. A record whose timestamp is still in
   the erased state was never written -- which is also how a power cut in the
   middle of a write comes out, so recovery is the same code as start-up. */
static int first_free_slot(const envstore_t *s, int i)
{
    for (int k = 0; k < s->per_sector; k++) {
        uint8_t rec[ENVSTORE_RECORD];
        size_t off = sector_off(s, i) + ENVSTORE_HDR + (size_t)k * ENVSTORE_RECORD;
        if (!s->f->read(s->f->ctx, off, rec, sizeof rec)) return k;
        if (get_u32(rec) == ENVSTORE_NO_MINUTE) return k;
    }
    return s->per_sector;
}

bool envstore_format(envstore_t *s)
{
    if (!s->f->erase(s->f->ctx, 0, s->f->size)) return false;
    if (!write_header(s, 0, 1)) return false;
    s->cur = 0;
    s->seq = 1;
    s->slot = 0;
    s->ready = true;
    return true;
}

bool envstore_open(envstore_t *s, const envflash_t *f)
{
    memset(s, 0, sizeof *s);
    s->f = f;
    if (f->sector_size <= ENVSTORE_HDR + ENVSTORE_RECORD) return false;
    s->sectors = (int)(f->size / f->sector_size);
    if (s->sectors < 2) return false;
    s->per_sector = (int)((f->sector_size - ENVSTORE_HDR) / ENVSTORE_RECORD);

    /* The newest sector is the one with the highest sequence number. */
    int best = -1;
    uint32_t best_seq = 0;
    for (int i = 0; i < s->sectors; i++) {
        uint32_t seq;
        if (!read_header(s, i, &seq)) continue;
        if (best < 0 || seq > best_seq) { best = i; best_seq = seq; }
    }
    if (best < 0) return envstore_format(s);

    s->cur = best;
    s->seq = best_seq;
    s->slot = first_free_slot(s, best);
    s->ready = true;
    return true;
}

/* Moves to the next sector in the ring, erasing it. This is where the oldest
   thirty-day-old readings are thrown away, one sector at a time. */
static bool advance(envstore_t *s)
{
    int next = (s->cur + 1) % s->sectors;
    if (!s->f->erase(s->f->ctx, sector_off(s, next), s->f->sector_size)) return false;
    if (!write_header(s, next, s->seq + 1)) return false;
    s->cur = next;
    s->seq += 1;
    s->slot = 0;
    return true;
}

bool envstore_add(envstore_t *s, const env_sample_t *sample)
{
    if (!s->ready) return false;
    /* A record with the erased timestamp would be indistinguishable from an
       empty slot and would truncate the log on the next open. */
    if (sample->minute == ENVSTORE_NO_MINUTE) return false;
    if (s->slot >= s->per_sector && !advance(s)) return false;

    uint8_t rec[ENVSTORE_RECORD];
    encode(rec, sample);
    size_t off = sector_off(s, s->cur) + ENVSTORE_HDR
               + (size_t)s->slot * ENVSTORE_RECORD;
    if (!s->f->write(s->f->ctx, off, rec, sizeof rec)) return false;
    s->slot++;
    return true;
}

int envstore_capacity(const envstore_t *s)
{
    return s->ready ? s->sectors * s->per_sector : 0;
}

/* Sectors oldest first. Sequence numbers only ever increase, so the oldest
   live sector is the one with the lowest, and the ring's order follows. */
static int oldest_sector(const envstore_t *s)
{
    int best = s->cur;
    uint32_t best_seq = s->seq;
    for (int i = 0; i < s->sectors; i++) {
        uint32_t seq;
        if (!read_header(s, i, &seq)) continue;
        if (seq < best_seq) { best = i; best_seq = seq; }
    }
    return best;
}

int envstore_count(const envstore_t *s)
{
    if (!s->ready) return 0;
    int n = 0;
    for (int i = 0; i < s->sectors; i++) {
        uint32_t seq;
        if (!read_header(s, i, &seq)) continue;
        n += (i == s->cur) ? s->slot : s->per_sector;
    }
    return n;
}

void envstore_walk(const envstore_t *s, void *buf,
                   bool (*fn)(const env_sample_t *, void *), void *ctx)
{
    if (!s->ready || buf == NULL || fn == NULL) return;

    int start = oldest_sector(s);
    for (int n = 0; n < s->sectors; n++) {
        int i = (start + n) % s->sectors;
        uint32_t seq;
        if (!read_header(s, i, &seq)) continue;
        if (!s->f->read(s->f->ctx, sector_off(s, i), buf, s->f->sector_size)) continue;

        const uint8_t *p = (const uint8_t *)buf + ENVSTORE_HDR;
        int limit = (i == s->cur) ? s->slot : s->per_sector;
        for (int k = 0; k < limit; k++) {
            env_sample_t sample;
            decode(p + (size_t)k * ENVSTORE_RECORD, &sample);
            if (sample.minute == ENVSTORE_NO_MINUTE) break;
            if (!fn(&sample, ctx)) return;
        }
        if (i == s->cur) return;        /* the newest sector is the last one */
    }
}

bool envstore_latest(const envstore_t *s, env_sample_t *out)
{
    if (!s->ready) return false;
    int i = s->cur, slot = s->slot;
    if (slot == 0) {
        /* Nothing in this sector yet; the newest reading is the last one of
           the sector before it, if there is one. */
        int prev = (i - 1 + s->sectors) % s->sectors;
        uint32_t seq;
        if (prev == i || !read_header(s, prev, &seq)) return false;
        i = prev;
        slot = s->per_sector;
    }
    uint8_t rec[ENVSTORE_RECORD];
    size_t off = sector_off(s, i) + ENVSTORE_HDR + (size_t)(slot - 1) * ENVSTORE_RECORD;
    if (!s->f->read(s->f->ctx, off, rec, sizeof rec)) return false;
    if (get_u32(rec) == ENVSTORE_NO_MINUTE) return false;
    decode(rec, out);
    return true;
}
