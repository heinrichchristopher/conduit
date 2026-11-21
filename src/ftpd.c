/*
 * Kleiner FTP-Server fuer die Xbox (lwIP-Sockets, ein Thread pro Verbindung).
 *
 * Virtuelles Dateisystem:
 *   /            Laufwerksliste (C, E, F, G, X, Y, Z - nur vorhandene)
 *   /E/Spiele    -> E:\Spiele
 *
 * Unterstuetzt: USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT EPRT
 *               LIST NLST MLSD MLST RETR STOR APPE REST DELE MKD RMD RNFR RNTO SIZE MDTM
 *               NOOP QUIT ABOR ALLO HELP STAT CLNT (+ X-Varianten)
 */
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netif.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>

#include "shared.h"

#define CMD_MAX       1024
#define XFER_BUF      (128 * 1024)
#define NBUF          3           /* Puffer je Session fuer die Pipeline */
#define IDLE_TIMEOUT  (15 * 60)   /* Sekunden ohne Befehl */
#define DATA_TIMEOUT  20          /* Sekunden fuer Datenverbindung / Stillstand */

session_info_t   g_sess[MAX_SESSIONS];
CRITICAL_SECTION g_lock;
volatile uint64_t g_bytes_sent;
volatile uint64_t g_bytes_recv;
volatile LONG     g_drives_dirty;
volatile LONG     g_server_state;

static volatile LONG s_pasv_next;
extern struct netif *g_pnetif;

typedef struct {
    int  slot;
    int  ctrl;                 /* Kontroll-Socket */
    int  pasv;                 /* Listen-Socket fuer PASV, -1 */
    struct sockaddr_in port_addr; /* Ziel fuer PORT */
    int  have_port;
    struct sockaddr_in local;  /* eigene Adresse der Kontrollverbindung */
    struct sockaddr_in peer;   /* Adresse des Clients */
    char cwd[512];             /* virtueller Pfad, beginnt mit / */
    char user[32];
    int  logged_in;
    uint64_t rest;
    char rnfr[512];            /* nativer Pfad nach RNFR */
    char rbuf[CMD_MAX];        /* Zeilenpuffer */
    int  rlen;
    int  discard;              /* Rest einer zu langen Zeile verwerfen */
    char *buf;                 /* = bufs[0], fuer Verzeichnislisten */
    char *bufs[NBUF];          /* Transferpuffer, sektor-/seitenausgerichtet */
    void *bufmem;              /* Speicherblock dazu */
    int  quit;
} sess_t;

/* ------------------------------------------------------------------ Hilfen */

static void stat_add(int dir, uint32_t n)
{
    EnterCriticalSection(&g_lock);
    if (dir == XFER_SEND) g_bytes_sent += n; else g_bytes_recv += n;
    LeaveCriticalSection(&g_lock);
}

static int send_all(int s, const char *p, int len)
{
    while (len > 0) {
        int n = lwip_send(s, p, len, 0);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static void reply(sess_t *s, const char *fmt, ...)
{
    char out[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(out) - 3) n = sizeof(out) - 3;
    out[n++] = '\r'; out[n++] = '\n';
    send_all(s->ctrl, out, n);
}

/* wartet bis Socket lesbar ist; 1 = lesbar, 0 = Timeout, -1 = Fehler */
static int wait_readable(int fd, int seconds, int usec)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = { seconds, usec };
    int r = lwip_select(fd + 1, &rs, NULL, NULL, &tv);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

/* Liest eine Zeile von der Kontrollverbindung. 1 = ok, 0 = Timeout, -1 = Verbindung zu */
static int read_line(sess_t *s, char *line, int max, int timeout)
{
    for (;;) {
        for (int i = 0; i < s->rlen; i++) {
            if (s->rbuf[i] == '\n') {
                if (s->discard) {
                    s->discard = 0;
                    memmove(s->rbuf, s->rbuf + i + 1, s->rlen - i - 1);
                    s->rlen -= i + 1;
                    i = -1;
                    continue;
                }
                int l = i;
                if (l > 0 && s->rbuf[l - 1] == '\r') l--;
                if (l >= max) l = max - 1;
                int o = 0;
                for (int k = 0; k < l; k++) {
                    unsigned char c = (unsigned char)s->rbuf[k];
                    if (c == 0xFF) { k++; continue; }      /* Telnet IAC + Befehl */
                    if (c < 0x20 && c != '\t') continue;
                    line[o++] = (char)c;
                }
                line[o] = 0;
                memmove(s->rbuf, s->rbuf + i + 1, s->rlen - i - 1);
                s->rlen -= i + 1;
                return 1;
            }
        }
        if (s->rlen >= CMD_MAX - 1) { s->rlen = 0; s->discard = 1; }  /* zu lange Zeile verwerfen */
        if (timeout > 0) {
            /* in 10-s-Schritten warten; hat die Xbox inzwischen eine andere IP (Netzwechsel),
               ist die Verbindung tot und der Slot wird freigegeben */
            int left = timeout, w = 0;
            while (left > 0) {
                int step = left < 10 ? left : 10;
                w = wait_readable(s->ctrl, step, 0);
                if (w != 0) break;
                left -= step;
                const ip4_addr_t *a = g_pnetif ? netif_ip4_addr(g_pnetif) : NULL;
                if (a && ip4_addr_get_u32(a) != s->local.sin_addr.s_addr) return -1;
            }
            if (w == 0) return 0;
            if (w < 0) return -1;
        } else if (timeout == 0) {
            int w = wait_readable(s->ctrl, 0, 0);
            if (w == 0) return 0;
            if (w < 0) return -1;
        }
        int n = lwip_recv(s->ctrl, s->rbuf + s->rlen, CMD_MAX - 1 - s->rlen, 0);
        if (n <= 0) return -1;
        s->rlen += n;
    }
}

static void set_xfer(sess_t *s, int dir, const char *native, uint64_t done, uint64_t total)
{
    const char *b = strrchr(native, '\\');
    b = b ? b + 1 : native;
    EnterCriticalSection(&g_lock);
    session_info_t *si = &g_sess[s->slot];
    si->xfer = dir;
    strncpy(si->file, b, sizeof(si->file) - 1);
    si->file[sizeof(si->file) - 1] = 0;
    si->done = done;
    si->total = total;
    LeaveCriticalSection(&g_lock);
}

static void upd_xfer(sess_t *s, uint64_t done)
{
    EnterCriticalSection(&g_lock);
    g_sess[s->slot].done = done;
    LeaveCriticalSection(&g_lock);
}

static void end_xfer(sess_t *s)
{
    EnterCriticalSection(&g_lock);
    g_sess[s->slot].xfer = XFER_NONE;
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ Pfade */

/* Normalisiert virtuellen Pfad: arg relativ zu cwd -> out (beginnt mit /, ohne . und ..) */
static void vpath_resolve(const char *cwd, const char *arg, char *out, int max)
{
    char tmp[1024];
    if (!arg || !*arg) arg = ".";
    if (arg[0] == '/' || arg[0] == '\\') snprintf(tmp, sizeof(tmp), "%s", arg);
    else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);

    char *seg[64];
    int n = 0;
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *t = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (n > 0) n--; continue; }
        if (n >= 64) { snprintf(out, max, "/?"); return; } /* zu tief: ungueltig statt abgeschnitten */
        seg[n++] = t;
    }
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        o += snprintf(out + o, max - o, "/%s", seg[i]);
        if (o >= max) { snprintf(out, max, "/?"); return; }
    }
    if (o == 0) { out[0] = '/'; out[1] = 0; }
    /* Laufwerksbuchstabe gross */
    if (out[1] && (out[2] == 0 || out[2] == '/')) out[1] = (char)toupper((unsigned char)out[1]);
}

