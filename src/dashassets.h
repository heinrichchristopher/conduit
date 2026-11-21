/*
 * Optionale Assets des Original-Dashboards, zur Laufzeit von C: gelesen.
 * Nichts davon ist Teil dieses Programms oder wird weitergegeben; fehlt es, greift der Ersatz.
 */
#pragma once
#include <stdint.h>

/*
 * Laedt das Zellmuster des Dashboard-Tunnels ("cellwall.xbx" aus default.xip in C:\xboxdashdata.*).
 * Rueckgabe: malloc'ter Puffer mit w*h Bytes (Linienstaerke 0..255), vom Aufrufer freizugeben;
 * NULL, wenn das Dashboard fehlt oder die Datei nicht wie erwartet aussieht.
 */
uint8_t *dash_load_cells(int *w, int *h);
