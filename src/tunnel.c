/*
 * Tunnel-Renderer. Gleiche Szene wie tunnel_reference() in tools/make_assets.py (dort 4 Strahlen je Pixel),
 * bei Aenderungen an Kamera/Farben bitte beide anpassen.
 *
 * Kamera sitzt in einer Roehre (Radius 1) und blickt entlang +z, leicht gedreht; jeder Pixelmittelpunkt schneidet
 * den Strahl mit dem Zylinder, daraus ergeben sich Winkel und Laenge als Texturkoordinaten, die Entfernung
 * als Nebel. Die teuren Funktionen (atan2, sqrt, exp) laufen nur einmal je Pixel; zur Glaettung wird die
 * Textur an vier Stellen im Pixel abgetastet, deren Lage aus den Nachbarpixeln folgt.
 * Die Kantenlaenge der Textur muss eine Zweierpotenz sein (Dashboard-Texturen und eigenes Muster: 256).
 *
 * Helligkeit: an Screenshots des Original-Dashboards angeglichen (Linien gedaempft, Grundton und Leuchten
 * zurueckgenommen); vorher wirkte der Hintergrund deutlich kraeftiger als im Original.
 */
#include <math.h>
#include <stdlib.h>
#include "gen_layout.h"
#include "tunnel.h"

#define PI_ 3.14159265358979323846
#define REP_U 28.0   /* Kacheln rundherum; REP_U * rep muss ganzzahlig sein, sonst faellt der Kachelsprung beim Winkel auf */
#define REP_V 2.8

static double rep_u, rep_v;

typedef struct { float u, v, fog; } geom_t;

static struct {
    double f, cyw, syw, cp, sp;
} cam;

/* Geometrie fuer Bildpunkt (x, y) in Pixeleinheiten (Pixelmitte = +0.5) */
static geom_t geometry(double x, double y, int W, int H)
{
    const double camx = 0.35, camy = -0.15, camz = 0.0;
    double dx = (x - W / 2.0) / cam.f;
    double dy = -(y - H / 2.0) / cam.f;
    double dz = 1.0, t0;
    t0 = dy * cam.cp - dz * cam.sp; dz = dy * cam.sp + dz * cam.cp; dy = t0;       /* Neigung */
    t0 = dx * cam.cyw + dz * cam.syw; dz = -dx * cam.syw + dz * cam.cyw; dx = t0;  /* Drehung */
    double a = dx * dx + dy * dy;
    double b = 2 * (camx * dx + camy * dy);
    double c = camx * camx + camy * camy - 1;
    double t = a > 1e-9 ? (-b + sqrt(b * b - 4 * a * c)) / (2 * a) : 50.0;
    double hx = camx + t * dx, hy = camy + t * dy, hz = camz + t * dz;
    geom_t g;
    g.u = (float)(atan2(hy, hx) / (2 * PI_) * rep_u);
    g.v = (float)(hz * rep_v);
    g.fog = (float)exp(-0.11 * t);
    return g;
}

/* Mittelwert von (Linienstaerke)^3: bestimmt, wie hell das Muster im Tunnel wirkt */
static double mean_cubed(const uint8_t *tex, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) { double v = tex[i] / 255.0; s += v * v * v; }
    return s / n;
}