/* virtuell -> nativ. Rueckgabe: 0 = Wurzel, 1 = Pfad ok, -1 = ungueltig */
static int vpath_native(const char *v, char *out, int max)
{
    if (!strcmp(v, "/")) { out[0] = 0; return 0; }
    char letter = (char)toupper((unsigned char)v[1]);
    if (!(v[2] == 0 || v[2] == '/')) return -1;
    if (!drive_present(letter)) return -1;
    if (v[2] == 0) snprintf(out, max, "%c:\\", letter);
    else {
        if (snprintf(out, max, "%c:%s", letter, v + 2) >= max) return -1; /* zu lang */
        for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    }
    return 1;
}

static int resolve_arg(sess_t *s, const char *arg, char *native, int max, char *vout)
{
    char v[1024];
    vpath_resolve(s->cwd, arg, v, sizeof(v));
    if (vout) snprintf(vout, 1024, "%s", v);
    return vpath_native(v, native, max);
}

/* ------------------------------------------------------------------ Datenverbindung */

static void close_pasv(sess_t *s)
{
    if (s->pasv >= 0) { lwip_close(s->pasv); s->pasv = -1; }
}

static int open_pasv(sess_t *s, int *port_out)
{
    close_pasv(s);
    for (int tries = 0; tries < PASV_PORT_COUNT; tries++) {
        LONG k = InterlockedIncrement(&s_pasv_next);
        int port = PASV_PORT_FIRST + (int)(k % PASV_PORT_COUNT);
        int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        /* Ports, deren letzte Datenverbindung wir selbst geschlossen haben (LIST, RETR), haengen 2 Minuten in
           TIME_WAIT; ohne SO_REUSEADDR waeren alle PASV_PORT_COUNT Ports nach so vielen Downloads belegt */
        int one = 1;
        lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = s->local.sin_addr.s_addr;
        if (lwip_bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && lwip_listen(fd, 1) == 0) {
            s->pasv = fd;
            *port_out = port;
            return 0;
        }
        lwip_close(fd);
    }
    return -1;
}

/* Sende-/Empfangs-Timeout, damit ein verschwundener Client (Kabel gezogen) die Session nicht blockiert */
static void set_timeouts(int fd)
{
    struct timeval tv = { DATA_TIMEOUT, 0 };
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Baut die Datenverbindung auf (nach 150). Rueckgabe Socket oder -1. */
static int data_connect(sess_t *s)
{
    int fd = -1;
    if (s->pasv >= 0) {
        /* Nur der eigene Client darf die Datenverbindung aufbauen; fremde Verbindungen auf dem Port werden
           verworfen und es wird weiter gewartet (insgesamt hoechstens DATA_TIMEOUT). */
        DWORD t0 = GetTickCount();
        for (;;) {
            int left = DATA_TIMEOUT * 1000 - (int)(GetTickCount() - t0);
            if (left <= 0 || wait_readable(s->pasv, left / 1000, (left % 1000) * 1000) != 1) break;
            struct sockaddr_in a;
            socklen_t al = sizeof(a);
            fd = lwip_accept(s->pasv, (struct sockaddr *)&a, &al);
            if (fd >= 0 && a.sin_addr.s_addr == s->peer.sin_addr.s_addr) break;
            if (fd >= 0) lwip_close(fd);
            fd = -1;
        }
        close_pasv(s);
    } else if (s->have_port) {
        fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && lwip_connect(fd, (struct sockaddr *)&s->port_addr, sizeof(s->port_addr)) != 0) {
            lwip_close(fd);
            fd = -1;
        }
        s->have_port = 0;
    }
    if (fd >= 0) set_timeouts(fd);
    return fd;
}

static int has_data_target(sess_t *s)
{
    return s->pasv >= 0 || s->have_port;
}

