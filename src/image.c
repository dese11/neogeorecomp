#include <neogeorecomp/neogeo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- reading indexed PNGs (ng_read_indexed_png) ---- */

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

uint8_t *ng_read_indexed_png(const char *path, int *w_out, int *h_out, uint8_t rgb[768]) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(n ? n : 1);
    if (fread(file, 1, n, f) != n) n = 0;
    fclose(f);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t *idat = NULL, *out = NULL;
    size_t idat_n = 0;
    int w = 0, h = 0, depth = 0, ok = n >= 8 && !memcmp(file, sig, 8);
    if (rgb) memset(rgb, 0, 768);
    for (size_t at = 8; ok && at + 12 <= n;) {
        uint32_t len = get32(file + at);
        const uint8_t *type = file + at + 4, *data = file + at + 8;
        if (at + 12 + len > n) { ok = 0; break; }
        if (!memcmp(type, "IHDR", 4)) {
            w = (int)get32(data); h = (int)get32(data + 4); depth = data[8];
            if (data[9] != 3 || data[12] != 0 || depth > 8) {
                fprintf(stderr, "error: %s is not an indexed, non-interlaced PNG\n", path);
                ok = 0;
            }
        } else if (!memcmp(type, "PLTE", 4)) {
            if (rgb) memcpy(rgb, data, len < 768 ? len : 768);
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = (uint8_t *)realloc(idat, idat_n + len);
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    int stride = (w * depth + 7) / 8, raw_n = 0;
    uint8_t *raw = ok && idat ? (uint8_t *)stbi_zlib_decode_malloc_guesssize_headerflag(
        (const char *)idat, (int)idat_n, h * (stride + 1), &raw_n, 1) : NULL;
    if (raw && raw_n >= h * (stride + 1)) {
        out = (uint8_t *)malloc((size_t)w * h);
        uint8_t *prev = (uint8_t *)calloc((size_t)stride, 1), *cur = (uint8_t *)malloc((size_t)stride);
        for (int y = 0; y < h; y++) {
            const uint8_t *line = raw + y * (stride + 1);
            for (int i = 0; i < stride; i++) {
                int a = i ? cur[i - 1] : 0, b = prev[i], c = i ? prev[i - 1] : 0, v = line[1 + i];
                switch (line[0]) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                }
                cur[i] = (uint8_t)v;
            }
            for (int x = 0; x < w; x++) {
                int bit = x * depth;
                out[y * w + x] = (uint8_t)((cur[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1));
            }
            memcpy(prev, cur, (size_t)stride);
        }
        free(prev); free(cur);
        *w_out = w; *h_out = h;
    } else if (ok) {
        fprintf(stderr, "error: cannot decode %s\n", path);
    }
    free(raw); free(idat); free(file);
    return out;
}
