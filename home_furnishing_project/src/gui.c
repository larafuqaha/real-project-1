/*
 * gui.c — OpenGL/GLUT visualization for the home furnishing competition.
 *
 * 
 */

#include "gui.h"
#include "referee.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef HAVE_GL
#include <GL/freeglut.h>
#endif

#ifdef HAVE_GL
static world_t* g_world = NULL;
static int      g_win_w = 1280;
static int      g_win_h = 720;
static double   g_t_start = 0.0;     /* wall-clock start for on-screen timer */
#endif

#ifdef HAVE_GL

/* ---------- helpers ---------------------------------------------------- */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void set_color(float r, float g, float b)            { glColor3f(r, g, b); }
static void set_color_a(float r, float g, float b, float a) { glColor4f(r, g, b, a); }

/* HSV(0..1, 0..1, 0..1) -> RGB(0..1, 0..1, 0..1).  Used to give every
 * piece its own colour, deterministically derived from its piece-index. */
static void hsv_to_rgb(float h, float s, float v,
                       float* r, float* g, float* b)
{
    h = h - floorf(h);                       /* wrap into [0,1) */
    float i = floorf(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1.f - s);
    float q = v * (1.f - s * f);
    float t = v * (1.f - s * (1.f - f));
    switch ((int)i % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}

/* Pick a stable, vivid colour for a given piece serial (1..num_pieces).
 * Same serial -> same colour, every time, on both teams' 
 * . */
static void piece_color(int serial, float* r, float* g, float* b)
{
    if (serial <= 0) { *r = *g = *b = 0.5f; return; }
    /* offset by a small constant so serial=1 doesn't land exactly on red */
    float h = fmodf((float)(serial - 1) * 0.61803398875f + 0.13f, 1.f);
    hsv_to_rgb(h, 0.85f, 0.97f, r, g, b);
}

static void draw_rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
    glEnd();
}

static void draw_rect_outline(float x, float y, float w, float h) {
    glBegin(GL_LINE_LOOP);
        glVertex2f(x,     y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x,     y + h);
    glEnd();
}

/* Filled rounded rectangle */
static void draw_rounded_rect(float x, float y, float w, float h, float r) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    int seg = 6;

    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(x + w * 0.5f, y + h * 0.5f);
        /* bottom-right corner */
        for (int i = 0; i <= seg; i++) {
            float a = -(float)M_PI / 2.f + (float)i / seg * (float)M_PI / 2.f;
            glVertex2f(x + w - r + r * cosf(a), y + r + r * sinf(a));
        }
        /* top-right */
        for (int i = 0; i <= seg; i++) {
            float a = 0.f + (float)i / seg * (float)M_PI / 2.f;
            glVertex2f(x + w - r + r * cosf(a), y + h - r + r * sinf(a));
        }
        /* top-left */
        for (int i = 0; i <= seg; i++) {
            float a = (float)M_PI / 2.f + (float)i / seg * (float)M_PI / 2.f;
            glVertex2f(x + r + r * cosf(a), y + h - r + r * sinf(a));
        }
        /* bottom-left */
        for (int i = 0; i <= seg; i++) {
            float a = (float)M_PI + (float)i / seg * (float)M_PI / 2.f;
            glVertex2f(x + r + r * cosf(a), y + r + r * sinf(a));
        }
        /* close to first vertex */
        glVertex2f(x + w, y + r);
    glEnd();
}

static void draw_circle(float cx, float cy, float r, int segments) {
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= segments; i++) {
            float a = (float)(2.0 * M_PI * i / segments);
            glVertex2f(cx + r * cosf(a), cy + r * sinf(a));
        }
    glEnd();
}

static void draw_ring(float cx, float cy, float r, int segments) {
    glBegin(GL_LINE_LOOP);
        for (int i = 0; i < segments; i++) {
            float a = (float)(2.0 * M_PI * i / segments);
            glVertex2f(cx + r * cosf(a), cy + r * sinf(a));
        }
    glEnd();
}

