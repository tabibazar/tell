#include "mp4mux.h"

#include <stdlib.h>
#include <string.h>

struct mp4mux {
    FILE    *f;
    uint16_t w, h;
    uint32_t fps;
    long     mdat_at;           /* where mdat's header starts */
    uint64_t mdat_bytes;        /* payload written so far */
    uint8_t  sps[128], pps[64];
    size_t   sps_len, pps_len;
    uint32_t *sizes;            /* per sample */
    uint8_t  *keys;
    size_t   n, cap;
    bool     bad;
};

/* ---- big-endian writers -------------------------------------------------- */

static void w8(FILE *f, uint8_t v) { fputc(v, f); }
static void w16(FILE *f, uint16_t v) { w8(f, (uint8_t)(v >> 8)); w8(f, (uint8_t)v); }
static void w24(FILE *f, uint32_t v) { w8(f, (uint8_t)(v >> 16)); w16(f, (uint16_t)v); }
static void w32(FILE *f, uint32_t v) { w16(f, (uint16_t)(v >> 16)); w16(f, (uint16_t)v); }
static void wtag(FILE *f, const char *t) { fwrite(t, 1, 4, f); }

/* A box: its size patched in when it closes. */
static long box_open(FILE *f, const char *type)
{
    long at = ftell(f);
    w32(f, 0);
    wtag(f, type);
    return at;
}

static void box_close(FILE *f, long at)
{
    long end = ftell(f);
    fseek(f, at, SEEK_SET);
    w32(f, (uint32_t)(end - at));
    fseek(f, end, SEEK_SET);
}

static void full_box_head(FILE *f, uint8_t version, uint32_t flags)
{
    w8(f, version);
    w24(f, flags);
}

/* ---- Annex-B ------------------------------------------------------------- */

/* The next NAL unit at or after *pos: its payload start and length, start
   code excluded. False at the end. */
static bool next_nal(const uint8_t *d, size_t len, size_t *pos, size_t *start, size_t *nal_len)
{
    size_t i = *pos;
    while (i + 3 <= len && !(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) i++;
    if (i + 3 > len) return false;
    size_t s = i + 3, j = s;
    while (j + 3 <= len && !(d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (d[j + 2] == 0 && j + 3 < len && d[j + 3] == 1)))) j++;
    size_t e = j + 3 <= len ? j : len;
    /* esp_h264 puts an extra zero between NALs ("68CE3C80 00 00 00 00 01"):
       trailing zeros are padding, not the NAL's, and left on a PPS they make
       it unparseable -- likely what esp_muxer tripped on. */
    while (e > s && d[e - 1] == 0) e--;
    *start = s;
    *nal_len = e - s;
    *pos = e;
    return true;
}

mp4mux_t *mp4mux_open(FILE *f, uint16_t width, uint16_t height, uint32_t fps)
{
    if (f == NULL || fps == 0) return NULL;
    mp4mux_t *m = calloc(1, sizeof *m);
    if (m == NULL) return NULL;
    m->f = f;
    m->w = width;
    m->h = height;
    m->fps = fps;
    long ftyp = box_open(f, "ftyp");
    wtag(f, "isom");
    w32(f, 0x200);
    wtag(f, "isom"); wtag(f, "iso2"); wtag(f, "avc1"); wtag(f, "mp41");
    box_close(f, ftyp);
    m->mdat_at = ftell(f);
    w32(f, 0);                   /* patched on close */
    wtag(f, "mdat");
    return m;
}

bool mp4mux_add(mp4mux_t *m, const uint8_t *d, size_t len, bool key)
{
    if (m == NULL || m->bad) return false;
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 256;
        uint32_t *s = realloc(m->sizes, cap * sizeof *s);
        uint8_t *k = realloc(m->keys, cap);
        if (!s || !k) { if (s) m->sizes = s; if (k) m->keys = k; m->bad = true; return false; }
        m->sizes = s;
        m->keys = k;
        m->cap = cap;
    }
    size_t pos = 0, st, nl;
    uint32_t sample = 0;
    bool picture = false;
    while (next_nal(d, len, &pos, &st, &nl)) {
        if (nl == 0) continue;
        uint8_t type = d[st] & 0x1F;
        if (type == 7) {                         /* SPS: into avcC, not mdat */
            if (nl <= sizeof m->sps) { memcpy(m->sps, d + st, nl); m->sps_len = nl; }
            continue;
        }
        if (type == 8) {                         /* PPS likewise */
            if (nl <= sizeof m->pps) { memcpy(m->pps, d + st, nl); m->pps_len = nl; }
            continue;
        }
        if (type == 9) continue;                 /* access unit delimiters are not stored */
        if (type == 5) key = true;
        if (type == 1 || type == 5) picture = true;
        w32(m->f, (uint32_t)nl);
        if (fwrite(d + st, 1, nl, m->f) != nl) { m->bad = true; return false; }
        sample += 4 + (uint32_t)nl;
    }
    if (!picture) {
        /* Nothing to show: undo anything written (an SEI alone, say). */
        if (sample) fseek(m->f, -(long)sample, SEEK_CUR);
        return false;
    }
    if (m->n == 0 && (!key || !m->sps_len || !m->pps_len)) { m->bad = true; return false; }
    m->sizes[m->n] = sample;
    m->keys[m->n] = key ? 1 : 0;
    m->n++;
    m->mdat_bytes += sample;
    return true;
}

