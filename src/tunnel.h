/* Hintergrund: Blick in eine Roehre mit Zellmuster, wie im Dashboard. */
#pragma once
#include <stdint.h>

/*
 * Rendert den Tunnel nach px (w*h Pixel, 0xAARRGGBB, Zeile fuer Zeile).
 * cells: Linienstaerke 0..255 als tw*th Bytes (kachelbar). rep: Kacheldichte (1.0 = eigenes Muster).
 * Entspricht tunnel_reference() in tools/make_assets.py.
 */
void tunnel_render(uint32_t *px, int w, int h, const uint8_t *cells, int tw, int th, double rep);