static void draw_text(float x, float y, void* font, const char* s) {
    glRasterPos2f(x, y);
    while (*s) glutBitmapCharacter(font, *s++);
}

static void draw_text_fmt(float x, float y, void* font, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    draw_text(x, y, font, buf);
}

/* approximate width of bitmap text — used to centre labels */
static int text_width(void* font, const char* s) {
    int w = 0;
    while (*s) w += glutBitmapWidth(font, *s++);
    return w;
}

/* ---------- icons ------------------------------------------------------ */

/* Source-side "pile" drawn as three stacked crates. */
static void draw_pile(float x, float y, float w, float h) {
    float ch = h / 3.f;
    /* shadow */
    set_color_a(0.f, 0.f, 0.f, 0.25f);
    draw_rounded_rect(x + 4, y - 4, w, h, 6);
    /* crates */
    set_color(0.55f, 0.36f, 0.18f);
    draw_rounded_rect(x, y,         w, ch, 4);
    set_color(0.62f, 0.42f, 0.22f);
    draw_rounded_rect(x + 4, y + ch + 2, w - 8, ch, 4);
    set_color(0.70f, 0.48f, 0.26f);
    draw_rounded_rect(x + 8, y + 2 * ch + 4, w - 16, ch, 4);
    /* slats */
    set_color_a(0.0f, 0.0f, 0.0f, 0.4f);
    glLineWidth(1.0f);
    glBegin(GL_LINES);
        glVertex2f(x + w * 0.5f, y + 2);
        glVertex2f(x + w * 0.5f, y + ch - 2);
        glVertex2f(x + 4 + (w - 8) * 0.5f, y + ch + 4);
        glVertex2f(x + 4 + (w - 8) * 0.5f, y + 2 * ch);
    glEnd();
}

static void draw_house(float x, float y, float w, float h, float fill_frac, int won) {
    if (fill_frac < 0) fill_frac = 0;
    if (fill_frac > 1) fill_frac = 1;

    /* shadow */
    set_color_a(0, 0, 0, 0.25f);
    draw_rect(x + 4, y - 4, w, h * 0.65f);

    /* roof */
    set_color(0.55f, 0.27f, 0.07f);
    glBegin(GL_TRIANGLES);
        glVertex2f(x - 6,      y + h * 0.65f);
        glVertex2f(x + w + 6,  y + h * 0.65f);
        glVertex2f(x + w/2.f,  y + h);
    glEnd();

    /* walls */
    if (won) set_color(1.00f, 0.85f, 0.20f);
    else     set_color(0.96f, 0.95f, 0.86f);
    draw_rect(x, y, w, h * 0.65f);
    set_color(0.15f, 0.15f, 0.15f);
    glLineWidth(2.f);
    draw_rect_outline(x, y, w, h * 0.65f);

    /* door */
    set_color(0.4f, 0.25f, 0.1f);
    draw_rect(x + w * 0.42f, y, w * 0.16f, h * 0.32f);

    /* fill bar */
    float bar_h = (h * 0.6f) * fill_frac;
    set_color(0.25f, 0.78f, 0.36f);
    draw_rect(x + 4, y + 4, w - 8, bar_h);
    set_color_a(1.f, 1.f, 1.f, 0.18f);
    draw_rect(x + 4, y + 4 + bar_h * 0.5f, w - 8, bar_h * 0.5f);
}

/* ---------- background ------------------------------------------------- */

static void draw_background(void) {
    /* deep navy gradient */
    glBegin(GL_QUADS);
        glColor3f(0.07f, 0.09f, 0.16f);  glVertex2f(0,           0);
        glColor3f(0.07f, 0.09f, 0.16f);  glVertex2f((float)g_win_w, 0);
        glColor3f(0.13f, 0.16f, 0.27f);  glVertex2f((float)g_win_w, (float)g_win_h);
        glColor3f(0.13f, 0.16f, 0.27f);  glVertex2f(0,           (float)g_win_h);
    glEnd();

    /* subtle grid */
    set_color_a(1.f, 1.f, 1.f, 0.04f);
    glLineWidth(1.f);
    glBegin(GL_LINES);
        for (int x = 0; x < g_win_w; x += 40) {
            glVertex2f((float)x, 0);
            glVertex2f((float)x, (float)g_win_h);
        }
        for (int y = 0; y < g_win_h; y += 40) {
            glVertex2f(0,            (float)y);
            glVertex2f((float)g_win_w, (float)y);
        }
    glEnd();
}