static void write_moov(mp4mux_t *m)
{
    FILE *f = m->f;
    const uint32_t ts = m->fps * 1000, delta = 1000;   /* timescale, one frame */
    const uint32_t dur = (uint32_t)m->n * delta;
    const uint32_t chunk_offset = (uint32_t)(m->mdat_at + 8);
    static const uint32_t matrix[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };

    long moov = box_open(f, "moov");
    long mvhd = box_open(f, "mvhd");
    full_box_head(f, 0, 0);
    w32(f, 0); w32(f, 0);                 /* creation, modification */
    w32(f, ts); w32(f, dur);
    w32(f, 0x00010000); w16(f, 0x0100);   /* rate 1.0, volume 1.0 */
    w16(f, 0); w32(f, 0); w32(f, 0);
    for (int i = 0; i < 9; i++) w32(f, matrix[i]);
    for (int i = 0; i < 6; i++) w32(f, 0);
    w32(f, 2);                            /* next track id */
    box_close(f, mvhd);

    long trak = box_open(f, "trak");
    long tkhd = box_open(f, "tkhd");
    full_box_head(f, 0, 3);               /* enabled, in movie */
    w32(f, 0); w32(f, 0); w32(f, 1); w32(f, 0); w32(f, dur);
    w32(f, 0); w32(f, 0); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
    for (int i = 0; i < 9; i++) w32(f, matrix[i]);
    w32(f, (uint32_t)m->w << 16); w32(f, (uint32_t)m->h << 16);
    box_close(f, tkhd);

    long mdia = box_open(f, "mdia");
    long mdhd = box_open(f, "mdhd");
    full_box_head(f, 0, 0);
    w32(f, 0); w32(f, 0); w32(f, ts); w32(f, dur);
    w16(f, 0x55C4);                       /* language "und" */
    w16(f, 0);
    box_close(f, mdhd);
    long hdlr = box_open(f, "hdlr");
    full_box_head(f, 0, 0);
    w32(f, 0); wtag(f, "vide"); w32(f, 0); w32(f, 0); w32(f, 0);
    fwrite("VideoHandler", 1, 13, f);
    box_close(f, hdlr);

    long minf = box_open(f, "minf");
    long vmhd = box_open(f, "vmhd");
    full_box_head(f, 0, 1);
    w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0);
    box_close(f, vmhd);
    long dinf = box_open(f, "dinf");
    long dref = box_open(f, "dref");
    full_box_head(f, 0, 0);
    w32(f, 1);
    long url = box_open(f, "url ");
    full_box_head(f, 0, 1);               /* self-contained */
    box_close(f, url);
    box_close(f, dref);
    box_close(f, dinf);

    long stbl = box_open(f, "stbl");
    long stsd = box_open(f, "stsd");
    full_box_head(f, 0, 0);
    w32(f, 1);
    long avc1 = box_open(f, "avc1");
    w32(f, 0); w16(f, 0); w16(f, 1);      /* reserved, data reference index */
    w16(f, 0); w16(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
    w16(f, m->w); w16(f, m->h);
    w32(f, 0x00480000); w32(f, 0x00480000); /* 72 dpi */
    w32(f, 0); w16(f, 1);                 /* frame count */
    for (int i = 0; i < 32; i++) w8(f, 0);  /* compressor name */
    w16(f, 0x0018); w16(f, 0xFFFF);
    long avcc = box_open(f, "avcC");
    w8(f, 1);
    w8(f, m->sps[1]); w8(f, m->sps[2]); w8(f, m->sps[3]);   /* profile, compat, level */
    w8(f, 0xFF);                          /* 4-byte lengths */
    w8(f, 0xE1); w16(f, (uint16_t)m->sps_len); fwrite(m->sps, 1, m->sps_len, f);
    w8(f, 1); w16(f, (uint16_t)m->pps_len); fwrite(m->pps, 1, m->pps_len, f);
    box_close(f, avcc);
    box_close(f, avc1);
    box_close(f, stsd);

    long stts = box_open(f, "stts");
    full_box_head(f, 0, 0);
    w32(f, 1); w32(f, (uint32_t)m->n); w32(f, delta);
    box_close(f, stts);

    size_t nkeys = 0;
    for (size_t i = 0; i < m->n; i++) nkeys += m->keys[i];
    if (nkeys < m->n) {                   /* all key frames needs no stss */
        long stss = box_open(f, "stss");
        full_box_head(f, 0, 0);
        w32(f, (uint32_t)nkeys);
        for (size_t i = 0; i < m->n; i++) if (m->keys[i]) w32(f, (uint32_t)(i + 1));
        box_close(f, stss);
    }

    long stsc = box_open(f, "stsc");
    full_box_head(f, 0, 0);
    w32(f, 1); w32(f, 1); w32(f, (uint32_t)m->n); w32(f, 1);   /* one chunk of every sample */
    box_close(f, stsc);

    long stsz = box_open(f, "stsz");
    full_box_head(f, 0, 0);
    w32(f, 0); w32(f, (uint32_t)m->n);
    for (size_t i = 0; i < m->n; i++) w32(f, m->sizes[i]);
    box_close(f, stsz);

    long stco = box_open(f, "stco");
    full_box_head(f, 0, 0);
    w32(f, 1); w32(f, chunk_offset);
    box_close(f, stco);

    box_close(f, stbl);
    box_close(f, minf);
    box_close(f, mdia);
    box_close(f, trak);
    box_close(f, moov);
}

bool mp4mux_close(mp4mux_t *m)
{
    if (m == NULL) return false;
    bool ok = !m->bad && m->n > 0 && m->mdat_bytes + 8 <= 0xFFFFFFFFu;
    if (ok) {
        long end = ftell(m->f);
        fseek(m->f, m->mdat_at, SEEK_SET);
        w32(m->f, (uint32_t)(m->mdat_bytes + 8));
        fseek(m->f, end, SEEK_SET);
        write_moov(m);
        ok = !ferror(m->f) && fflush(m->f) == 0;
    }
    free(m->sizes);
    free(m->keys);
    free(m);
    return ok;
}
