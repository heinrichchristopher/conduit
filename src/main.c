/*
 * FTP-Server fuer die Original-Xbox mit Statusanzeige im Stil des Dashboards.
 *
 * Anzeige: IP-Adresse, freier Speicher je Partition, Verbindungen, Uebertragungen, Geschwindigkeit.
 * Bedienung: B oder BACK beendet und kehrt zum Dashboard zurueck
 *            (laeuft eine Uebertragung, muss B innerhalb von 3 s ein zweites Mal gedrueckt werden).
 */
#include <hal/video.h>
#include <hal/xbox.h>
#include <hal/debug.h>
#include <nxdk/mount.h>
#include <nxdk/net.h>
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <lwip/netifapi.h>
#include <lwip/ip4_addr.h>
#include <nvnetdrv.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <zlib.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "shared.h"
#include "gen_layout.h"
#include "dashassets.h"
#include "tunnel.h"

extern const unsigned char asset_overlay_z[];   /* SCREEN_W x SCREEN_H RGBA, zlib-gepackt */
extern const unsigned int  asset_overlay_z_len;
extern const unsigned char asset_cells_z[];     /* CELLS_W x CELLS_H Graustufen, zlib-gepackt */
extern const unsigned int  asset_cells_z_len;
extern const unsigned char asset_font_body[];
extern const unsigned int  asset_font_body_len;
extern const unsigned char asset_font_head[];
extern const unsigned int  asset_font_head_len;
extern struct netif *g_pnetif;

#define SCREEN_W 640
#define SCREEN_H 480

/* ------------------------------------------------------------------ Laufwerke */

drive_info_t g_drives[NUM_DRIVES] = {
    { 'C' }, { 'E' }, { 'F' }, { 'G' }, { 'X' }, { 'Y' }, { 'Z' },
};
static const char *DRIVE_DEV[NUM_DRIVES] = {
    "\\Device\\Harddisk0\\Partition2\\",
    "\\Device\\Harddisk0\\Partition1\\",
    "\\Device\\Harddisk0\\Partition6\\",
    "\\Device\\Harddisk0\\Partition7\\",
    "\\Device\\Harddisk0\\Partition3\\",
    "\\Device\\Harddisk0\\Partition4\\",
    "\\Device\\Harddisk0\\Partition5\\",
};

int drive_present(char letter)
{
    for (int i = 0; i < NUM_DRIVES; i++)
        if (g_drives[i].letter == letter) return g_drives[i].present;
    return 0;
}

static void drives_mount(void)
{
    for (int i = 0; i < NUM_DRIVES; i++) {
        if (!nxIsDriveMounted(g_drives[i].letter)) nxMountDrive(g_drives[i].letter, DRIVE_DEV[i]);
    }
}

static void drives_refresh(void)
{
    for (int i = 0; i < NUM_DRIVES; i++) {
        char root[4] = { g_drives[i].letter, ':', '\\', 0 };
        ULARGE_INTEGER avail, total, freeb;
        if (GetDiskFreeSpaceExA(root, &avail, &total, &freeb) && total.QuadPart > 0) {
            g_drives[i].total = total.QuadPart;
            g_drives[i].free = freeb.QuadPart;
            g_drives[i].present = 1;
        } else {
            g_drives[i].present = 0;
        }
    }
}

/* ------------------------------------------------------------------ Netzwerk */

static volatile LONG s_net_state; /* 0 startet, 1 bereit, -1 Fehler */

