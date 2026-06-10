//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Minimal PNG decoder for bare-metal / embedded.
 * Only supports 8-bit indexed color (color type 3, PLTE chunk).
 *
 * Usage:
 *   #include <string.h>  // memcpy, memset, memcmp
 *   #include "png.h"
 *   int rc = png_decode(data, len, palette, pixels, &w, &h);
 */

#ifndef PNG_H
#define PNG_H

#include <stdint.h>
#include <string.h>

/* Error codes */
#define PNG_ERR_SIGNATURE   (-1)
#define PNG_ERR_IHDR        (-2)
#define PNG_ERR_COLORTYPE   (-3)
#define PNG_ERR_BITDEPTH    (-4)
#define PNG_ERR_NO_PLTE     (-5)
#define PNG_ERR_INFLATE     (-6)
#define PNG_ERR_FILTER      (-7)
#define PNG_ERR_TRUNC       (-8)

/* --- Byte reading --- */

static inline uint32_t png__be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* --- DEFLATE bit reader --- */

typedef struct {
    const uint8_t *src;
    uint32_t src_len, pos;
    uint32_t bitbuf;
    int      bitcnt;
} png__bits_t;

static inline void png__bits_init(png__bits_t *b, const uint8_t *src, uint32_t len) {
    b->src = src; b->src_len = len; b->pos = 0;
    b->bitbuf = 0; b->bitcnt = 0;
}