/* ---------- top scoreboard --------------------------------------------- */

static void draw_topbar(double elapsed) {
    float bar_y = (float)g_win_h - 60.f;

    /* card */
    set_color_a(1.f, 1.f, 1.f, 0.08f);
    draw_rounded_rect(20.f, bar_y, (float)g_win_w - 40.f, 50.f, 12.f);

    /* title */
    set_color(0.90f, 0.94f, 1.0f);
    draw_text(40.f, bar_y + 18.f, GLUT_BITMAP_HELVETICA_18,
              "ENCS4330  -  Home Furnishing Competition");

    /* round */
    set_color(0.75f, 0.85f, 1.0f);
    draw_text_fmt((float)g_win_w * 0.42f, bar_y + 18.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "Round %d", g_world->current_round);

    /* timer (MM:SS.s) */
    int total = (int)elapsed;
    int mm = total / 60;
    int ss = total % 60;
    int tenths = (int)((elapsed - total) * 10.0);
    set_color(0.85f, 1.00f, 0.85f);
    draw_text_fmt((float)g_win_w * 0.55f, bar_y + 18.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "Time  %02d:%02d.%d", mm, ss, tenths);

    /* scores: A and B */
    int wa = g_world->teams[0].wins;
    int wb = g_world->teams[1].wins;
    int wm = g_world->cfg.wins_to_match;

    set_color(0.55f, 0.80f, 1.00f);
    draw_text_fmt((float)g_win_w * 0.72f, bar_y + 18.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "A: %d", wa);
    set_color(1.00f, 0.78f, 0.55f);
    draw_text_fmt((float)g_win_w * 0.78f, bar_y + 18.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "B: %d", wb);
    set_color(0.85f, 0.85f, 0.90f);
    draw_text_fmt((float)g_win_w * 0.85f, bar_y + 18.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "/ %d to win", wm);
}

/* ---------- one team lane --------------------------------------------- */