void tunnel_render(uint32_t *out, int W, int H, const uint8_t *tex, int tw, int th, double rep)
{
    rep_u = REP_U * rep;
    rep_v = REP_V * rep;

    /* Helligkeit teilweise an die Linienflaeche des Referenzmusters angleichen (Dashboard-Textur ist dichter) */
    double m3 = mean_cubed(tex, tw * th);
    double gain = m3 > 1e-6 ? sqrt(sqrt(CELLS_REF_MEAN3 / m3)) : 1.0;
    if (gain < 0.5) gain = 0.5;
    if (gain > 1.5) gain = 1.5;

    static float cube[256];
    for (int i = 0; i < 256; i++) { double v = i / 255.0; cube[i] = (float)(v * v * v); }

    const double fov = 70.0 * PI_ / 180.0, yaw = 8.0 * PI_ / 180.0, pitch = -10.0 * PI_ / 180.0;
    cam.f = (W / 2.0) / tan(fov / 2.0);
    cam.cyw = cos(yaw); cam.syw = sin(yaw); cam.cp = cos(pitch); cam.sp = sin(pitch);

    /* exp(-5 (rx^2 + ry^2)) = exp(-5 rx^2) * exp(-5 ry^2): nur W + H statt W * H Aufrufe */
    float *gcol = malloc(sizeof(float) * W);
    if (!gcol) return;
    for (int x = 0; x < W; x++) { double rx = (x - W * 0.55) / W; gcol[x] = (float)exp(-5.0 * rx * rx); }

    geom_t *row[2] = { malloc(sizeof(geom_t) * (W + 1)), malloc(sizeof(geom_t) * (W + 1)) };
    if (!row[0] || !row[1]) { free(row[0]); free(row[1]); free(gcol); return; }
    for (int x = 0; x <= W; x++) row[0][x] = geometry(x + 0.5, 0.5, W, H);

    for (int py = 0; py < H; py++) {
        geom_t *cur = row[py & 1], *nxt = row[(py + 1) & 1];
        double ry = (py - H * 0.42) / H, grow = exp(-5.0 * ry * ry);
        for (int x = 0; x <= W; x++) nxt[x] = geometry(x + 0.5, py + 1.5, W, H);

        for (int px = 0; px < W; px++) {
            geom_t g = cur[px];
            /* Aenderung der Texturkoordinaten pro Pixel nach rechts / unten (Winkel zyklisch) */
            double dux = cur[px + 1].u - g.u, duy = nxt[px].u - g.u;
            double dvx = cur[px + 1].v - g.v, dvy = nxt[px].v - g.v;
            if (dux > rep_u / 2) dux -= rep_u; else if (dux < -rep_u / 2) dux += rep_u;
            if (duy > rep_u / 2) duy -= rep_u; else if (duy < -rep_u / 2) duy += rep_u;

            double acc = 0;
            for (int s = 0; s < 4; s++) {
                double sx = (s & 1) ? 0.25 : -0.25, sy = (s & 2) ? 0.25 : -0.25;
                double u = g.u + sx * dux + sy * duy, v = g.v + sx * dvx + sy * dvy;
                /* +1024 (ganzzahlig, Kachelperiode 1) macht die Werte positiv; die Maske haelt den Index in jedem
                   Fall im Bereich (Kantenlaenge ist eine Zweierpotenz), ganz ohne floor() */
                int iu = (int)((u + 1024.0) * tw) & (tw - 1), iv = (int)((v + 1024.0) * th) & (th - 1);
                acc += cube[tex[iv * tw + iu]];
            }
            acc = acc / 4 * g.fog * gain;

            double glow = gcol[px] * grow;
            double r = 4 + 13 * glow + 26 * acc;
            double gr = 18 + 36 * glow + 64 * acc;
            double bl = 2 + 6 * glow + 10 * acc;
            if (r > 255) r = 255;
            if (gr > 255) gr = 255;
            if (bl > 255) bl = 255;

            /* Vignette: nur die Ecken werden leicht abgedunkelt */
            double vx = (px - W / 2.0) / (0.75 * W), vy = (py - H / 2.0) / (0.8 * H), d2 = vx * vx + vy * vy;
            double m = 1.0;
            if (d2 > 0.72 * 0.72) {            /* innerhalb von 0.72 ist m = 1, dort entfaellt die Wurzel */
                m = (1.12 - sqrt(d2)) / 0.4;
                m = m < 0 ? 0 : m;
            }
            out[py * W + px] = 0xFF000000u | ((uint32_t)(r * m) << 16) | ((uint32_t)(8 + (gr - 8) * m) << 8) | (uint32_t)(bl * m);
        }
    }
    free(row[0]);
    free(row[1]);
    free(gcol);
}