static inline int png__bits_ensure(png__bits_t *b, int need) {
    while (b->bitcnt < need) {
        if (b->pos >= b->src_len) return -1;
        b->bitbuf |= (uint32_t)b->src[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    return 0;
}

static inline uint32_t png__bits_peek(png__bits_t *b, int n) {
    return b->bitbuf & ((1u << n) - 1);
}

static inline void png__bits_drop(png__bits_t *b, int n) {
    b->bitbuf >>= n; b->bitcnt -= n;
}

static inline int png__bits_read(png__bits_t *b, int n, uint32_t *out) {
    if (png__bits_ensure(b, n) < 0) return -1;
    *out = png__bits_peek(b, n);
    png__bits_drop(b, n);
    return 0;
}

static inline void png__bits_align(png__bits_t *b) {
    int drop = b->bitcnt & 7;
    b->bitbuf >>= drop; b->bitcnt -= drop;
}

/* --- Huffman tables (max 15-bit codes per DEFLATE spec) --- */

#define PNG__MAXBITS 15

typedef struct {
    uint16_t counts[PNG__MAXBITS + 1];
    uint16_t symbols[288 + 32];
} png__huff_t;

static int png__huff_build(png__huff_t *h, const uint8_t *lengths, int n) {
    uint16_t offsets[PNG__MAXBITS + 1];

    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++)
        h->counts[lengths[i]]++;

    int left = 1;
    for (int i = 1; i <= PNG__MAXBITS; i++) {
        left <<= 1;
        left -= h->counts[i];
        if (left < 0) return -1;
    }

    offsets[0] = offsets[1] = 0;
    for (int i = 1; i < PNG__MAXBITS; i++)
        offsets[i + 1] = offsets[i] + h->counts[i];

    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;

    return 0;
}

static int png__huff_decode(png__bits_t *b, const png__huff_t *h, uint16_t *sym) {
    int code = 0, first = 0, idx = 0;
    for (int len = 1; len <= PNG__MAXBITS; len++) {
        if (png__bits_ensure(b, 1) < 0) return -1;
        code |= (int)png__bits_peek(b, 1);
        png__bits_drop(b, 1);
        int count = h->counts[len];
        if (code < first + count) {
            *sym = h->symbols[idx + (code - first)];
            return 0;
        }
        idx  += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* --- DEFLATE tables --- */

static const uint16_t png__len_base[29] = {
      3,   4,   5,   6,   7,   8,   9,  10,  11,  13,
     15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
     67,  83,  99, 115, 131, 163, 195, 227, 258
};
static const uint8_t png__len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t png__dist_base[30] = {
        1,    2,    3,    4,    5,    7,    9,   13,   17,   25,
       33,   49,   65,   97,  129,  193,  257,  385,  513,  769,
     1025, 1537, 2049, 3073, 4097, 6145, 8193,12289,16385,24577
};
static const uint8_t png__dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* --- Inflate blocks --- */

static int png__inflate_codes(png__bits_t *b, const png__huff_t *lit,
                              const png__huff_t *dist,
                              uint8_t *out, uint32_t cap, uint32_t *pos) {
    uint16_t sym;
    for (;;) {
        if (png__huff_decode(b, lit, &sym) < 0) return -1;
        if (sym < 256) {
            if (*pos >= cap) return -1;
            out[(*pos)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int li = sym - 257;
            if (li >= 29) return -1;
            uint32_t len = png__len_base[li], extra;
            if (png__len_extra[li]) {
                if (png__bits_read(b, png__len_extra[li], &extra) < 0) return -1;
                len += extra;
            }
            if (png__huff_decode(b, dist, &sym) < 0) return -1;
            if (sym >= 30) return -1;
            uint32_t d = png__dist_base[sym];
            if (png__dist_extra[sym]) {
                if (png__bits_read(b, png__dist_extra[sym], &extra) < 0) return -1;
                d += extra;
            }
            if (d > *pos || *pos + len > cap) return -1;
            /* Byte-by-byte for overlapping back-references */
            uint32_t src = *pos - d;
            for (uint32_t j = 0; j < len; j++)
                out[(*pos)++] = out[src + j];
        }
    }
}

static void png__build_fixed(png__huff_t *lit, png__huff_t *dist) {
    uint8_t lengths[288 + 32];
    for (int i =   0; i <= 143; i++) lengths[i] = 8;
    for (int i = 144; i <= 255; i++) lengths[i] = 9;
    for (int i = 256; i <= 279; i++) lengths[i] = 7;
    for (int i = 280; i <= 287; i++) lengths[i] = 8;
    png__huff_build(lit, lengths, 288);
    for (int i = 0; i < 32; i++) lengths[i] = 5;
    png__huff_build(dist, lengths, 32);
}

static const uint8_t png__clcl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int png__inflate_dynamic(png__bits_t *b, uint8_t *out,
                                uint32_t cap, uint32_t *pos) {
    uint32_t hlit, hdist, hclen;
    if (png__bits_read(b, 5, &hlit)  < 0) return -1;
    if (png__bits_read(b, 5, &hdist) < 0) return -1;
    if (png__bits_read(b, 4, &hclen) < 0) return -1;
    hlit += 257; hdist += 1; hclen += 4;

    uint8_t clcl[19] = {0};
    for (int i = 0; i < (int)hclen; i++) {
        uint32_t v;
        if (png__bits_read(b, 3, &v) < 0) return -1;
        clcl[png__clcl_order[i]] = (uint8_t)v;
    }

    png__huff_t cl, lit, dist;
    if (png__huff_build(&cl, clcl, 19) < 0) return -1;

    uint8_t code_lengths[288 + 32];
    int total = (int)(hlit + hdist), i = 0;
    while (i < total) {
        uint16_t sym;
        if (png__huff_decode(b, &cl, &sym) < 0) return -1;
        if (sym < 16) {
            code_lengths[i++] = (uint8_t)sym;
        } else {
            uint32_t rep;
            uint8_t val = 0;
            if (sym == 16) {
                if (i == 0) return -1;
                val = code_lengths[i - 1];
                if (png__bits_read(b, 2, &rep) < 0) return -1;
                rep += 3;
            } else if (sym == 17) {
                if (png__bits_read(b, 3, &rep) < 0) return -1;
                rep += 3;
            } else if (sym == 18) {
                if (png__bits_read(b, 7, &rep) < 0) return -1;
                rep += 11;
            } else {
                return -1;
            }
            while (rep-- && i < total)
                code_lengths[i++] = val;
        }
    }

    if (png__huff_build(&lit, code_lengths, (int)hlit) < 0) return -1;
    if (png__huff_build(&dist, code_lengths + hlit, (int)hdist) < 0) return -1;
    return png__inflate_codes(b, &lit, &dist, out, cap, pos);
}

/* --- Inflate (zlib-wrapped DEFLATE) --- */

static int png__inflate(const uint8_t *src, uint32_t src_len,
                        uint8_t *out, uint32_t cap, uint32_t *out_written) {
    if (src_len < 2) return -1;
    if ((src[0] & 0x0F) != 8) return -1;
    if (((uint16_t)src[0] * 256 + src[1]) % 31 != 0) return -1;

    png__bits_t b;
    png__bits_init(&b, src + 2, src_len - 2);
    uint32_t pos = 0;

    for (;;) {
        uint32_t bfinal, btype;
        if (png__bits_read(&b, 1, &bfinal) < 0) return -1;
        if (png__bits_read(&b, 2, &btype)  < 0) return -1;

        if (btype == 0) {
            png__bits_align(&b);
            if (b.pos + 4 > b.src_len) return -1;
            uint32_t len  = (uint32_t)b.src[b.pos] | ((uint32_t)b.src[b.pos+1] << 8);
            uint32_t nlen = (uint32_t)b.src[b.pos+2] | ((uint32_t)b.src[b.pos+3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return -1;
            if (b.pos + len > b.src_len || pos + len > cap) return -1;
            memcpy(out + pos, b.src + b.pos, len);
            pos += len; b.pos += len;
            b.bitbuf = 0; b.bitcnt = 0;
        } else if (btype == 1) {
            png__huff_t lit, dist;
            png__build_fixed(&lit, &dist);
            if (png__inflate_codes(&b, &lit, &dist, out, cap, &pos) < 0) return -1;
        } else if (btype == 2) {
            if (png__inflate_dynamic(&b, out, cap, &pos) < 0) return -1;
        } else {
            return -1;
        }
        if (bfinal) break;
    }
    *out_written = pos;
    return 0;
}

/* --- PNG filter reconstruction --- */

static inline uint8_t png__paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a; if (pa < 0) pa = -pa;
    int pb = p - (int)b; if (pb < 0) pb = -pb;
    int pc = p - (int)c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

static int png__unfilter(uint8_t *raw, int w, int h, uint8_t *pixels) {
    int raw_stride = 1 + w;
    for (int y = 0; y < h; y++) {
        uint8_t *row  = raw + y * raw_stride;
        uint8_t  filt = row[0];
        uint8_t *data = row + 1;
        uint8_t *cur  = pixels + y * w;
        uint8_t *prev = (y > 0) ? pixels + (y - 1) * w : NULL;

        switch (filt) {
        case 0: /* None */
            memcpy(cur, data, w);
            break;
        case 1: /* Sub */
            for (int x = 0; x < w; x++)
                cur[x] = data[x] + ((x >= 1) ? cur[x - 1] : 0);
            break;
        case 2: /* Up */
            for (int x = 0; x < w; x++)
                cur[x] = data[x] + (prev ? prev[x] : 0);
            break;
        case 3: /* Average */
            for (int x = 0; x < w; x++) {
                uint8_t a = (x >= 1) ? cur[x - 1] : 0;
                uint8_t b = prev ? prev[x] : 0;
                cur[x] = data[x] + (uint8_t)(((int)a + (int)b) >> 1);
            }
            break;
        case 4: /* Paeth */
            for (int x = 0; x < w; x++) {
                uint8_t a = (x >= 1) ? cur[x - 1] : 0;
                uint8_t b = prev ? prev[x] : 0;
                uint8_t c = (prev && x >= 1) ? prev[x - 1] : 0;
                cur[x] = data[x] + png__paeth(a, b, c);
            }
            break;
        default:
            return PNG_ERR_FILTER;
        }
    }
    return 0;
}

/* --- Main decode --- */

static int png_decode(const uint8_t *png, uint32_t len,
                      uint32_t *pal_out, uint8_t *pix_out,
                      int *w_out, int *h_out) {
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    if (len < 8 || memcmp(png, sig, 8) != 0) return PNG_ERR_SIGNATURE;

    uint32_t pos = 8;

    /* IHDR */
    if (pos + 12 > len) return PNG_ERR_TRUNC;
    uint32_t clen = png__be32(png + pos);
    if (png__be32(png + pos + 4) != 0x49484452u || clen < 13) return PNG_ERR_IHDR;
    if (pos + 12 + clen > len) return PNG_ERR_TRUNC;

    uint32_t width  = png__be32(png + pos + 8);
    uint32_t height = png__be32(png + pos + 12);
    if (png[pos + 17] != 3) return PNG_ERR_COLORTYPE;  /* indexed only */
    if (png[pos + 16] != 8) return PNG_ERR_BITDEPTH;   /* 8bpp only */
    *w_out = (int)width;
    *h_out = (int)height;
    pos += 12 + clen;

    /* Walk chunks: find PLTE and total IDAT size */
    const uint8_t *plte_data = NULL;
    uint32_t plte_len = 0, total_idat = 0;
    int have_plte = 0;

    for (uint32_t scan = pos; scan + 12 <= len; ) {
        uint32_t cl = png__be32(png + scan);
        uint32_t ct = png__be32(png + scan + 4);
        if (scan + 12 + cl > len) return PNG_ERR_TRUNC;

        if (ct == 0x504C5445u) {        /* PLTE */
            plte_data = png + scan + 8;
            plte_len = cl;
            have_plte = 1;
        } else if (ct == 0x49444154u) { /* IDAT */
            total_idat += cl;
        } else if (ct == 0x49454E44u) { /* IEND */
            break;
        }
        scan += 12 + cl;
    }
    if (!have_plte) return PNG_ERR_NO_PLTE;

    /* Extract palette */
    memset(pal_out, 0, 256 * sizeof(uint32_t));
    uint32_t n_pal = plte_len / 3;
    if (n_pal > 256) n_pal = 256;
    for (uint32_t j = 0; j < n_pal; j++)
        pal_out[j] = ((uint32_t)plte_data[j*3] << 16) |
                     ((uint32_t)plte_data[j*3+1] << 8) |
                      (uint32_t)plte_data[j*3+2];

    /* Concatenate IDAT payloads into scratch area past raw data region */
    uint32_t raw_size = height * (1 + width);
    uint8_t *compressed = pix_out + raw_size;
    uint32_t cpos = 0;

    for (uint32_t scan = pos; scan + 12 <= len; ) {
        uint32_t cl = png__be32(png + scan);
        uint32_t ct = png__be32(png + scan + 4);
        if (ct == 0x49444154u) {
            memcpy(compressed + cpos, png + scan + 8, cl);
            cpos += cl;
        } else if (ct == 0x49454E44u) {
            break;
        }
        scan += 12 + cl;
    }

    /* Inflate compressed data into raw (filter byte + pixel data per row) */
    uint32_t decompressed_len = 0;
    if (png__inflate(compressed, total_idat, pix_out, raw_size,
                     &decompressed_len) < 0 || decompressed_len != raw_size)
        return PNG_ERR_INFLATE;

    /* Unfilter into scratch area, then copy back */
    uint8_t *temp = compressed;
    if (png__unfilter(pix_out, (int)width, (int)height, temp) < 0)
        return PNG_ERR_FILTER;
    memcpy(pix_out, temp, width * height);

    return 0;
}

#endif /* PNG_H */