static DWORD WINAPI net_thread(LPVOID unused)
{
    (void)unused;
    int r = nxNetInit(NULL);
    if (r == -1 && g_pnetif == NULL) {
        /* Keine Netzwerkeinstellungen lesbar: DHCP versuchen */
        nx_net_parameters_t p;
        memset(&p, 0, sizeof(p));
        p.ipv4_mode = NX_NET_DHCP;
        p.ipv6_mode = NX_NET_DHCP;
        r = nxNetInit(&p);
    }
    if (g_pnetif == NULL) {
        InterlockedExchange(&s_net_state, -1);
        return 1;
    }
    /* r == -2: DHCP noch ohne Adresse, laeuft aber weiter */
    InterlockedExchange(&s_net_state, 1);
    ftpd_start();

    /* Dauer-Watchdog (nur bei DHCP):
     *  - Link-Wechsel (Kabel ab/an, anderes VLAN): lwIP wuerde bei Link-Up nur ein DHCP-REBOOT mit
     *    der alten IP versuchen, was in einem anderen Netz oft unbeantwortet bleibt. Stattdessen
     *    wird die alte Adresse verworfen und DHCP komplett neu gestartet (DISCOVER).
     *  - Bleibt DHCP bei vorhandenem Link ohne Adresse, wird es alle 15 s neu gestartet (30 x 500 ms). */
    int idle = 0;
    int was_up = netif_is_link_up(g_pnetif) ? 1 : 0;
    for (;;) {
        Sleep(500);
        if (!netif_dhcp_data(g_pnetif)) continue; /* feste Adresse: nichts zu tun */
        int up = netif_is_link_up(g_pnetif) ? 1 : 0;
        int restart = 0;
        if (up && !was_up) {
            restart = 1;
        } else if (!up && was_up) {
            /* Link weg: alte Adresse sofort verwerfen, damit keine veraltete IP angezeigt wird */
            netifapi_dhcp_stop(g_pnetif);
            netifapi_netif_set_addr(g_pnetif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        } else if (up) {
            const ip4_addr_t *a = netif_ip4_addr(g_pnetif);
            if (a && !ip4_addr_isany(a)) idle = 0;
            else if (++idle >= 30) { idle = 0; restart = 1; }
        }
        was_up = up;
        if (restart) {
            idle = 0;
            netifapi_dhcp_stop(g_pnetif);
            netifapi_netif_set_addr(g_pnetif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
            netifapi_dhcp_start(g_pnetif);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ Text-Cache */

typedef struct {
    TTF_Font   *font;
    SDL_Color   col;
    char        text[128];
    SDL_Texture *tex;
    int         w, h;
    int         used;
} text_entry_t;

#define TEXT_CACHE 160
static text_entry_t s_cache[TEXT_CACHE];
static SDL_Renderer *R;

static text_entry_t *text_get(TTF_Font *f, SDL_Color c, const char *s)
{
    for (int i = 0; i < TEXT_CACHE; i++) {
        text_entry_t *e = &s_cache[i];
        if (e->tex && e->font == f && e->col.r == c.r && e->col.g == c.g && e->col.b == c.b && e->col.a == c.a &&
            !strcmp(e->text, s)) {
            e->used = 1;
            return e;
        }
    }
    text_entry_t *slot = NULL;
    for (int i = 0; i < TEXT_CACHE && !slot; i++) if (!s_cache[i].tex) slot = &s_cache[i];
    if (!slot) {
        for (int i = 0; i < TEXT_CACHE && !slot; i++) if (!s_cache[i].used) slot = &s_cache[i];
        if (!slot) return NULL;
        SDL_DestroyTexture(slot->tex);
        slot->tex = NULL;
    }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return NULL;
    slot->tex = SDL_CreateTextureFromSurface(R, surf);
    slot->w = surf->w;
    slot->h = surf->h;
    SDL_FreeSurface(surf);
    if (!slot->tex) return NULL;
    SDL_SetTextureBlendMode(slot->tex, SDL_BLENDMODE_BLEND);
    slot->font = f;
    slot->col = c;
    snprintf(slot->text, sizeof(slot->text), "%s", s);
    slot->used = 1;
    return slot;
}

static void text_frame_end(void)
{
    for (int i = 0; i < TEXT_CACHE; i++) {
        if (s_cache[i].tex && !s_cache[i].used) {
            SDL_DestroyTexture(s_cache[i].tex);
            s_cache[i].tex = NULL;
        }
        s_cache[i].used = 0;
    }
}

enum { AL_LEFT, AL_RIGHT, AL_CENTER };

/* zeichnet Text; y ist die Oberkante. Gibt Breite zurueck. */
static int text(TTF_Font *f, SDL_Color c, int x, int y, int align, const char *s)
{
    if (!s || !*s) return 0;
    text_entry_t *e = text_get(f, c, s);
    if (!e) return 0;
    SDL_Rect d = { x, y, e->w, e->h };
    if (align == AL_RIGHT) d.x = x - e->w;
    else if (align == AL_CENTER) d.x = x - e->w / 2;
    SDL_RenderCopy(R, e->tex, NULL, &d);
    return e->w;
}

/* kuerzt s, bis es in maxw passt (mit "...") */
static void ellipsize(TTF_Font *f, const char *in, char *out, int outmax, int maxw)
{
    snprintf(out, outmax, "%s", in);
    int w;
    if (TTF_SizeUTF8(f, out, &w, NULL) == 0 && w <= maxw) return;
    int len = (int)strlen(out);
    while (len > 1) {
        len--;
        snprintf(out + len, outmax - len, "...");
        if (TTF_SizeUTF8(f, out, &w, NULL) == 0 && w <= maxw) return;
        out[len] = 0;
    }
}

static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(R, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, r, g, b, a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(R, &rc);
}

static void frame(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, r, g, b, a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderDrawRect(R, &rc);
}

/* Auswahlbalken wie im Dashboard: gelbgruen, oben und unten heller (Roehre), rechts unten abgeschraegt.
   Farben aus Screenshots des Original-Dashboards; vorher deutlich heller, das brannte auf LCDs ein. */
static void highlight_bar(int x, int y, int w, int h)
{
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
    for (int i = 0; i < h; i++) {
        int cut = i > h - 7 ? i - (h - 7) : 0;
        int d = 2 * i - (h - 1);                       /* -(h-1) .. h-1 */
        int k = 64 + 36 * d * d / ((h - 1) * (h - 1)); /* Mitte 64 %, Raender 100 % */
        SDL_SetRenderDrawColor(R, (Uint8)(148 * k / 100), (Uint8)(174 * k / 100), 0, 255);
        SDL_RenderDrawLine(R, x, y + i, x + w - 1 - cut, y + i);
    }
}

/* ------------------------------------------------------------------ Formatierung */

/* Zahl mit einer Nachkommastelle (Zehntel) als "12,3"; ab 100 ohne Nachkomma.
   Kein %f: die C-Bibliothek von nxdk formatiert keine Gleitkommazahlen. */
static void fmt_tenths(char *o, int max, uint64_t tenths, const char *unit)
{
    if (tenths >= 1000) snprintf(o, max, "%u %s", (unsigned)((tenths + 5) / 10), unit);
    else snprintf(o, max, "%u,%u %s", (unsigned)(tenths / 10), (unsigned)(tenths % 10), unit);
}

static void fmt_size(char *o, int max, uint64_t b)
{
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    if (b < 1024) { snprintf(o, max, "%u B", (unsigned)b); return; }
    int i = 0;
    uint64_t div = 1;
    while (b / div >= 1024 && i < 4) { div *= 1024; i++; }
    fmt_tenths(o, max, (b * 10 + div / 2) / div, u[i]);
}

static void fmt_speed(char *o, int max, double bps)
{
    uint64_t v = bps > 0 ? (uint64_t)bps : 0;
    if (v >= 1019 * 1024) fmt_tenths(o, max, (v * 10 + 524288) / 1048576, "MB/s");
    else snprintf(o, max, "%u KB/s", (unsigned)((v + 512) / 1024));
}

/* ------------------------------------------------------------------ Hintergrund */

static voidpf z_alloc(voidpf op, uInt n, uInt sz) { (void)op; return calloc(n, sz); }
static void z_free(voidpf op, voidpf p) { (void)op; free(p); }

/* Entpackt ein eingebettetes Bild (zlib) in einen neuen Puffer mit genau len Bytes; NULL bei Fehler */
static void *unpack(const unsigned char *src, unsigned int srclen, size_t len)
{
    void *out = malloc(len);
    if (!out) return NULL;
    z_stream z = { 0 };
    z.zalloc = z_alloc;   /* nxdk baut zlib mit Z_SOLO: ohne eigenen Allokator */
    z.zfree = z_free;
    z.next_in = (Bytef *)src;
    z.avail_in = srclen;
    z.next_out = out;
    z.avail_out = (uInt)len;
    int ok = inflateInit(&z) == Z_OK;
    ok = ok && inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == len;
    inflateEnd(&z);
    if (!ok) { free(out); return NULL; }
    return out;
}

/*
 * Rendert den Tunnel einmalig beim Start. Zellmuster: bevorzugt aus dem Original-Dashboard auf C:,
 * sonst das eingebettete eigene Muster (assets/cells.png).
 */
static SDL_Texture *make_tunnel_texture(void)
{
    int tw = 0, th = 0;
    uint8_t *cells = dash_load_cells(&tw, &th);
    /* Das Dashboard-Muster hat weniger, groessere Zellen je Kachel als das eigene: dichter kacheln */
    double rep = cells ? 1.5 : 1.0;
    if (!cells) {
        tw = CELLS_W;
        th = CELLS_H;
        cells = unpack(asset_cells_z, asset_cells_z_len, (size_t)tw * th);
    }
    void *ov = unpack(asset_overlay_z, asset_overlay_z_len, (size_t)SCREEN_W * SCREEN_H * 4);
    SDL_Surface *ovs = ov ? SDL_CreateRGBSurfaceWithFormatFrom(ov, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_Surface *img = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture *tex = NULL;
    if (cells && ovs && img) {
        tunnel_render((uint32_t *)img->pixels, SCREEN_W, SCREEN_H, cells, tw, th, rep);
        /* Panel-Overlay einmalig einrechnen: spart pro Bild einen Alpha-Blend ueber den ganzen Schirm */
        SDL_SetSurfaceBlendMode(ovs, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(ovs, NULL, img, NULL);
        tex = SDL_CreateTextureFromSurface(R, img);
        /* deckend kopieren (ARGB-Texturen wuerden sonst jedes Bild per Alpha geblendet) */
        if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    }
    SDL_FreeSurface(img);
    SDL_FreeSurface(ovs);
    free(ov);
    free(cells);
    return tex;
}

/* ------------------------------------------------------------------ Beenden */

/*
 * Zurueck zum Dashboard. Frueher stand hier SDL_RenderClear, das mit der zuletzt benutzten Zeichenfarbe loescht;
 * dazu wird der noch angezeigte Bildspeicher beim Neustart stellenweise ueberschrieben. Ergebnis war ein gruener
 * Schirm mit Stoerstreifen, bis das Dashboard ein eigenes Bild hatte.
 * Jetzt: ein volles schwarzes Bild ausgeben und danach die Videoausgabe abschalten; das Dashboard schaltet sie
 * beim Start selbst wieder ein. (Getestet und verworfen: die Gammatabelle auf null setzen statt abzuschalten,
 * das blitzte mit Komponentenkabel staerker. Den Bildspeicher per AvSetSavedDataAddress zu uebergeben, bringt
 * das Dashboard in xemu zum Absturz.)
 */
static void return_to_dashboard(void)
{
    /* Netzwerkkarte anhalten: nxNetShutdown() ist in nxdk leer, die Karte schriebe sonst waehrend des Neustarts
       eingehende Pakete per DMA weiter in ihre Empfangspuffer, deren Speicher dann schon neu vergeben wird. */
    if (g_pnetif) nvnetdrv_stop_txrx();
    VIDEO_MODE vm = XVideoGetMode();
    unsigned char *fb = XVideoGetFB();
    if (fb) {
        memset(fb, 0, (size_t)vm.width * vm.height * ((vm.bpp + 7) / 8));
        XVideoFlushFB();
        XVideoWaitForVBlank();
        XVideoWaitForVBlank();
        XVideoSetVideoEnable(FALSE);
    }
    XLaunchXBE(NULL);
}

/* ------------------------------------------------------------------ Hauptprogramm */

#define GRAPH_N 54

int main(void)
{
    XVideoSetMode(SCREEN_W, SCREEN_H, 32, REFRESH_DEFAULT);
    InitializeCriticalSection(&g_lock);
    drives_mount();
    drives_refresh();

    HANDLE nt = CreateThread(NULL, 64 * 1024, net_thread, NULL, 0, NULL);
    if (nt) CloseHandle(nt);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0 || TTF_Init() != 0) {
        debugPrint("SDL konnte nicht gestartet werden: %s\n", SDL_GetError());
        Sleep(5000);
        return_to_dashboard();
    }
    SDL_Window *win = SDL_CreateWindow("FTP", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    R = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *bg = R ? make_tunnel_texture() : NULL;

    TTF_Font *f_big   = TTF_OpenFontRW(SDL_RWFromConstMem(asset_font_body, asset_font_body_len), 1, 25);
    TTF_Font *f_body  = TTF_OpenFontRW(SDL_RWFromConstMem(asset_font_body, asset_font_body_len), 1, 17);
    TTF_Font *f_small = TTF_OpenFontRW(SDL_RWFromConstMem(asset_font_body, asset_font_body_len), 1, 15);
    TTF_Font *f_label = TTF_OpenFontRW(SDL_RWFromConstMem(asset_font_head, asset_font_head_len), 1, 9);
    TTF_Font *f_drive = TTF_OpenFontRW(SDL_RWFromConstMem(asset_font_head, asset_font_head_len), 1, 11);
    if (!R || !bg || !f_big || !f_body || !f_small || !f_label || !f_drive) {
        debugPrint("Grafik konnte nicht geladen werden: %s\n", SDL_GetError());
        Sleep(5000);
        return_to_dashboard();
    }

    const SDL_Color C_TXT   = { 214, 236, 190, 255 };
    const SDL_Color C_DIM   = { 128, 170, 88, 255 };
    const SDL_Color C_LABEL = { 150, 200, 60, 255 };
    const SDL_Color C_DARK  = { 16, 34, 4, 255 };
    const SDL_Color C_WARN  = { 250, 220, 60, 255 };

    SDL_GameController *pads[4] = { 0 };

    double speed_total = 0, speed_send = 0, speed_recv = 0;
    double sess_speed[MAX_SESSIONS] = { 0 };
    uint64_t sess_prev[MAX_SESSIONS] = { 0 };
    int sess_prev_x[MAX_SESSIONS] = { 0 };
    float graph[GRAPH_N] = { 0 };
    uint64_t prev_sent = 0, prev_recv = 0;
    DWORD t_sample = GetTickCount();
    DWORD t_drives = GetTickCount();
    DWORD t_busy = 0;
    DWORD quit_ask = 0;

    int frame_no = 0;
    uint32_t last_sig = 0xFFFFFFFFu;
    uint32_t last_ip = 0;
    for (;;) {
        if (frame_no < 3) frame_no++;
        /* ---------------- Eingabe */
        SDL_Event ev;
        int want_quit = 0;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_GameController *p = SDL_GameControllerOpen(ev.cdevice.which);
                for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = p; break; }
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                SDL_GameController *p = SDL_GameControllerFromInstanceID(ev.cdevice.which);
                for (int i = 0; i < 4; i++) if (pads[i] == p) { SDL_GameControllerClose(p); pads[i] = NULL; }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B || ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
                    want_quit = 1;
            } else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_BACKSPACE) want_quit = 1;
            }
        }

        /* ---------------- Zustand einsammeln */
        session_info_t ss[MAX_SESSIONS];
        uint64_t sent, recv;
        EnterCriticalSection(&g_lock);
        memcpy(ss, g_sess, sizeof(ss));
        sent = g_bytes_sent;
        recv = g_bytes_recv;
        LeaveCriticalSection(&g_lock);

        int n_conn = 0, n_xfer = 0;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (ss[i].used) n_conn++;
            if (ss[i].used && ss[i].xfer) n_xfer++;
        }

        DWORD now = GetTickCount();
        if (want_quit) {
            if (n_xfer == 0 || (quit_ask && now - quit_ask < 3000)) break;
            quit_ask = now;
        }
        if (quit_ask && now - quit_ask >= 3000) quit_ask = 0;

        int need_draw = frame_no < 3;
        if (now - t_sample >= 1000) {
            need_draw = 1;   /* Zahlen und Verlauf aendern sich nur hier (1x pro Sekunde) */
            double dt = (now - t_sample) / 1000.0;
            double ss_ = (sent - prev_sent) / dt, rr = (recv - prev_recv) / dt;
            speed_send = speed_send * 0.4 + ss_ * 0.6;
            speed_recv = speed_recv * 0.4 + rr * 0.6;
            if (speed_send < 512) speed_send = 0;
            if (speed_recv < 512) speed_recv = 0;
            speed_total = speed_send + speed_recv;
            prev_sent = sent;
            prev_recv = recv;
            memmove(graph, graph + 1, sizeof(float) * (GRAPH_N - 1));
            graph[GRAPH_N - 1] = (float)speed_total;
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (ss[i].used && ss[i].xfer && sess_prev_x[i] == ss[i].xfer && ss[i].done >= sess_prev[i]) {
                    double v = (ss[i].done - sess_prev[i]) / dt;
                    sess_speed[i] = sess_speed[i] * 0.4 + v * 0.6;
                } else {
                    sess_speed[i] = 0;
                }
                sess_prev[i] = ss[i].done;
                sess_prev_x[i] = ss[i].used ? ss[i].xfer : 0;
            }
            t_sample = now;
        }
        /* Freier Speicher: waehrend Uebertragungen selten abfragen (der FATX-Treiber muss dafuer ggf. die FAT
           durchzaehlen und konkurriert dann mit der Uebertragung um die Platte). Bei vielen kleinen Dateien ist
           zwischen zwei Dateien kurz keine Uebertragung aktiv; erst nach 1,5 s Ruhe gilt das als Leerlauf. */
        if (n_xfer) t_busy = now;
        int busy = now - t_busy < 1500;
        if ((!busy && InterlockedExchange(&g_drives_dirty, 0)) || now - t_drives >= (busy ? 30000u : 5000u)) {
            drives_refresh();
            t_drives = now;
            need_draw = 1;
        }
        {
            /* Zustandsaenderungen, die sofort sichtbar sein sollen */
            uint32_t ip = 0;
            int link = 0;
            if (s_net_state == 1 && g_pnetif) {
                const ip4_addr_t *a = netif_ip4_addr(g_pnetif);
                if (a) ip = ip4_addr_get_u32(a);
                link = netif_is_link_up(g_pnetif) ? 1 : 0;
            }
            uint32_t sig = (uint32_t)n_conn | ((uint32_t)n_xfer << 4) | ((uint32_t)(s_net_state + 1) << 8) |
                           ((uint32_t)(g_server_state + 1) << 10) | ((uint32_t)(quit_ask != 0) << 12) | ((uint32_t)link << 13);
            for (int i = 0; i < MAX_SESSIONS; i++) sig ^= (uint32_t)(ss[i].logged_in | (ss[i].xfer << 1)) << (14 + (i % 6) * 3);
            if (sig != last_sig || ip != last_ip) need_draw = 1;
            last_sig = sig;
            last_ip = ip;
        }
        if (!need_draw) {
            Sleep(50);
            continue;
        }

        /* ---------------- Zeichnen */
        SDL_RenderCopy(R, bg, NULL, NULL);
        char b1[128], b2[128], b3[160];

        /* Titelzeile rechts: Status */
        {
            const char *st;
            SDL_Color c = C_DIM;
            if (s_net_state < 0) { st = "NETZWERKFEHLER"; c = C_WARN; }
            else if (s_net_state == 0) st = "NETZWERK STARTET ...";
            else if (g_server_state < 0) { st = "PORT 21 BELEGT"; c = C_WARN; }
            else if (g_server_state == 0) st = "SERVER STARTET ...";
            else if (n_xfer) { st = "ÜBERTRAGUNG LÄUFT"; c = C_TXT; }
            else st = "BEREIT";
            text(f_label, c, PANEL_MEM_X1, TITLE_Y + 22, AL_RIGHT, st);
        }

        /* --- Netzwerk */
        {
            int x = PANEL_NET_X0 + 18, y = PANEL_NET_Y0 + 32;
            text(f_label, C_LABEL, x, y, AL_LEFT, "IP-ADRESSE");
            int up = 0;
            char ip[20] = "";
            if (s_net_state == 1 && g_pnetif) {
                const ip4_addr_t *a = netif_ip4_addr(g_pnetif);
                if (a && !ip4_addr_isany(a)) { up = 1; snprintf(ip, sizeof(ip), "%s", ip4addr_ntoa(a)); }
            }
            int bar_y = y + 14, bar_h = 30;
            highlight_bar(x - 6, bar_y, PANEL_NET_X1 - PANEL_NET_X0 - 36, bar_h);
            /* senkrecht mittig nach Ziffernhoehe (nicht Zeilenhoehe, die Unterlaengen einrechnet) */
            TTF_Font *fi = up ? f_big : f_body;
            int maxy = 0;
            if (TTF_GlyphMetrics(fi, '0', NULL, NULL, NULL, &maxy, NULL) != 0) maxy = TTF_FontAscent(fi) * 7 / 10;
            int ty = bar_y + bar_h / 2 - TTF_FontAscent(fi) + maxy / 2;
            if (up) text(f_big, C_DARK, x + 2, ty, AL_LEFT, ip);
            else text(f_body, C_DARK, x + 2, ty, AL_LEFT, s_net_state < 0 ? "kein Netzwerk" : "warte auf Adresse ...");

            int ry = y + 52, rx = x + 96;
            text(f_small, C_DIM, x, ry, AL_LEFT, "Port");
            snprintf(b1, sizeof(b1), "%d", FTP_PORT);
            text(f_small, C_TXT, rx, ry, AL_LEFT, b1);
            text(f_small, C_DIM, x, ry + 17, AL_LEFT, "Benutzer");
            text(f_small, C_TXT, rx, ry + 17, AL_LEFT, FTP_USER);
            text(f_small, C_DIM, x, ry + 34, AL_LEFT, "Passwort");
            text(f_small, C_TXT, rx, ry + 34, AL_LEFT, FTP_PASS);
            text(f_small, C_DIM, x, ry + 51, AL_LEFT, "Adresse");
            const char *mode = "-";
            if (s_net_state == 1 && g_pnetif) {
                if (!netif_is_link_up(g_pnetif)) mode = "kein Kabel";
                else mode = netif_dhcp_data(g_pnetif) ? "per DHCP" : "fest eingestellt";
            }
            text(f_small, C_TXT, rx, ry + 51, AL_LEFT, mode);
        }

        /* --- Speicher */
        {
            int x = PANEL_MEM_X0 + 18, y = PANEL_MEM_Y0 + 33;
            text(f_label, C_LABEL, PANEL_MEM_X1 - 14, PANEL_MEM_Y0 + 8, AL_RIGHT, "FREI / GESAMT");
            int row = 0, nd = 0;
            for (int i = 0; i < NUM_DRIVES; i++) nd += g_drives[i].present;
            int rh = nd > 6 ? 17 : 19;
            for (int i = 0; i < NUM_DRIVES; i++) {
                drive_info_t *d = &g_drives[i];
                if (!d->present) continue;
                int ry = y + row * rh;
                char l[3] = { d->letter, ':', 0 };
                text(f_drive, C_TXT, x, ry + 2, AL_LEFT, l);
                /* Fuellstand: belegt = hell */
                int bx = x + 30, bw = 76, by = ry + 6, bh = 7;
                fill(bx, by, bw, bh, 10, 30, 6, 255);
                int used = d->total ? (int)((double)(d->total - d->free) / (double)d->total * (bw - 2) + 0.5) : 0;
                SDL_Color bc = (d->total && d->free * 10 < d->total) ? (SDL_Color){ 230, 200, 40, 255 } : (SDL_Color){ 150, 214, 30, 255 };
                fill(bx + 1, by + 1, used, bh - 2, bc.r, bc.g, bc.b, 255);
                frame(bx, by, bw, bh, 90, 140, 30, 255);
                fmt_size(b1, sizeof(b1), d->free);
                fmt_size(b2, sizeof(b2), d->total);
                snprintf(b3, sizeof(b3), "%s / %s", b1, b2);
                text(f_small, C_TXT, PANEL_MEM_X1 - 16, ry, AL_RIGHT, b3);
                row++;
            }
            if (nd == 0) text(f_small, C_DIM, x, y, AL_LEFT, "keine Partition gefunden");
        }

        /* --- Aktivitaet */
        {
            int x = PANEL_ACT_X0 + 18, y = PANEL_ACT_Y0 + 32;
            text(f_label, C_LABEL, x, y, AL_LEFT, "VERBINDUNGEN");
            snprintf(b1, sizeof(b1), "%d", n_conn);
            text(f_big, C_TXT, x, y + 11, AL_LEFT, b1);
            text(f_label, C_LABEL, x + 118, y, AL_LEFT, "ÜBERTRAGUNGEN");
            snprintf(b1, sizeof(b1), "%d", n_xfer);
            text(f_big, C_TXT, x + 118, y + 11, AL_LEFT, b1);
            text(f_label, C_LABEL, x + 244, y, AL_LEFT, "GESCHWINDIGKEIT");
            fmt_speed(b1, sizeof(b1), speed_total);
            text(f_big, C_TXT, x + 244, y + 11, AL_LEFT, b1);

            /* Verlauf der letzten GRAPH_N Sekunden (ein Balken je Sekunde) */
            int gx = PANEL_ACT_X1 - 136, gy = y + 2, gw = 118, gh = 38;
            fill(gx, gy, gw, gh, 6, 22, 4, 255);
            float mx = 1024 * 1024;
            for (int i = 0; i < GRAPH_N; i++) if (graph[i] > mx) mx = graph[i];
            for (int i = 0; i < GRAPH_N; i++) {
                int h = (int)(graph[i] / mx * (gh - 4));
                if (h > 0) fill(gx + 3 + i * 2 + (gw - 6 - GRAPH_N * 2), gy + gh - 2 - h, 2, h, 140, 210, 30, 255);
            }
            frame(gx, gy, gw, gh, 90, 140, 30, 255);
            fmt_speed(b1, sizeof(b1), speed_recv);
            fmt_speed(b2, sizeof(b2), speed_send);
            snprintf(b3, sizeof(b3), "EIN %s   AUS %s", b1, b2);
            text(f_label, C_DIM, gx + gw, gy + gh + 4, AL_RIGHT, b3);

            /* Liste der Verbindungen */
            int ly = y + 62;
            fill(x - 6, ly - 4, PANEL_ACT_X1 - PANEL_ACT_X0 - 24, 1, 70, 120, 20, 255);
            /* zuerst die Uebertragungen, danach die uebrigen Verbindungen: sonst verdraengen untaetige Sitzungen
               die interessanten Zeilen in "+ n weitere" */
            int shown = 0, more = 0;
            for (int k = 0; k < 2 * MAX_SESSIONS; k++) {
                int i = k % MAX_SESSIONS;
                session_info_t *s = &ss[i];
                if (!s->used || (k < MAX_SESSIONS) != (s->xfer != 0)) continue;
                if (shown >= 3) { more++; continue; }
                int ry = ly + shown * 19;
                text(f_small, C_TXT, x, ry, AL_LEFT, s->ip);
                const char *st = !s->logged_in ? "Anmeldung" : s->xfer == XFER_RECV ? "empfängt" : s->xfer == XFER_SEND ? "sendet" : "verbunden";
                text(f_small, s->xfer ? C_LABEL : C_DIM, x + 104, ry, AL_LEFT, st);
                if (s->xfer) {
                    ellipsize(f_small, s->file, b1, sizeof(b1), 150);
                    text(f_small, C_TXT, x + 180, ry, AL_LEFT, b1);
                    fmt_speed(b2, sizeof(b2), sess_speed[i]);
                    if (s->xfer == XFER_SEND && s->total) {
                        if (s->done > s->total) s->done = s->total;
                        int pct = (int)(s->done * 100 / s->total);
                        snprintf(b3, sizeof(b3), "%d %%   %s", pct, b2);
                        int px = x + 338, pw = 56, py = ry + 7;
                        fill(px, py, pw, 6, 10, 30, 6, 255);
                        fill(px + 1, py + 1, (int)((pw - 2) * s->done / s->total), 4, 150, 214, 30, 255);
                        frame(px, py, pw, 6, 90, 140, 30, 255);
                    } else {
                        fmt_size(b1, sizeof(b1), s->done);
                        snprintf(b3, sizeof(b3), "%s   %s", b1, b2);
                    }
                    text(f_small, C_TXT, PANEL_ACT_X1 - 18, ry, AL_RIGHT, b3);
                }
                shown++;
            }
            if (more) {
                snprintf(b1, sizeof(b1), "+ %d weitere", more);
                text(f_small, C_DIM, PANEL_ACT_X1 - 18, ly + 3 * 19 - 4, AL_RIGHT, b1);
            }
            if (shown == 0) {
                text(f_small, C_DIM, x, ly, AL_LEFT, "Keine Verbindung. Im FTP-Programm die IP-Adresse oben eintragen,");
                text(f_small, C_DIM, x, ly + 18, AL_LEFT, "Benutzer und Passwort: " FTP_USER " / " FTP_PASS ", passiver Modus.");
            }
        }

        /* --- Fusszeile */
        if (quit_ask) {
            text(f_body, C_WARN, BTN_B_X + 26, BTN_B_Y - 10, AL_LEFT, "Übertragung läuft - zum Beenden B erneut drücken");
        } else {
            fmt_size(b1, sizeof(b1), recv);
            fmt_size(b2, sizeof(b2), sent);
            snprintf(b3, sizeof(b3), "Empfangen %s  ·  Gesendet %s", b1, b2);
            text(f_small, C_DIM, PANEL_ACT_X1, BTN_B_Y - 9, AL_RIGHT, b3);
        }

        SDL_RenderPresent(R);
        text_frame_end();

        /* Eingaben werden alle 50 ms geprueft, gezeichnet wird 1x pro Sekunde und bei Zustandsaenderungen (s. need_draw) */
        DWORD spent = GetTickCount() - now;
        if (spent < 50) Sleep(50 - spent);
    }

    return_to_dashboard();
    return 0;
}
