/*
 * Gemeinsamer Zustand zwischen FTP-Server-Threads und Oberflaeche.
 * Alle Felder ausser den reinen Zaehlern werden unter g_lock geaendert/gelesen.
 */
#pragma once
#include <windows.h>
#include <stdint.h>

#define FTP_PORT        21
#define FTP_USER        "xbox"
#define FTP_PASS        "xbox"
#define PASV_PORT_FIRST 50000
#define PASV_PORT_COUNT 100
#define MAX_SESSIONS    8

enum { XFER_NONE = 0, XFER_SEND = 1 /* Xbox -> PC (RETR) */, XFER_RECV = 2 /* PC -> Xbox (STOR) */ };

typedef struct {
    int      used;
    int      logged_in;
    char     ip[16];
    int      xfer;            /* XFER_* */
    char     file[96];        /* Dateiname ohne Pfad */
    uint64_t done;            /* bereits uebertragen (inkl. REST-Offset) */
    uint64_t total;           /* Dateigroesse bei RETR, 0 = unbekannt */
} session_info_t;

typedef struct {
    char     letter;
    int      present;
    uint64_t total;
    uint64_t free;
} drive_info_t;

#define NUM_DRIVES 7
extern drive_info_t     g_drives[NUM_DRIVES];

extern session_info_t   g_sess[MAX_SESSIONS];
extern CRITICAL_SECTION g_lock;
extern volatile uint64_t g_bytes_sent;  /* gesamt Xbox -> PC */
extern volatile uint64_t g_bytes_recv;  /* gesamt PC -> Xbox */
extern volatile LONG     g_drives_dirty; /* Speicher neu einlesen */
extern volatile LONG     g_server_state; /* 0 = startet, 1 = laeuft, -1 = Fehler */

void ftpd_start(void);   /* startet den Listener-Thread */
int  drive_present(char letter);
