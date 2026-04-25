/*
 * gui.c — OpenGL/GLUT visualization for the home furnishing competition.
 *
 * Two horizontal "lanes" — Team A on top, Team B on bottom. Each lane shows:
 *   - source pile on the left (rectangle, labeled "Pile")
 *   - team members in a row of circles
 *   - house on the right (drawn outline)
 *   - currently in-flight pieces drawn as colored squares between members
 *
 * GLUT timer fires every ~33 ms: it polls referee_tick() and triggers
 * a redraw. All OpenGL calls happen in this single thread.
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

#ifdef HAVE_GL
#include <GL/freeglut.h>
#endif

#ifdef HAVE_GL
static world_t* g_world = NULL;
static int      g_win_w = 1100;
static int      g_win_h = 600;
#endif

#ifdef HAVE_GL

/* ---------- drawing primitives ----------------------------------------- */

static void set_color(float r, float g, float b) { glColor3f(r, g, b); }

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

static void draw_circle(float cx, float cy, float r, int segments) {
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx, cy);
        for (int i = 0; i <= segments; i++) {
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

/* ---------- house "icon" ----------------------------------------------- */

static void draw_house(float x, float y, float w, float h, float fill_frac, int won) {
    /* roof */
    set_color(0.55f, 0.27f, 0.07f);
    glBegin(GL_TRIANGLES);
        glVertex2f(x - 5,       y + h * 0.65f);
        glVertex2f(x + w + 5,   y + h * 0.65f);
        glVertex2f(x + w / 2.f, y + h);
    glEnd();

    /* walls */
    if (won) set_color(1.0f, 0.85f, 0.2f);
    else     set_color(0.95f, 0.95f, 0.85f);
    draw_rect(x, y, w, h * 0.65f);
    set_color(0.2f, 0.2f, 0.2f);
    draw_rect_outline(x, y, w, h * 0.65f);

    /* door */
    set_color(0.4f, 0.25f, 0.1f);
    draw_rect(x + w * 0.42f, y, w * 0.16f, h * 0.32f);

    /* fill bar inside the house showing progress */
    float bar_h = h * 0.6f * fill_frac;
    set_color(0.2f, 0.7f, 0.3f);
    draw_rect(x + 4, y + 4, w - 8, bar_h);
}

/* ---------- main draw -------------------------------------------------- */

static void draw_team_lane(int t, float lane_y, float lane_h) {
    const cfg_t* cfg = &g_world->cfg;
    const team_state_t* ts = &g_world->teams[t];
    int N = cfg->team_size;

    /* lane boundaries */
    float pile_x   = 30.f;
    float pile_w   = 60.f;
    float house_x  = (float)g_win_w - 110.f;
    float house_w  = 80.f;
    float chain_x0 = pile_x + pile_w + 20.f;
    float chain_x1 = house_x - 20.f;
    float chain_y  = lane_y + lane_h * 0.5f;
    float chain_w  = chain_x1 - chain_x0;

    /* lane band */
    if (t == 0) set_color(0.93f, 0.97f, 1.0f);
    else        set_color(1.0f, 0.97f, 0.93f);
    draw_rect(0, lane_y, (float)g_win_w, lane_h);

    /* label */
    set_color(0.1f, 0.1f, 0.4f);
    draw_text_fmt(20, lane_y + lane_h - 22, GLUT_BITMAP_HELVETICA_18,
                  "TEAM %c   wins: %d / %d   delivered: %d / %d",
                  (char)('A' + t), ts->wins, cfg->wins_to_match,
                  ts->delivered_in_round, cfg->num_pieces);

    /* pile */
    set_color(0.6f, 0.4f, 0.2f);
    draw_rect(pile_x, chain_y - 25.f, pile_w, 50.f);
    set_color(0.2f, 0.2f, 0.2f);
    draw_rect_outline(pile_x, chain_y - 25.f, pile_w, 50.f);
    draw_text(pile_x + 10, chain_y - 5, GLUT_BITMAP_8_BY_13, "Pile");

    /* member positions along the chain */
    float member_xs[MAX_TEAM_SIZE];
    for (int m = 0; m < N; m++) {
        member_xs[m] = chain_x0 + chain_w * ((float)m / (float)(N - 1));
    }

    /* connector line */
    set_color(0.4f, 0.4f, 0.4f);
    glLineWidth(1.5f);
    glBegin(GL_LINES);
        glVertex2f(member_xs[0], chain_y);
        glVertex2f(member_xs[N - 1], chain_y);
    glEnd();

    /* members */
    for (int m = 0; m < N; m++) {
        if (m == 0)            set_color(0.2f, 0.6f, 0.9f);   /* source */
        else if (m == N - 1)   set_color(0.9f, 0.5f, 0.2f);   /* sink */
        else                   set_color(0.5f, 0.7f, 0.4f);
        draw_circle(member_xs[m], chain_y, 14.f, 24);

        set_color(0.0f, 0.0f, 0.0f);
        draw_text_fmt(member_xs[m] - 4, chain_y - 4, GLUT_BITMAP_8_BY_13, "%d", m);
    }

    /* in-flight pieces — draw a small square near whatever member currently holds them.
     * To avoid overdraw with thousands of pieces, only draw the most recently moved
     * piece per member (simple visualization). */
    int slot_count[MAX_TEAM_SIZE] = {0};
    for (int p = 0; p < cfg->num_pieces; p++) {
        if (ts->delivered[p]) continue;
        int pos = ts->piece_position[p];
        if (pos < 0 || pos >= N) continue;
        if (slot_count[pos] >= 4) continue;       /* show at most 4 per slot */
        float sx = member_xs[pos] - 8.f + (slot_count[pos] * 4.f);
        float sy = (t == 0) ? chain_y + 18.f + slot_count[pos] * 6.f
                            : chain_y - 24.f - slot_count[pos] * 6.f;
        slot_count[pos]++;

        set_color(0.85f, 0.2f, 0.2f);
        draw_rect(sx, sy, 6.f, 6.f);
    }

    /* house with fill bar */
    float frac = (cfg->num_pieces > 0)
                 ? (float)ts->delivered_in_round / (float)cfg->num_pieces : 0.f;
    int won_round_just_now = (ts->delivered_in_round >= cfg->num_pieces);
    draw_house(house_x, chain_y - 35.f, house_w, 80.f, frac, won_round_just_now);
}

static void display(void) {
    glClear(GL_COLOR_BUFFER_BIT);

    /* split screen into top half (Team A) and bottom half (Team B) */
    float lane_h = (float)g_win_h * 0.45f;
    float lane_a_y = (float)g_win_h - lane_h;
    float lane_b_y = 0.f;

    draw_team_lane(0, lane_a_y, lane_h);
    draw_team_lane(1, lane_b_y, lane_h);

    /* divider strip / scoreboard */
    set_color(0.95f, 0.95f, 0.95f);
    draw_rect(0, lane_h, (float)g_win_w, (float)g_win_h - 2 * lane_h);
    set_color(0.0f, 0.0f, 0.0f);
    draw_text_fmt((float)g_win_w / 2.f - 80.f, lane_h + 10.f, GLUT_BITMAP_HELVETICA_18,
                  "Round %d", g_world->current_round);
    if (g_world->winner >= 0) {
        set_color(0.7f, 0.0f, 0.0f);
        draw_text_fmt((float)g_win_w / 2.f - 130.f, lane_h + 30.f, GLUT_BITMAP_HELVETICA_18,
                      "TEAM %c WINS!", (char)('A' + g_world->winner));
    }

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
        static int countdown = 60;   /* 60 frames * 33ms ≈ 2s */
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

    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA);
    glutInitWindowSize(g_win_w, g_win_h);
    glutCreateWindow("ENCS4330 — Home Furnishing Competition");

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(0, timer, 0);

    /* on close: clean shutdown */
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
    /* Just poll referee_tick() until the match ends. */
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
    /* Linger briefly so any final messages drain. */
    for (int i = 0; i < 10; i++) { referee_tick(world); usleep(30000); }
    referee_shutdown(world);
}