/* Prueft waehrend eines Transfers, ob ABOR kam. 1 = abbrechen */
static int check_abort(sess_t *s)
{
    if (!memchr(s->rbuf, '\n', s->rlen) && wait_readable(s->ctrl, 0, 0) != 1) return 0;
    char line[CMD_MAX];
    int r = read_line(s, line, sizeof(line), 0);
    if (r < 0) return 1;
    if (r == 1) {
        if (!_strnicmp(line, "ABOR", 4)) return 1;
        if (!_strnicmp(line, "NOOP", 4) || !_strnicmp(line, "STAT", 4)) reply(s, "200 Transfer laeuft.");
        else reply(s, "503 Transfer laeuft, Befehl ignoriert.");
    }
    return 0;
}

/* ------------------------------------------------------------------ Listen */

static const char *MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

typedef struct {
    char name[256];
    int  dir;
    uint64_t size;
    SYSTEMTIME t;
} entry_t;

static void ft_to_st(const FILETIME *ft, SYSTEMTIME *st)
{
    if (!FileTimeToSystemTime(ft, st) || st->wYear < 1980) {
        memset(st, 0, sizeof(*st));
        st->wYear = 2001; st->wMonth = 11; st->wDay = 15;
    }
}

static int format_entry(char *o, int max, const entry_t *e, int mode, int cur_year)
{
    if (mode == 0) { /* LIST */
        char when[16];
        if (e->t.wYear == cur_year) snprintf(when, sizeof(when), "%02d:%02d", e->t.wHour, e->t.wMinute);
        else snprintf(when, sizeof(when), " %4d", e->t.wYear);
        return snprintf(o, max, "%s 1 xbox xbox %13llu %s %2d %5s %s\r\n",
                        e->dir ? "drwxr-xr-x" : "-rw-r--r--", (unsigned long long)e->size,
                        MONTHS[(e->t.wMonth + 11) % 12], e->t.wDay, when, e->name);
    } else if (mode == 1) { /* NLST */
        return snprintf(o, max, "%s\r\n", e->name);
    } else { /* MLSD / MLST-Fakten */
        return snprintf(o, max, "type=%s;size=%llu;modify=%04d%02d%02d%02d%02d%02d;perm=%s; %s\r\n",
                        e->dir ? "dir" : "file", (unsigned long long)e->size,
                        e->t.wYear, e->t.wMonth, e->t.wDay, e->t.wHour, e->t.wMinute, e->t.wSecond,
                        e->dir ? "elcdmfp" : "rwdf", e->name);
    }
}

/* Sendet Verzeichnisliste. mode: 0 LIST, 1 NLST, 2 MLSD */
static void cmd_list(sess_t *s, const char *arg, int mode)
{
    /* Optionen wie "-la" ignorieren */
    while (arg && arg[0] == '-') {
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    char native[600];
    int r = resolve_arg(s, arg, native, sizeof(native), NULL);
    if (r < 0) { reply(s, "550 Pfad nicht gefunden."); close_pasv(s); return; }
    DWORD a = FILE_ATTRIBUTE_DIRECTORY;
    if (r == 1) {
        a = GetFileAttributesA(native);
        if (a == INVALID_FILE_ATTRIBUTES) {
            if (strlen(native) != 3) { reply(s, "550 Pfad nicht gefunden."); close_pasv(s); return; }
            a = FILE_ATTRIBUTE_DIRECTORY; /* Laufwerkswurzel "E:\" hat evtl. keine Attribute */
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY) && mode == 2) { reply(s, "501 Kein Verzeichnis."); close_pasv(s); return; }
    }
    if (!has_data_target(s)) { reply(s, "425 Erst PASV oder PORT senden."); return; }

    reply(s, "150 Verzeichnisliste folgt.");
    int d = data_connect(s);
    if (d < 0) { reply(s, "425 Datenverbindung fehlgeschlagen."); return; }

    SYSTEMTIME now;
    GetSystemTime(&now);
    char *out = s->buf;
    int ol = 0, err = 0;
    entry_t e;

    if (r == 0) {
        for (int i = 0; i < NUM_DRIVES; i++) {
            if (!g_drives[i].present) continue;
            memset(&e, 0, sizeof(e));
            e.name[0] = g_drives[i].letter;
            e.dir = 1;
            e.t = now;
            ol += format_entry(out + ol, XFER_BUF - ol, &e, mode, now.wYear);
        }
    } else {
        char pat[620];
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            size_t L = strlen(native);
            snprintf(pat, sizeof(pat), "%s%s*", native, (L && native[L - 1] == '\\') ? "" : "\\");
        } else {
            snprintf(pat, sizeof(pat), "%s", native);
        }
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                memset(&e, 0, sizeof(e));
                strncpy(e.name, fd.cFileName, sizeof(e.name) - 1);
                e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.size = e.dir ? 0 : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
                ft_to_st(&fd.ftLastWriteTime, &e.t);
                if (ol > XFER_BUF - 1024) {
                    if (send_all(d, out, ol) < 0) { err = 1; break; }
                    ol = 0;
                }
                ol += format_entry(out + ol, XFER_BUF - ol, &e, mode, now.wYear);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (!err && ol > 0 && send_all(d, out, ol) < 0) err = 1;
    lwip_shutdown(d, SHUT_RDWR);
    lwip_close(d);
    if (err) reply(s, "426 Verbindung abgebrochen.");
    else reply(s, "226 Liste gesendet.");
}