static void draw_team_lane(int t, float lane_y, float lane_h) {
    const cfg_t* cfg = &g_world->cfg;
    const team_state_t* ts = &g_world->teams[t];
    int N = cfg->team_size;
    if (N < 1) return;

    /* per-team accent colours */
    float accent_r = (t == 0) ? 0.30f : 0.95f;
    float accent_g = (t == 0) ? 0.65f : 0.55f;
    float accent_b = (t == 0) ? 1.00f : 0.25f;

    /* lane card */
    set_color_a(1.f, 1.f, 1.f, 0.05f);
    draw_rounded_rect(20.f, lane_y, (float)g_win_w - 40.f, lane_h, 14.f);

    /* lane label */
    set_color(accent_r, accent_g, accent_b);
    draw_text_fmt(40.f, lane_y + lane_h - 24.f,
                  GLUT_BITMAP_HELVETICA_18,
                  "TEAM %c", (char)('A' + t));
    set_color(0.80f, 0.85f, 0.95f);
    draw_text_fmt(110.f, lane_y + lane_h - 24.f,
                  GLUT_BITMAP_HELVETICA_12,
                  "wins %d / %d   delivered this round %d / %d",
                  ts->wins, cfg->wins_to_match,
                  ts->delivered_in_round, cfg->num_pieces);

    /* progress bar (just under the label) */
    float pb_x = 40.f, pb_y = lane_y + lane_h - 42.f;
    float pb_w = (float)g_win_w - 80.f, pb_h = 6.f;
    set_color_a(1.f, 1.f, 1.f, 0.08f);
    draw_rounded_rect(pb_x, pb_y, pb_w, pb_h, 3.f);
    float frac = (cfg->num_pieces > 0)
        ? (float)ts->delivered_in_round / (float)cfg->num_pieces : 0.f;
    set_color_a(accent_r, accent_g, accent_b, 0.85f);
    draw_rounded_rect(pb_x, pb_y, pb_w * frac, pb_h, 3.f);

    /* lane geometry for chain */
    float pile_x   = 50.f;
    float pile_w   = 60.f;
    float house_x  = (float)g_win_w - 120.f;
    float house_w  = 70.f;
    float chain_x0 = pile_x + pile_w + 30.f;
    float chain_x1 = house_x - 30.f;
    float chain_y  = lane_y + lane_h * 0.45f;
    float chain_w  = chain_x1 - chain_x0;

    /* pile */
    draw_pile(pile_x, chain_y - 30.f, pile_w, 60.f);
    set_color(0.85f, 0.85f, 0.90f);
    draw_text(pile_x + 14.f, chain_y - 46.f, GLUT_BITMAP_8_BY_13, "Source");

    /* house with fill bar */
    int won = (ts->delivered_in_round >= cfg->num_pieces);
    draw_house(house_x, chain_y - 35.f, house_w, 80.f, frac, won);
    set_color(0.85f, 0.85f, 0.90f);
    draw_text(house_x + 14.f, chain_y - 51.f, GLUT_BITMAP_8_BY_13, "House");

    /* member positions along the chain.  
     */
    float member_xs[MAX_TEAM_SIZE];
    for (int m = 0; m < MAX_TEAM_SIZE; m++) member_xs[m] = 0.f;

    if (N == 1) {
        member_xs[0] = chain_x0 + chain_w * 0.5f;
    } else {
        for (int m = 0; m < N; m++) {
            member_xs[m] = chain_x0 + chain_w * ((float)m / (float)(N - 1));
        }
    }

    /* connector line — soft glow */
    set_color_a(accent_r, accent_g, accent_b, 0.45f);
    glLineWidth(4.f);
    glBegin(GL_LINES);
        glVertex2f(member_xs[0],     chain_y);
        glVertex2f(member_xs[N - 1], chain_y);
    glEnd();
    set_color_a(1.0f, 1.0f, 1.0f, 0.85f);
    glLineWidth(1.5f);
    glBegin(GL_LINES);
        glVertex2f(member_xs[0],     chain_y);
        glVertex2f(member_xs[N - 1], chain_y);
    glEnd();

    /* members */
    for (int m = 0; m < N; m++) {
        /* outer halo */
        set_color_a(accent_r, accent_g, accent_b, 0.25f);
        draw_circle(member_xs[m], chain_y, 22.f, 32);
        /* body */
        if (m == 0)              set_color(0.30f, 0.65f, 1.00f);   /* source */
        else if (m == N - 1)     set_color(1.00f, 0.55f, 0.20f);   /* sink   */
        else                     set_color(0.55f, 0.78f, 0.55f);
        draw_circle(member_xs[m], chain_y, 16.f, 32);
        /* outline */
        set_color_a(0.f, 0.f, 0.f, 0.6f);
        glLineWidth(1.5f);
        draw_ring(member_xs[m], chain_y, 16.f, 32);

        /* label */
        char lbl[16];
        snprintf(lbl, sizeof lbl, "%d", m);
        int tw = text_width(GLUT_BITMAP_HELVETICA_12, lbl);
        set_color(1.f, 1.f, 1.f);
        draw_text(member_xs[m] - tw * 0.5f, chain_y - 4.f,
                  GLUT_BITMAP_HELVETICA_12, lbl);
    }

    /* in-flight pieces.  Forward pieces drawn above the chain line, backward
     * (rejected) pieces drawn below it.  Each piece has a stable colour
     * derived from its serial so it can be tracked visually as it moves. */
    int slot_above[MAX_TEAM_SIZE] = {0};
    int slot_below[MAX_TEAM_SIZE] = {0};
    for (int p = 0; p < cfg->num_pieces; p++) {
        if (ts->delivered[p]) continue;
        int pos = ts->piece_position[p];
        if (pos < 0 || pos >= N) continue;

        /* up to 4 stacked per side per slot to avoid runaway overdraw */
        int dir_up = 1;            /* default: forward → above */
        if (slot_above[pos] >= 4 && slot_below[pos] >= 4) continue;

        /* without per-piece direction state we approximate: pieces at the
         * sink that haven't been delivered are bouncing back, so draw below. */
        if (pos == N - 1) dir_up = 0;

        /* per-piece colour, derived from this piece's serial number so
         * a given piece keeps the same colour as it travels.  If we don't
         * know the serial yet (piece hasn't moved), fall back to a colour
         * derived from the index. */
        float pr, pg, pb;
        int s_for_color = ts->piece_serial[p];
        if (s_for_color <= 0) s_for_color = p + 1;
        piece_color(s_for_color, &pr, &pg, &pb);

        float sx, sy;
        const float pw = 22.f, ph = 18.f;   /* piece size */
        if (dir_up) {
            if (slot_above[pos] >= 4) continue;
            sx = member_xs[pos] - pw * 0.5f + (slot_above[pos] * 6.f);
            sy = chain_y + 28.f + slot_above[pos] * (ph + 4.f);
            slot_above[pos]++;
        } else {
            if (slot_below[pos] >= 4) continue;
            sx = member_xs[pos] - pw * 0.5f + (slot_below[pos] * 6.f);
            sy = chain_y - 28.f - ph - slot_below[pos] * (ph + 4.f);
            slot_below[pos]++;
        }

        /* drop shadow */
        set_color_a(0.f, 0.f, 0.f, 0.45f);
        draw_rounded_rect(sx + 2.f, sy - 2.f, pw, ph, 4.f);
        /* body in piece colour */
        set_color(pr, pg, pb);
        draw_rounded_rect(sx, sy, pw, ph, 4.f);
        /* glossy highlight on the top half */
        set_color_a(1.f, 1.f, 1.f, 0.30f);
        draw_rounded_rect(sx + 1.f, sy + ph * 0.5f, pw - 2.f, ph * 0.45f, 3.f);
        /* dark outline so light pieces stay visible on the dark bg */
        set_color_a(0.f, 0.f, 0.f, 0.85f);
        glLineWidth(1.5f);
        draw_rect_outline(sx, sy, pw, ph);

        /* serial number on top of the piece (always show, fitting font) */
        char sn[8];
        snprintf(sn, sizeof sn, "%d", ts->piece_serial[p]);
        void* sn_font = (strlen(sn) >= 3) ? GLUT_BITMAP_HELVETICA_10
                                          : GLUT_BITMAP_HELVETICA_12;
        int snw = text_width(sn_font, sn);
        /* pick black or white text based on luminance for legibility */
        float lum = 0.30f * pr + 0.59f * pg + 0.11f * pb;
        if (lum > 0.55f) set_color(0.05f, 0.05f, 0.05f);
        else             set_color(0.98f, 0.98f, 0.98f);
        draw_text(sx + (pw - snw) * 0.5f, sy + 4.f, sn_font, sn);
    }
}

