/*
 * Liest eine Textur aus den XIP-Archiven des Original-Dashboards.
 *
 * XIP0: Kopf { "XIP0", u32 Datenbeginn, u16 Anzahl Dateien, u16 Anzahl Namen, u32 Datengroesse },
 *       dann je Datei { u32 Offset, u32 Groesse, u32 Typ, u32 Zeit }, je Name { u16 Dateinummer, u16 Namensoffset },
 *       dann die Namenstabelle (nullterminiert); die Daten beginnen bei "Datenbeginn".
 * XPR0 (.xbx): { "XPR0", u32 Gesamtgroesse, u32 Kopfgroesse, ... u32 Format bei 0x18 }, Pixel ab Kopfgroesse.
 *       Format: Bits 8..15 Typ (0x06 A8R8G8B8 verwuerfelt, 0x0E DXT3, 0x0F DXT5), Bits 20..23 / 24..27 = log2(Breite/Hoehe).
 * Gebraucht wird nur der Alphakanal: dort liegen die Linien des Zellmusters.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "dashassets.h"

#define MAX_HEADER  (4u * 1024 * 1024)
#define MAX_FILE    (4u * 1024 * 1024)

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static int read_at(HANDLE h, uint32_t off, void *buf, uint32_t len)
{
    LARGE_INTEGER li;
    li.QuadPart = off;
    DWORD got = 0;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, len, &got, NULL) && got == len;
}

/* Datei "name" (Vergleich ohne Verzeichnis, ohne Gross-/Kleinschreibung) aus einem XIP lesen. */
static uint8_t *xip_extract(const char *path, const char *name, uint32_t *size_out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    uint8_t *hdr = NULL, *out = NULL;
    uint8_t head[16];
    if (!read_at(h, 0, head, 16) || memcmp(head, "XIP0", 4) != 0) goto done;
    uint32_t dataoff = rd32(head + 4), nfiles = rd16(head + 8), nnames = rd16(head + 10);
    if (dataoff < 16 || dataoff > MAX_HEADER) goto done;
    if (16 + nfiles * 16 + nnames * 4 > dataoff) goto done;

    hdr = malloc(dataoff);
    if (!hdr || !read_at(h, 0, hdr, dataoff)) goto done;
    uint32_t names = 16 + nfiles * 16, strtab = names + nnames * 4;

    for (uint32_t i = 0; i < nnames; i++) {
        uint32_t idx = rd16(hdr + names + i * 4), noff = rd16(hdr + names + i * 4 + 2);
        if (idx >= nfiles || strtab + noff >= dataoff) continue;
        const char *n = (const char *)hdr + strtab + noff;
        size_t maxlen = dataoff - (strtab + noff);
        if (memchr(n, 0, maxlen) == NULL) continue;
        const char *base = n;
        for (const char *c = n; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
        if (_stricmp(base, name) != 0) continue;

        uint32_t foff = rd32(hdr + 16 + idx * 16), fsize = rd32(hdr + 16 + idx * 16 + 4);
        if (fsize == 0 || fsize > MAX_FILE) goto done;
        out = malloc(fsize);
        if (out && !read_at(h, dataoff + foff, out, fsize)) { free(out); out = NULL; }
        if (out) *size_out = fsize;
        goto done;
    }
done:
    free(hdr);
    CloseHandle(h);
    return out;
}

/* Verwuerfelte (Morton-)Adresse eines Pixels, wie in der GPU-Speicheranordnung. */
static uint32_t swizzle_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r = 0, bit = 0, sx = 0, sy = 0;
    while (w > 1 || h > 1) {
        if (w > 1) { r |= ((x >> sx) & 1) << bit; bit++; sx++; w >>= 1; }
        if (h > 1) { r |= ((y >> sy) & 1) << bit; bit++; sy++; h >>= 1; }
    }
    return r;
}