static void cmd_mlst(sess_t *s, const char *arg)
{
    char native[600], v[1024];
    int r = resolve_arg(s, arg, native, sizeof(native), v);
    entry_t e;
    memset(&e, 0, sizeof(e));
    GetSystemTime(&e.t);
    if (r < 0) { reply(s, "550 Nicht gefunden."); return; }
    if (r == 0) { e.dir = 1; strcpy(e.name, "/"); }
    else {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(native, GetFileExInfoStandard, &fa)) {
            /* Laufwerkswurzel hat evtl. keine Attribute */
            if (strlen(native) == 3) { e.dir = 1; }
            else { reply(s, "550 Nicht gefunden."); return; }
        } else {
            e.dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = e.dir ? 0 : (((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
            ft_to_st(&fa.ftLastWriteTime, &e.t);
        }
        snprintf(e.name, sizeof(e.name), "%s", v);
    }
    char line[1024];
    format_entry(line + 1, sizeof(line) - 1, &e, 2, 0);
    line[0] = ' ';
    char *crlf = strstr(line, "\r\n");
    if (crlf) *crlf = 0;
    reply(s, "250-Fakten folgen");
    reply(s, "%s", line);
    reply(s, "250 Ende");
}

/* ------------------------------------------------------------------ Dateien */

/*
 * Pipeline fuer Uebertragungen: Ein Hilfsthread liest bzw. schreibt die Datei, waehrend der Session-Thread
 * gleichzeitig das Netzwerk bedient. So laufen Platte und Netzwerk parallel statt abwechselnd.
 * NBUF Puffer wandern im Kreis: sem_free zaehlt leere, sem_full gefuellte Puffer.
 *
 * Wo moeglich wird ungepuffert gelesen/geschrieben (FILE_FLAG_NO_BUFFERING): die Daten gehen dann direkt
 * zwischen Platte und Puffer, ohne Umweg ueber den kleinen Datei-Cache des Kernels. Voraussetzung:
 * Startposition und Laengen sind Vielfache der Sektorgroesse, die Puffer sind ausgerichtet.
 */
#define SECTOR 512

typedef struct {
    HANDLE  h;
    char   *buf[NBUF];
    DWORD   len[NBUF];
    int     state[NBUF];     /* Lesen: 0 Daten, 1 Dateiende, 2 Fehler */
    HANDLE  sem_free, sem_full;
    volatile LONG stop;      /* Session bricht ab */
    volatile LONG werr;      /* Schreibfehler */
    int     unbuffered;
} io_pipe_t;

static int pipe_init(io_pipe_t *io, sess_t *s, HANDLE h, int unbuffered)
{
    memset(io, 0, sizeof(*io));
    io->h = h;
    io->unbuffered = unbuffered;
    for (int i = 0; i < NBUF; i++) io->buf[i] = s->bufs[i];
    io->sem_free = CreateSemaphore(NULL, NBUF, NBUF * 2, NULL);
    io->sem_full = CreateSemaphore(NULL, 0, NBUF * 2, NULL);
    if (!io->sem_free || !io->sem_full) {
        if (io->sem_free) CloseHandle(io->sem_free);
        if (io->sem_full) CloseHandle(io->sem_full);
        io->sem_free = io->sem_full = NULL;
        return -1;
    }
    return 0;
}

static void pipe_close(io_pipe_t *io, HANDLE thread)
{
    if (thread) {
        InterlockedExchange(&io->stop, 1);
        ReleaseSemaphore(io->sem_free, NBUF, NULL);   /* Hilfsthread aufwecken, falls er auf Platz wartet */
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    CloseHandle(io->sem_free);
    CloseHandle(io->sem_full);
}

static DWORD WINAPI reader_thread(LPVOID p)
{
    io_pipe_t *io = (io_pipe_t *)p;
    for (int i = 0;; i = (i + 1) % NBUF) {
        WaitForSingleObject(io->sem_free, INFINITE);
        if (io->stop) break;
        DWORD got = 0;
        BOOL ok = ReadFile(io->h, io->buf[i], XFER_BUF, &got, NULL);
        io->len[i] = got;
        io->state[i] = !ok ? 2 : (got == 0 ? 1 : 0);
        ReleaseSemaphore(io->sem_full, 1, NULL);
        if (io->state[i]) break;
    }
    return 0;
}

static DWORD WINAPI writer_thread(LPVOID p)
{
    io_pipe_t *io = (io_pipe_t *)p;
    for (int i = 0;; i = (i + 1) % NBUF) {
        WaitForSingleObject(io->sem_full, INFINITE);
        DWORD n = io->len[i];
        if (n == 0) break;                            /* Ende-Markierung */
        if (!io->werr) {
            DWORD wlen = n;
            if (io->unbuffered && (n % SECTOR)) {     /* nur der letzte Block: auf Sektor auffuellen, wird danach gekuerzt */
                wlen = (n + SECTOR - 1) / SECTOR * SECTOR;
                memset(io->buf[i] + n, 0, wlen - n);
            }
            DWORD wr = 0;
            if (!WriteFile(io->h, io->buf[i], wlen, &wr, NULL) || wr != wlen) InterlockedExchange(&io->werr, 1);
        }
        ReleaseSemaphore(io->sem_free, 1, NULL);
    }
    return 0;
}

static void cmd_retr(sess_t *s, const char *arg)
{
    char native[600];
    if (resolve_arg(s, arg, native, sizeof(native), NULL) != 1) { reply(s, "550 Datei nicht gefunden."); close_pasv(s); return; }
    uint64_t pos = s->rest;
    s->rest = 0;
    if (!has_data_target(s)) { reply(s, "425 Erst PASV oder PORT senden."); return; }
    int unbuffered = (pos % SECTOR) == 0;
    HANDLE h = CreateFileA(native, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE && unbuffered) {   /* Rueckfall auf gepufferten Zugriff */
        unbuffered = 0;
        h = CreateFileA(native, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) { reply(s, "550 Datei kann nicht geoeffnet werden."); close_pasv(s); return; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) sz.QuadPart = 0;
    if (pos) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)pos;
        if ((LONGLONG)pos > sz.QuadPart || !SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            reply(s, "554 Ungueltige REST-Position.");
            close_pasv(s);
            return;
        }
    }
    reply(s, "150 Sende %s (%llu Bytes).", arg, (unsigned long long)sz.QuadPart);
    int d = data_connect(s);
    if (d < 0) { CloseHandle(h); reply(s, "425 Datenverbindung fehlgeschlagen."); return; }

    io_pipe_t io;
    HANDLE th = NULL;
    int result = 0; /* 0 ok, 1 Netz, 2 Datei, 3 Abbruch */
    if (pipe_init(&io, s, h, unbuffered) < 0 || !(th = CreateThread(NULL, 16 * 1024, reader_thread, &io, 0, NULL))) {
        if (th == NULL && io.sem_free) { CloseHandle(io.sem_free); CloseHandle(io.sem_full); }
        CloseHandle(h);
        lwip_close(d);
        reply(s, "451 Interner Fehler.");
        return;
    }
    set_xfer(s, XFER_SEND, native, pos, (uint64_t)sz.QuadPart);
    for (int i = 0;; i = (i + 1) % NBUF) {
        WaitForSingleObject(io.sem_full, INFINITE);
        if (io.state[i] == 2) { result = 2; break; }
        if (io.state[i] == 1) break;
        DWORD got = io.len[i];
        if (send_all(d, io.buf[i], (int)got) < 0) { result = 1; break; }
        ReleaseSemaphore(io.sem_free, 1, NULL);
        pos += got;
        stat_add(XFER_SEND, got);
        upd_xfer(s, pos);
        if (check_abort(s)) { result = 3; break; }
    }
    pipe_close(&io, th);
    end_xfer(s);
    CloseHandle(h);
    if (result == 0) lwip_shutdown(d, SHUT_WR);
    lwip_close(d);
    switch (result) {
    case 0: reply(s, "226 Uebertragung abgeschlossen."); break;
    case 1: reply(s, "426 Verbindung abgebrochen."); break;
    case 2: reply(s, "451 Lesefehler."); break;
    default: reply(s, "426 Uebertragung abgebrochen."); reply(s, "226 ABOR ausgefuehrt."); break;
    }
}

static void cmd_stor(sess_t *s, const char *arg, int append)
{
    char native[600];
    if (resolve_arg(s, arg, native, sizeof(native), NULL) != 1) { reply(s, "553 Ungueltiger Pfad."); close_pasv(s); return; }
    if (!has_data_target(s)) { s->rest = 0; reply(s, "425 Erst PASV oder PORT senden."); return; }
    uint64_t pos = s->rest;
    s->rest = 0;
    DWORD disp = (append || pos) ? OPEN_ALWAYS : CREATE_ALWAYS;
    /* Zuerst immer gepuffert oeffnen: Kleine Dateien (bis XFER_BUF) werden so mit einem einzigen WriteFile
       gespeichert, ohne Schreib-Thread und ohne nachtraegliches Kuerzen des aufgefuellten letzten Sektors.
       Erst wenn mehr als ein Puffer ankommt, wird fuer die Pipeline ungepuffert neu geoeffnet. */
    HANDLE h = CreateFileA(native, GENERIC_WRITE, 0, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { reply(s, "553 Datei kann nicht angelegt werden."); close_pasv(s); return; }
    LARGE_INTEGER li;
    BOOL seek_ok = TRUE;
    if (append) {
        li.QuadPart = 0;
        LARGE_INTEGER np;
        seek_ok = SetFilePointerEx(h, li, &np, FILE_END);
        pos = (uint64_t)np.QuadPart;
    } else if (pos) {
        li.QuadPart = (LONGLONG)pos;   /* Datei ab pos abschneiden */
        seek_ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && SetEndOfFile(h);
    }
    if (!seek_ok) {
        CloseHandle(h);
        reply(s, "554 Ungueltige REST-Position.");
        close_pasv(s);
        return;
    }
    const uint64_t start = pos;
    reply(s, "150 Bereit fuer %s.", arg);
    int d = data_connect(s);
    if (d < 0) { CloseHandle(h); reply(s, "425 Datenverbindung fehlgeschlagen."); return; }
    set_xfer(s, XFER_RECV, native, pos, 0);

    int result = 0; /* 0 ok, 1 Netz, 2 Datei, 3 Abbruch */
    int net_errno = 0;
    int unbuffered = 0;
    int fill = 0, end = 0;
    /* ersten Puffer fuellen (SO_RCVTIMEO = DATA_TIMEOUT) */
    while (fill < XFER_BUF) {
        int n = lwip_recv(d, s->bufs[0] + fill, XFER_BUF - fill, 0);
        if (n < 0) { net_errno = errno; result = 1; end = 1; break; }
        if (n == 0) { end = 1; break; }
        fill += n;
    }
    pos += fill;
    if (fill > 0) { stat_add(XFER_RECV, (uint32_t)fill); upd_xfer(s, pos); }

    if (end) {
        /* Datei passt in einen Puffer: direkt gepuffert schreiben */
        DWORD wr = 0;
        if (fill > 0 && (!WriteFile(h, s->bufs[0], (DWORD)fill, &wr, NULL) || wr != (DWORD)fill) && result != 1) result = 2;
        CloseHandle(h);
    } else if (check_abort(s)) {
        DWORD wr = 0;
        WriteFile(h, s->bufs[0], (DWORD)fill, &wr, NULL);
        CloseHandle(h);
        result = 3;
    } else {
        /* grosse Datei: wenn moeglich ungepuffert neu oeffnen, dann Pipeline mit vorgefuelltem Puffer 0 */
        if (!append && (start % SECTOR) == 0) {
            CloseHandle(h);
            h = CreateFileA(native, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, NULL);
            unbuffered = h != INVALID_HANDLE_VALUE;
            if (!unbuffered) h = CreateFileA(native, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            li.QuadPart = (LONGLONG)start;
            if (h == INVALID_HANDLE_VALUE || !SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
                result = 2;
            }
        }
        io_pipe_t io;
        HANDLE th = NULL;
        int internal = 0;
        if (result == 0 && (pipe_init(&io, s, h, unbuffered) < 0 ||
                            !(th = CreateThread(NULL, 16 * 1024, writer_thread, &io, 0, NULL)))) {
            if (io.sem_free) { CloseHandle(io.sem_free); CloseHandle(io.sem_full); }
            internal = 1;
        }
        if (result != 0 || internal) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            end_xfer(s);
            lwip_close(d);
            reply(s, internal ? "451 Interner Fehler." : "451 Datei kann nicht geoeffnet werden.");
            return;
        }
        WaitForSingleObject(io.sem_free, INFINITE);   /* Puffer 0 ist schon gefuellt */
        io.len[0] = (DWORD)fill;
        ReleaseSemaphore(io.sem_full, 1, NULL);
        int i = 1;
        for (;;) {
            WaitForSingleObject(io.sem_free, INFINITE);
            if (io.werr) { result = 2; ReleaseSemaphore(io.sem_free, 1, NULL); break; }
            /* Puffer i fuellen, bis er voll ist oder die Verbindung endet */
            fill = 0; end = 0;
            while (fill < XFER_BUF) {
                int n = lwip_recv(d, io.buf[i] + fill, XFER_BUF - fill, 0);
                if (n < 0) { net_errno = errno; result = 1; end = 1; break; }
                if (n == 0) { end = 1; break; }
                fill += n;
            }
            if (fill > 0) {
                io.len[i] = (DWORD)fill;
                ReleaseSemaphore(io.sem_full, 1, NULL);   /* an den Schreib-Thread */
                i = (i + 1) % NBUF;
                pos += fill;
                stat_add(XFER_RECV, (uint32_t)fill);
                upd_xfer(s, pos);
            } else {
                ReleaseSemaphore(io.sem_free, 1, NULL);   /* Platz unbenutzt zurueckgeben */
            }
            if (end) break;
            if (check_abort(s)) { result = 3; break; }
        }
        /* Ende-Markierung senden und warten, bis alles geschrieben ist */
        WaitForSingleObject(io.sem_free, INFINITE);
        io.len[i] = 0;
        ReleaseSemaphore(io.sem_full, 1, NULL);
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        CloseHandle(io.sem_free);
        CloseHandle(io.sem_full);
        if (io.werr && result != 1) result = 2;
        CloseHandle(h);
        if (unbuffered && (pos % SECTOR)) {
            /* aufgefuellten letzten Sektor wieder abschneiden; FATX erlaubt das nur ueber ein gepuffertes Handle */
            HANDLE h2 = CreateFileA(native, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            li.QuadPart = (LONGLONG)pos;
            if (h2 == INVALID_HANDLE_VALUE || !SetFilePointerEx(h2, li, NULL, FILE_BEGIN) || !SetEndOfFile(h2)) result = 2;
            if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
        }
    }
    end_xfer(s);
    lwip_close(d);
    InterlockedExchange(&g_drives_dirty, 1);
    switch (result) {
    case 0: reply(s, "226 Datei gespeichert."); break;
    case 1: reply(s, "426 Verbindung abgebrochen (Fehler %d).", net_errno); break;
    case 2: reply(s, "452 Schreibfehler (Speicher voll?)."); break;
    default: reply(s, "426 Uebertragung abgebrochen."); reply(s, "226 ABOR ausgefuehrt."); break;
    }
}

/* ------------------------------------------------------------------ Befehle */

static void cmd_pasv(sess_t *s, int extended)
{
    int port;
    s->have_port = 0;
    if (open_pasv(s, &port) < 0) { reply(s, "425 Kein freier Port."); return; }
    if (extended) {
        reply(s, "229 Passiver Modus (|||%d|)", port);
    } else {
        uint32_t ip = ntohl(s->local.sin_addr.s_addr);
        reply(s, "227 Passiver Modus (%u,%u,%u,%u,%d,%d)",
              (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255, (unsigned)(ip >> 8) & 255, (unsigned)ip & 255,
              port >> 8, port & 255);
    }
}

static void cmd_port(sess_t *s, const char *arg)
{
    unsigned a[6];
    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) {
        reply(s, "501 Syntaxfehler.");
        return;
    }
    close_pasv(s);
    memset(&s->port_addr, 0, sizeof(s->port_addr));
    s->port_addr.sin_family = AF_INET;
    s->port_addr.sin_addr.s_addr = htonl((a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3]);
    s->port_addr.sin_port = htons((a[4] << 8) | a[5]);
    if (s->port_addr.sin_addr.s_addr != s->peer.sin_addr.s_addr || a[4] > 255 || a[5] > 255) {
        reply(s, "504 Nur Ports beim eigenen Client erlaubt.");
        return;
    }
    s->have_port = 1;
    reply(s, "200 PORT ok.");
}

static void cmd_eprt(sess_t *s, const char *arg)
{
    /* |1|132.235.1.2|6275| */
    char d = arg[0];
    char ip[64];
    unsigned proto, port;
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%c%%u%c%%63[^%c]%c%%u%c", d, d, d, d, d);
    if (!d || sscanf(arg, fmt, &proto, ip, &port) != 3 || proto != 1) { reply(s, "522 Nur IPv4 (1)."); return; }
    close_pasv(s);
    memset(&s->port_addr, 0, sizeof(s->port_addr));
    s->port_addr.sin_family = AF_INET;
    s->port_addr.sin_addr.s_addr = inet_addr(ip);
    s->port_addr.sin_port = htons(port);
    if (s->port_addr.sin_addr.s_addr != s->peer.sin_addr.s_addr || port > 65535) {
        reply(s, "504 Nur Ports beim eigenen Client erlaubt.");
        return;
    }
    s->have_port = 1;
    reply(s, "200 EPRT ok.");
}

static int is_dir(const char *native)
{
    DWORD a = GetFileAttributesA(native);
    if (a == INVALID_FILE_ATTRIBUTES) return strlen(native) == 3; /* "E:\" */
    return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void cmd_cwd(sess_t *s, const char *arg)
{
    char native[600], v[1024];
    int r = resolve_arg(s, arg, native, sizeof(native), v);
    if (r < 0 || (r == 1 && !is_dir(native)) || strlen(v) >= sizeof(s->cwd)) { reply(s, "550 Verzeichnis nicht gefunden."); return; }
    memcpy(s->cwd, v, strlen(v) + 1);
    reply(s, "250 Verzeichnis ist jetzt \"%s\".", s->cwd);
}

static void pwd_quoted(const char *p, char *out, int max)
{
    /* Anfuehrungszeichen im Pfad verdoppeln (RFC 959) */
    int o = 0;
    for (; *p && o < max - 3; p++) {
        if (*p == '"') out[o++] = '"';
        out[o++] = *p;
    }
    out[o] = 0;
}

static void handle(sess_t *s, char *line)
{
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    char *cmd = line;
    for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

    /* ohne Anmeldung erlaubt */
    if (!strcmp(cmd, "USER")) {
        snprintf(s->user, sizeof(s->user), "%s", arg);
        s->logged_in = 0;
        EnterCriticalSection(&g_lock);
        g_sess[s->slot].logged_in = 0;
        LeaveCriticalSection(&g_lock);
        reply(s, "331 Passwort bitte.");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        if (!_stricmp(s->user, FTP_USER) && !strcmp(arg, FTP_PASS)) {
            s->logged_in = 1;
            EnterCriticalSection(&g_lock);
            g_sess[s->slot].logged_in = 1;
            LeaveCriticalSection(&g_lock);
            reply(s, "230 Angemeldet.");
        } else {
            Sleep(1000);
            reply(s, "530 Anmeldung fehlgeschlagen.");
        }
        return;
    }
    if (!strcmp(cmd, "QUIT")) { reply(s, "221 Tschuess."); s->quit = 1; return; }
    if (!strcmp(cmd, "NOOP")) { reply(s, "200 OK."); return; }
    if (!strcmp(cmd, "SYST")) { reply(s, "215 UNIX Type: L8"); return; }
    if (!strcmp(cmd, "FEAT")) {
        reply(s, "211-Features:");
        reply(s, " MDTM");
        reply(s, " SIZE");
        reply(s, " REST STREAM");
        reply(s, " MLST type*;size*;modify*;perm*;");
        reply(s, " EPSV");
        reply(s, " EPRT");
        reply(s, " UTF8");
        reply(s, "211 Ende");
        return;
    }
    if (!strcmp(cmd, "OPTS")) { reply(s, "200 OK."); return; }
    if (!strcmp(cmd, "CLNT")) { reply(s, "200 OK."); return; }
    if (!strcmp(cmd, "HELP")) { reply(s, "214 Xbox FTP-Server."); return; }

    if (!s->logged_in) { reply(s, "530 Bitte anmelden."); return; }

    /* REST gilt nur fuer den direkt folgenden Transfer, RNFR nur fuer das direkt folgende RNTO */
    int keep_rest = !strcmp(cmd, "REST") || !strcmp(cmd, "RETR") || !strcmp(cmd, "STOR") || !strcmp(cmd, "APPE") ||
                    !strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV") || !strcmp(cmd, "PORT") || !strcmp(cmd, "EPRT") ||
                    !strcmp(cmd, "TYPE") || !strcmp(cmd, "NOOP");
    if (!keep_rest) s->rest = 0;
    if (strcmp(cmd, "RNTO") != 0 && strcmp(cmd, "RNFR") != 0) s->rnfr[0] = 0;

    if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
        char q[1100];
        pwd_quoted(s->cwd, q, sizeof(q));
        reply(s, "257 \"%s\" ist das aktuelle Verzeichnis.", q);
    } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD")) {
        cmd_cwd(s, arg);
    } else if (!strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
        cmd_cwd(s, "..");
    } else if (!strcmp(cmd, "TYPE")) {
        reply(s, "200 Typ gesetzt.");
    } else if (!strcmp(cmd, "MODE")) {
        reply(s, toupper((unsigned char)arg[0]) == 'S' ? "200 OK." : "504 Nur Stream-Modus.");
    } else if (!strcmp(cmd, "STRU")) {
        reply(s, toupper((unsigned char)arg[0]) == 'F' ? "200 OK." : "504 Nur Dateistruktur.");
    } else if (!strcmp(cmd, "PASV")) {
        cmd_pasv(s, 0);
    } else if (!strcmp(cmd, "EPSV")) {
        if (!_stricmp(arg, "ALL")) reply(s, "200 OK.");
        else cmd_pasv(s, 1);
    } else if (!strcmp(cmd, "PORT")) {
        cmd_port(s, arg);
    } else if (!strcmp(cmd, "EPRT")) {
        cmd_eprt(s, arg);
    } else if (!strcmp(cmd, "LIST")) {
        cmd_list(s, arg, 0);
    } else if (!strcmp(cmd, "NLST")) {
        cmd_list(s, arg, 1);
    } else if (!strcmp(cmd, "MLSD")) {
        cmd_list(s, arg, 2);
    } else if (!strcmp(cmd, "MLST")) {
        cmd_mlst(s, arg);
    } else if (!strcmp(cmd, "REST")) {
        s->rest = strtoull(arg, NULL, 10);
        reply(s, "350 Fortsetzen ab %llu.", (unsigned long long)s->rest);
    } else if (!strcmp(cmd, "RETR")) {
        cmd_retr(s, arg);
    } else if (!strcmp(cmd, "STOR")) {
        cmd_stor(s, arg, 0);
    } else if (!strcmp(cmd, "APPE")) {
        cmd_stor(s, arg, 1);
    } else if (!strcmp(cmd, "ALLO")) {
        reply(s, "202 Nicht noetig.");
    } else if (!strcmp(cmd, "DELE")) {
        char n[600];
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && DeleteFileA(n)) {
            InterlockedExchange(&g_drives_dirty, 1);
            reply(s, "250 Geloescht.");
        } else reply(s, "550 Loeschen fehlgeschlagen.");
    } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
        char n[600], v[1024];
        if (resolve_arg(s, arg, n, sizeof(n), v) == 1 && CreateDirectoryA(n, NULL)) {
            char q[1100];
            pwd_quoted(v, q, sizeof(q));
            reply(s, "257 \"%s\" angelegt.", q);
        } else reply(s, "550 Anlegen fehlgeschlagen.");
    } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
        char n[600];
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && strlen(n) > 3 && RemoveDirectoryA(n)) {
            InterlockedExchange(&g_drives_dirty, 1);
            reply(s, "250 Verzeichnis entfernt.");
        } else reply(s, "550 Entfernen fehlgeschlagen (nicht leer?).");
    } else if (!strcmp(cmd, "RNFR")) {
        char n[600];
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && strlen(n) > 3 &&
            GetFileAttributesA(n) != INVALID_FILE_ATTRIBUTES) {
            snprintf(s->rnfr, sizeof(s->rnfr), "%s", n);
            reply(s, "350 Zielname bitte.");
        } else reply(s, "550 Nicht gefunden.");
    } else if (!strcmp(cmd, "RNTO")) {
        char n[600];
        if (!s->rnfr[0]) { reply(s, "503 Erst RNFR."); return; }
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && MoveFileA(s->rnfr, n)) reply(s, "250 Umbenannt.");
        else reply(s, "550 Umbenennen fehlgeschlagen.");
        s->rnfr[0] = 0;
    } else if (!strcmp(cmd, "SIZE")) {
        char n[600];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && GetFileAttributesExA(n, GetFileExInfoStandard, &fa) &&
            !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            reply(s, "213 %llu", (unsigned long long)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow));
        else reply(s, "550 Nicht gefunden.");
    } else if (!strcmp(cmd, "MDTM")) {
        char n[600];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (resolve_arg(s, arg, n, sizeof(n), NULL) == 1 && GetFileAttributesExA(n, GetFileExInfoStandard, &fa)) {
            SYSTEMTIME t;
            ft_to_st(&fa.ftLastWriteTime, &t);
            reply(s, "213 %04d%02d%02d%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        } else reply(s, "550 Nicht gefunden.");
    } else if (!strcmp(cmd, "ABOR")) {
        close_pasv(s);
        reply(s, "226 Kein Transfer aktiv.");
    } else if (!strcmp(cmd, "STAT")) {
        reply(s, "211 Xbox FTP-Server, angemeldet als %s.", s->user);
    } else {
        reply(s, "502 Befehl unbekannt.");
    }
}

/* ------------------------------------------------------------------ Threads */

static DWORD WINAPI session_thread(LPVOID p)
{
    sess_t *s = (sess_t *)p;
    char line[CMD_MAX];

    reply(s, "220 Xbox FTP-Server bereit.");
    for (;;) {
        int r = read_line(s, line, sizeof(line), IDLE_TIMEOUT);
        if (r == 0) { reply(s, "421 Zeitueberschreitung."); break; }
        if (r < 0) break;
        if (!line[0]) continue;
        handle(s, line);
        if (s->quit) break;
    }
    close_pasv(s);
    lwip_close(s->ctrl);
    EnterCriticalSection(&g_lock);
    memset(&g_sess[s->slot], 0, sizeof(g_sess[0]));
    LeaveCriticalSection(&g_lock);
    free(s->bufmem);
    free(s);
    return 0;
}

static DWORD WINAPI listen_thread(LPVOID unused)
{
    (void)unused;
    int ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(FTP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls < 0 || lwip_bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || lwip_listen(ls, MAX_SESSIONS) < 0) {
        InterlockedExchange(&g_server_state, -1);
        return 1;
    }
    InterlockedExchange(&g_server_state, 1);

    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int c = lwip_accept(ls, (struct sockaddr *)&ca, &cl);
        if (c < 0) { Sleep(100); continue; }

        int slot = -1;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!g_sess[i].used) {
                slot = i;
                memset(&g_sess[i], 0, sizeof(g_sess[i]));
                g_sess[i].used = 1;
                snprintf(g_sess[i].ip, sizeof(g_sess[i].ip), "%s", inet_ntoa(ca.sin_addr));
                break;
            }
        }
        LeaveCriticalSection(&g_lock);
        if (slot < 0) {
            static const char busy[] = "421 Zu viele Verbindungen.\r\n";
            lwip_send(c, busy, sizeof(busy) - 1, 0);
            lwip_close(c);
            continue;
        }

        sess_t *s = calloc(1, sizeof(sess_t));
        char *buf = malloc((size_t)XFER_BUF * NBUF + 4096);
        if (!s || !buf) {
            free(s); free(buf);
            lwip_close(c);
            EnterCriticalSection(&g_lock);
            g_sess[slot].used = 0;
            LeaveCriticalSection(&g_lock);
            continue;
        }
        s->slot = slot;
        s->ctrl = c;
        s->pasv = -1;
        s->bufmem = buf;
        {
            char *al = (char *)(((uintptr_t)buf + 4095) & ~(uintptr_t)4095);   /* fuer ungepufferte I/O */
            for (int k = 0; k < NBUF; k++) s->bufs[k] = al + (size_t)k * XFER_BUF;
            s->buf = s->bufs[0];
        }
        s->peer = ca;
        strcpy(s->cwd, "/");
        socklen_t ll = sizeof(s->local);
        lwip_getsockname(c, (struct sockaddr *)&s->local, &ll);

        int one = 1;
        lwip_setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval stv = { DATA_TIMEOUT, 0 };
        lwip_setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

        HANDLE t = CreateThread(NULL, 64 * 1024, session_thread, s, 0, NULL);
        if (!t) {
            lwip_close(c);
            free(buf); free(s);
            EnterCriticalSection(&g_lock);
            g_sess[slot].used = 0;
            LeaveCriticalSection(&g_lock);
        } else {
            CloseHandle(t);
        }
    }
    return 0;
}

void ftpd_start(void)
{
    HANDLE t = CreateThread(NULL, 32 * 1024, listen_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_server_state, -1);
}