/* ---------- main draw -------------------------------------------------- */

static void draw_winner_banner(void) {
    if (g_world->winner < 0) return;

    /* dim the screen */
    set_color_a(0.f, 0.f, 0.f, 0.55f);
    draw_rect(0, 0, (float)g_win_w, (float)g_win_h);

    /* banner card */
    float bw = 460.f, bh = 140.f;
    float bx = ((float)g_win_w - bw) * 0.5f;
    float by = ((float)g_win_h - bh) * 0.5f;
    set_color_a(1.f, 1.f, 1.f, 0.96f);
    draw_rounded_rect(bx, by, bw, bh, 16.f);

    set_color(0.10f, 0.10f, 0.18f);
    char msg[64];
    snprintf(msg, sizeof msg, "TEAM %c WINS THE COMPETITION!",
             (char)('A' + g_world->winner));
    int tw = text_width(GLUT_BITMAP_TIMES_ROMAN_24, msg);
    draw_text(bx + (bw - tw) * 0.5f, by + bh * 0.55f,
              GLUT_BITMAP_TIMES_ROMAN_24, msg);

    set_color(0.30f, 0.30f, 0.40f);
    char sub[64];
    snprintf(sub, sizeof sub, "Final score:  A %d  -  %d B",
             g_world->teams[0].wins, g_world->teams[1].wins);
    int sw = text_width(GLUT_BITMAP_HELVETICA_18, sub);
    draw_text(bx + (bw - sw) * 0.5f, by + bh * 0.25f,
              GLUT_BITMAP_HELVETICA_18, sub);
}