/* Alphakanal der obersten Stufe einer .xbx als w*h Bytes. */
static uint8_t *xbx_alpha(const uint8_t *d, uint32_t size, int *wo, int *ho)
{
    if (size < 0x20 || memcmp(d, "XPR0", 4) != 0) return NULL;
    uint32_t hdr = rd32(d + 8), fmt = rd32(d + 0x18);
    uint32_t type = (fmt >> 8) & 0xFF;
    uint32_t w = 1u << ((fmt >> 20) & 0xF), h = 1u << ((fmt >> 24) & 0xF);
    if (w < 4 || h < 4 || w > 1024 || h > 1024 || hdr >= size) return NULL;
    const uint8_t *px = d + hdr;
    uint32_t avail = size - hdr;

    uint8_t *a = malloc(w * h);
    if (!a) return NULL;

    if (type == 0x0E || type == 0x0F) {                 /* DXT3 / DXT5, Bloecke zeilenweise */
        uint32_t bw = w / 4, bh = h / 4;
        if (avail < bw * bh * 16) { free(a); return NULL; }
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *b = px + (by * bw + bx) * 16;
                uint8_t al[16];
                if (type == 0x0E) {                     /* 4 Bit je Pixel */
                    for (int i = 0; i < 16; i++) {
                        uint8_t v = (b[i >> 1] >> ((i & 1) * 4)) & 0xF;
                        al[i] = (uint8_t)(v * 17);
                    }
                } else {                                /* 2 Endwerte + 3 Bit Index je Pixel */
                    uint8_t a0 = b[0], a1 = b[1], pal[8];
                    pal[0] = a0; pal[1] = a1;
                    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
                    else {
                        for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
                        pal[6] = 0; pal[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++) al[i] = pal[(bits >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; i++) a[(by * 4 + (i >> 2)) * w + bx * 4 + (i & 3)] = al[i];
            }
    } else if (type == 0x06) {                          /* A8R8G8B8 verwuerfelt: Bytes B G R A */
        if (avail < w * h * 4) { free(a); return NULL; }
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                a[y * w + x] = px[swizzle_index(x, y, w, h) * 4 + 3];
    } else {
        free(a);
        return NULL;
    }
    *wo = (int)w;
    *ho = (int)h;
    return a;
}

/*
 * Linien um je ein Pixel duenner machen (Minimum ueber 3x3, kachelbar am Rand umlaufend). Das Dashboard-Muster hat
 * etwa 8 Pixel breite Linien, die im Tunnel deutlich dicker und heller wirken als im Original-Dashboard.
 */
static void thin_lines(uint8_t *a, int w, int h)
{
    uint8_t *src = malloc((size_t)w * h);
    if (!src) return;
    memcpy(src, a, (size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t m = 255;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    uint8_t v = src[((y + dy) & (h - 1)) * w + ((x + dx) & (w - 1))];   /* w, h: Zweierpotenzen */
                    if (v < m) m = v;
                }
            a[y * w + x] = m;
        }
    free(src);
}

uint8_t *dash_load_cells(int *w, int *h)
{
    /* Datenordner heisst je nach Dashboard-Version anders (z. B. xboxdashdata.185ead00) */
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("C:\\*", &fd);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    uint8_t *result = NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (_strnicmp(fd.cFileName, "xboxdashdata.", 13) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "C:\\%s\\default.xip", fd.cFileName);
        uint32_t size = 0;
        uint8_t *xbx = xip_extract(path, "cellwall.xbx", &size);
        if (!xbx) continue;
        int tw = 0, th = 0;
        uint8_t *a = xbx_alpha(xbx, size, &tw, &th);
        free(xbx);
        if (!a) continue;
        /* Plausibilitaet: ein Linienmuster hat wenige helle Pixel, sonst ist es etwas anderes */
        uint32_t lit = 0, n = (uint32_t)tw * th;
        for (uint32_t i = 0; i < n; i++) lit += a[i] > 128;
        if (lit * 100 < n * 3 || lit * 100 > n * 40) { free(a); continue; }
        thin_lines(a, tw, th);
        *w = tw;
        *h = th;
        result = a;
        break;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return result;
}