static void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);

    /* enable blending so our halos / shadows look right */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_background();

    double elapsed = (g_t_start > 0.0) ? (now_sec() - g_t_start) : 0.0;
    draw_topbar(elapsed);

    /* two lanes, each ~40% of the height, with a small gap between them */
    float top_pad = 80.f;       /* room for top bar */
    float bot_pad = 30.f;
    float gap     = 16.f;
    float avail   = (float)g_win_h - top_pad - bot_pad - gap;
    float lane_h  = avail * 0.5f;

    float lane_a_y = (float)g_win_h - top_pad - lane_h;
    float lane_b_y = lane_a_y - gap - lane_h;

    draw_team_lane(0, lane_a_y, lane_h);
    draw_team_lane(1, lane_b_y, lane_h);

    /* footer hint */
    set_color_a(1.f, 1.f, 1.f, 0.45f);
    draw_text(20.f, 8.f, GLUT_BITMAP_8_BY_13,
              "Press Q or Esc to quit.");

    draw_winner_banner();

    glDisable(GL_BLEND);
    glutSwapBuffers();
}

static void reshape(int w, int h) {
    g_win_w = w; g_win_h = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, w, 0, h);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void timer(int v) {
    (void)v;
    if (!g_world->shutting_down) referee_tick(g_world);
    glutPostRedisplay();

    if (g_world->shutting_down && g_world->winner >= 0) {
        /* Stay open for ~2s after winner declared, then quit. */
        static int countdown = 60;   /* 60 frames * 33 ms ~ 2 s */
        if (--countdown <= 0) {
            referee_shutdown(g_world);
            glutLeaveMainLoop();
            return;
        }
    }
    glutTimerFunc(33, timer, 0);
}

static void keyboard(unsigned char k, int x, int y) {
    (void)x; (void)y;
    if (k == 27 || k == 'q' || k == 'Q') {
        referee_shutdown(g_world);
        glutLeaveMainLoop();
    }
}

void gui_run(int* argc, char** argv, world_t* world) {
    g_world = world;
    g_t_start = now_sec();

    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("ENCS4330 - Home Furnishing Competition");

    /* navy background — actual gradient is drawn in draw_background() */
    glClearColor(0.07f, 0.09f, 0.16f, 1.0f);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(0, timer, 0);

    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutMainLoop();
}

#else /* !HAVE_GL */

void gui_run(int* argc, char** argv, world_t* world) {
    (void)argc; (void)argv;
    fprintf(stderr, "[gui] OpenGL not available at compile time, falling back to headless mode.\n");
    headless_run(world);
}

#endif

void headless_run(world_t* world) {
    int dbg = getenv("FURN_TRACE") != NULL;
    int ticks = 0;
    while (!world->shutting_down) {
        referee_tick(world);
        if (dbg && (ticks % 30) == 0) {
            fprintf(stderr, "[parent] tick=%d wins A=%d B=%d round=%d delivered=%d/%d %d/%d\n",
                    ticks, world->teams[0].wins, world->teams[1].wins,
                    world->current_round,
                    world->teams[0].delivered_in_round, world->cfg.num_pieces,
                    world->teams[1].delivered_in_round, world->cfg.num_pieces);
        }
        ticks++;
        struct timespec ts = {0, 30 * 1000000L};
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < 10; i++) { referee_tick(world); usleep(30000); }
    referee_shutdown(world);
}
