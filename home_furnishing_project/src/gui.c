/*
 * gui.c — Home Furnishing Competition visualization.
 *
 * Aesthetic: soft pink/lavender/rose gold palette, rounded shapes, sparkle
 * accents, pastel gradients — professional but feminine.
 *
 * Movement fix: pieces are tracked with VELOCITY-based physics, not lerp.
 * Each piece knows its CURRENT member and TARGET member from the referee.
 * When target changes, the piece launches with a fixed speed (pixels/second)
 * so you visually see it travel across the full chain regardless of pause time.
 *
 * Layout (top → bottom):
 *   [ HEADER: title / round / time ]
 *   [ TEAM A LANE: pile → M0 → M1 → M2 → M3(sink) → house ]
 *   [ SCOREBOARD: dual progress bars + winner banner ]
 *   [ TEAM B LANE: pile → M0 → M1 → M2 → M3(sink) → house ]
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
#include <unistd.h>

#ifdef HAVE_GL
#include <GL/freeglut.h>
#endif

#ifdef HAVE_GL

/* ── window ── */
static world_t* g_world   = NULL;
static int      g_win_w   = 1300;
static int      g_win_h   = 780;
static double   g_elapsed = 0.0;
static struct timespec g_t0;

/* ── piece animation ────────────────────────────────────────────────────
 * Rather than velocity-based physics, we use TIME-BASED INTERPOLATION:
 *   - referee stores piece_position[p], piece_direction[p], piece_moved_at[p]
 *   - each frame we compute  progress = (now - moved_at) / avg_pause
 *   - screen pos is lerped between member[pos] and member[pos+dir]
 * This makes pieces glide continuously across the full chain gap during
 * the member's tired_pause() — no extra IPC needed.
 * ──────────────────────────────────────────────────────────────────────*/

typedef struct {
    float  x,  y;   /* smoothed screen position (drawn here) */
    float  tx, ty;  /* interpolated target this frame */
    int    alive;
} PieceAnim;

#define PIECE_SPEED 220.f      /* pixels per second */

static PieceAnim g_pa[NUM_TEAMS][MAX_PIECES];

/* Cache of member screen positions filled by draw_lane() each frame */
static float g_mx[NUM_TEAMS][MAX_TEAM_SIZE];
static float g_my[NUM_TEAMS][MAX_TEAM_SIZE];

/* ── palette (soft girly) ───────────────────────────────────────────── */
/* Backgrounds */
static const float BG1[3]  = {0.13f, 0.08f, 0.14f};   /* deep plum */
static const float BG2[3]  = {0.19f, 0.11f, 0.20f};   /* aubergine */

/* Lane tints */
static const float LA1[3]  = {0.22f, 0.12f, 0.26f};   /* team A: purple */
static const float LA2[3]  = {0.14f, 0.08f, 0.17f};
static const float LB1[3]  = {0.26f, 0.12f, 0.18f};   /* team B: rose */
static const float LB2[3]  = {0.17f, 0.08f, 0.12f};

/* Accents */
static const float ACC_A[3] = {0.88f, 0.52f, 0.85f};  /* orchid */
static const float ACC_B[3] = {1.00f, 0.60f, 0.72f};  /* rose pink */
static const float GOLD[3]  = {1.00f, 0.82f, 0.50f};  /* rose gold */
static const float MINT[3]  = {0.55f, 0.95f, 0.78f};  /* mint green */
static const float CORAL[3] = {1.00f, 0.45f, 0.45f};  /* coral/warn */

/* Text */
static const float TXT_HI[3]  = {1.00f, 0.95f, 0.98f};  /* near-white */
static const float TXT_DIM[3] = {0.76f, 0.65f, 0.78f};  /* muted lilac */

/* Member role colors */
static const float SRC_IN[3]  = {0.90f, 0.60f, 1.00f};  /* violet */
static const float SRC_OUT[3] = {0.45f, 0.20f, 0.55f};
static const float MID_IN[3]  = {1.00f, 0.75f, 0.88f};  /* blush */
static const float MID_OUT[3] = {0.55f, 0.25f, 0.38f};
static const float SNK_IN[3]  = {1.00f, 0.85f, 0.50f};  /* gold */
static const float SNK_OUT[3] = {0.55f, 0.35f, 0.10f};

/* Piece colors (they alternate so you can tell them apart) */
static const float PIECE_COLS[6][3] = {
    {1.00f, 0.45f, 0.60f},   /* hot pink */
    {0.90f, 0.55f, 1.00f},   /* lavender */
    {1.00f, 0.75f, 0.40f},   /* peach */
    {0.50f, 0.90f, 0.80f},   /* aqua */
    {1.00f, 0.65f, 0.75f},   /* rose */
    {0.75f, 0.60f, 1.00f},   /* periwinkle */
};

/* ── math helpers ───────────────────────────────────────────────────── */

/* ── draw helpers ───────────────────────────────────────────────────── */
static void col(const float c[3])             { glColor3fv(c); }
static void cola(const float c[3], float a)   { glColor4f(c[0],c[1],c[2],a); }

static void rect(float x, float y, float w, float h) {
    glBegin(GL_QUADS);
        glVertex2f(x,y); glVertex2f(x+w,y);
        glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}
static void rect_ol(float x, float y, float w, float h) {
    glBegin(GL_LINE_LOOP);
        glVertex2f(x,y); glVertex2f(x+w,y);
        glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}
static void grad_v(float x, float y, float w, float h,
                   const float t[3], const float b[3]) {
    glBegin(GL_QUADS);
        glColor3fv(b); glVertex2f(x,y);   glVertex2f(x+w,y);
        glColor3fv(t); glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}
static void disk(float cx, float cy, float r, int s) {
    glBegin(GL_TRIANGLE_FAN);
        glVertex2f(cx,cy);
        for(int i=0;i<=s;i++){
            float a=(float)(2*M_PI*i/s);
            glVertex2f(cx+r*cosf(a),cy+r*sinf(a));
        }
    glEnd();
}
static void ring(float cx, float cy, float r, int s) {
    glBegin(GL_LINE_LOOP);
        for(int i=0;i<s;i++){
            float a=(float)(2*M_PI*i/s);
            glVertex2f(cx+r*cosf(a),cy+r*sinf(a));
        }
    glEnd();
}
static void arc(float cx,float cy,float r,float a0,float a1,int s){
    glBegin(GL_LINE_STRIP);
        for(int i=0;i<=s;i++){
            float a=a0+(a1-a0)*(float)i/s;
            glVertex2f(cx+r*cosf(a),cy+r*sinf(a));
        }
    glEnd();
}
static void disk_grad(float cx,float cy,float r,
                      const float inner[3],const float outer[3]){
    glBegin(GL_TRIANGLE_FAN);
        glColor3fv(inner); glVertex2f(cx,cy);
        glColor3fv(outer);
        for(int i=0;i<=36;i++){
            float a=(float)(2*M_PI*i/36);
            glVertex2f(cx+r*cosf(a),cy+r*sinf(a));
        }
    glEnd();
}
static void txt(float x,float y,void* f,const char* s){
    glRasterPos2f(x,y);
    while(*s) glutBitmapCharacter(f,*s++);
}
static void txtf(float x,float y,void* f,const char* fmt,...){
    char buf[256]; va_list ap;
    va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    txt(x,y,f,buf);
}
static int tw18(const char* s){
    int w=0;
    while(*s) w+=glutBitmapWidth(GLUT_BITMAP_HELVETICA_18,(unsigned char)*s++);
    return w;
}

/* ── sparkle dots (decorative) ─────────────────────────────────────── */
static void sparkles(float cx, float cy, float r, double t, int n,
                     const float c[3]) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    for(int i=0;i<n;i++){
        float a = (float)(2*M_PI*i/n) + (float)t*0.8f;
        float br = r + 5.f * sinf((float)t*3.f + i*1.3f);
        float bx = cx + br*cosf(a);
        float by = cy + br*sinf(a);
        float alpha = 0.5f + 0.5f*sinf((float)t*4.f + i*0.9f);
        cola(c, alpha*0.7f);
        disk(bx,by,1.5f,6);
    }
    glDisable(GL_BLEND);
}

/* ── HEADER ─────────────────────────────────────────────────────────── */
static void draw_header(void){
    float hh=52.f, y=(float)g_win_h-hh;
    float t[3]={0.28f,0.14f,0.32f}, b[3]={0.18f,0.09f,0.21f};
    grad_v(0,y,(float)g_win_w,hh,t,b);

    /* shimmer line */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    cola(ACC_A,0.6f); glLineWidth(1.5f);
    glBegin(GL_LINES);
        glVertex2f(0,y); glVertex2f((float)g_win_w,y);
    glEnd();
    glDisable(GL_BLEND);

    col(TXT_HI);
    txt(20,y+20,GLUT_BITMAP_HELVETICA_18,"Home Furnishing Competition  ✦  ENCS4330");

    char buf[128];
    snprintf(buf,sizeof buf,"ROUND %d    ✦    %.1fs",
             g_world->current_round, g_elapsed);
    int bw=tw18(buf);
    col(TXT_HI);
    txt((float)g_win_w/2.f-bw/2.f, y+20, GLUT_BITMAP_HELVETICA_18, buf);

    col(TXT_DIM);
    const char* h2="Q to quit";
    txt((float)g_win_w-tw18(h2)-20, y+20, GLUT_BITMAP_HELVETICA_18, h2);
}

/* ── HOUSE ──────────────────────────────────────────────────────────── */
static void draw_house(float x,float y,float w,float h,
                       float fill_frac,int won){
    float wh=h*0.62f, rh=h*0.38f;

    /* roof */
    float rc[3];
    if(won){ rc[0]=1.0f; rc[1]=0.6f; rc[2]=0.75f; }
    else   { rc[0]=0.7f; rc[1]=0.3f; rc[2]=0.5f;  }
    glColor3fv(rc);
    glBegin(GL_TRIANGLES);
        glVertex2f(x-6,y+wh);
        glVertex2f(x+w+6,y+wh);
        glVertex2f(x+w/2.f,y+wh+rh);
    glEnd();
    glColor3f(0.4f,0.1f,0.25f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
        glVertex2f(x-6,y+wh);
        glVertex2f(x+w+6,y+wh);
        glVertex2f(x+w/2.f,y+wh+rh);
    glEnd();

    /* walls */
    if(won){ glColor3f(1.0f,0.92f,0.96f); }
    else    { glColor3f(0.95f,0.88f,0.92f); }
    rect(x,y,w,wh);
    glColor3f(0.45f,0.18f,0.30f);
    rect_ol(x,y,w,wh);

    /* door */
    glColor3f(0.6f,0.28f,0.45f);
    float dw=w*0.22f, dh=wh*0.55f;
    rect(x+(w-dw)/2.f,y,dw,dh);
    glColor3f(0.35f,0.12f,0.25f);
    rect_ol(x+(w-dw)/2.f,y,dw,dh);
    /* heart knob */
    col(GOLD);
    disk(x+(w-dw)/2.f+dw-4.f, y+dh/2.f, 2.f, 8);

    /* windows */
    glColor3f(0.75f,0.88f,1.0f);
    float wnw=w*0.18f, wnh=wh*0.24f, wny=y+wh-wnh-7;
    rect(x+7,wny,wnw,wnh);
    rect(x+w-wnw-7,wny,wnw,wnh);
    glColor3f(0.35f,0.18f,0.30f);
    rect_ol(x+7,wny,wnw,wnh);
    rect_ol(x+w-wnw-7,wny,wnw,wnh);
    glBegin(GL_LINES);
        glVertex2f(x+7+wnw/2.f,wny); glVertex2f(x+7+wnw/2.f,wny+wnh);
        glVertex2f(x+7,wny+wnh/2.f); glVertex2f(x+7+wnw,wny+wnh/2.f);
        glVertex2f(x+w-wnw-7+wnw/2.f,wny); glVertex2f(x+w-wnw-7+wnw/2.f,wny+wnh);
        glVertex2f(x+w-wnw-7,wny+wnh/2.f); glVertex2f(x+w-wnw-7+wnw,wny+wnh/2.f);
    glEnd();

    /* fill bar */
    if(fill_frac>0.f){
        float bh2=(wh-6)*fill_frac;
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(MINT[0],MINT[1],MINT[2],0.40f);
        rect(x+2,y+2,w-4,bh2);
        glDisable(GL_BLEND);
    }

    /* win glow */
    if(won){
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        cola(GOLD,0.8f);
        txt(x+w/2.f-14,y+wh+rh/2.f-4,GLUT_BITMAP_HELVETICA_12,"WIN!");
        glDisable(GL_BLEND);
        sparkles(x+w/2.f,y+wh+rh/2.f,18.f,g_elapsed,8,GOLD);
    }
}

/* ── PILE ───────────────────────────────────────────────────────────── */
static void draw_pile(float cx, float cy, int rem){
    if(rem>25) rem=25;
    int per_row=5; float bw=12,bh=10;
    for(int i=0;i<rem;i++){
        int row=i/per_row, col2=i%per_row;
        float bx=cx-(per_row*bw)/2.f+col2*bw;
        float by=cy-20+row*bh;
        const float* c=PIECE_COLS[i%6];
        glColor3f(c[0]*0.75f,c[1]*0.75f,c[2]*0.75f);
        rect(bx,by,bw-1,bh-1);
        glColor3f(c[0]*0.45f,c[1]*0.45f,c[2]*0.45f);
        rect_ol(bx,by,bw-1,bh-1);
    }
}

/* ── MEMBER NODE ────────────────────────────────────────────────────── */
static void draw_member(float cx,float cy,int role,int idx,
                        float fatigue,double t){
    /* glow halo */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    float pulse=0.4f+0.4f*sinf((float)t*3.5f+idx*0.9f);
    const float* gc=(role==0)?ACC_A:(role==2)?GOLD:ACC_B;
    cola(gc,0.12f*pulse);
    disk(cx,cy,26.f,24);
    glDisable(GL_BLEND);

    /* body */
    const float* in2=(role==0)?SRC_IN:(role==2)?SNK_IN:MID_IN;
    const float* out=(role==0)?SRC_OUT:(role==2)?SNK_OUT:MID_OUT;
    disk_grad(cx,cy,17.f,in2,out);

    /* ring */
    const float* rc=(role==0)?ACC_A:(role==2)?GOLD:ACC_B;
    col(rc); glLineWidth(2.f); ring(cx,cy,17.f,32);

    /* fatigue arc (coral, clockwise from top) */
    if(fatigue>0.01f){
        if(fatigue>1.f) fatigue=1.f;
        col(CORAL); glLineWidth(3.f);
        arc(cx,cy,21.f,(float)M_PI/2.f,
            (float)M_PI/2.f-2.f*(float)M_PI*fatigue,32);
        glLineWidth(1.f);
    }

    /* sparkle ring when source or sink */
    if(role==0||role==2)
        sparkles(cx,cy,20.f,t+(idx*0.4),5,rc);

    /* id label */
    glColor3f(0.08f,0.04f,0.10f);
    char buf[4]; snprintf(buf,sizeof buf,"%d",idx);
    int bw2=glutBitmapWidth(GLUT_BITMAP_HELVETICA_12,buf[0]);
    if(buf[1]) bw2+=glutBitmapWidth(GLUT_BITMAP_HELVETICA_12,buf[1]);
    txt(cx-bw2/2.f,cy-4,GLUT_BITMAP_HELVETICA_12,buf);
}

/* ── WIN PIPS ───────────────────────────────────────────────────────── */
static void draw_pips(float x,float y,int wins,int total,const float c[3]){
    float r=7.f,gap=18.f;
    for(int i=0;i<total;i++){
        if(i<wins){
            col(c); disk(x+i*gap,y,r,16);
            glColor3f(1,1,1); glLineWidth(1.f); ring(x+i*gap,y,r,16);
            sparkles(x+i*gap,y,10.f,g_elapsed+i,4,c);
        } else {
            glColor3f(0.25f,0.15f,0.28f); disk(x+i*gap,y,r,16);
            col(TXT_DIM); ring(x+i*gap,y,r,16);
        }
    }
}

/* ── PIECE ANIMATION UPDATE ─────────────────────────────────────────── */
static void update_pieces(int t){
    const cfg_t*        cfg = &g_world->cfg;
    const team_state_t* ts  = &g_world->teams[t];
    int N = cfg->team_size;

    float yoff = (t == 0) ? 30.f : -30.f;

    for(int p = 0; p < cfg->num_pieces; p++){
        PieceAnim* a = &g_pa[t][p];

        if(ts->delivered[p]){ a->alive = 0; continue; }

        int m = ts->piece_position[p];
        if(m < 0 || m >= N){ a->alive = 0; continue; }

        /* target = the member circle this piece is currently at */
        float tx = g_mx[t][m];
        float ty = g_my[t][m] + yoff;

        if(!a->alive){
            /* first frame: snap to source, start moving */
            a->x = g_mx[t][0];
            a->y = g_my[t][0] + yoff;
            a->alive = 1;
        }

        /* if the piece has a NEW member target that differs from where
         * the visual currently is, launch it at a fixed speed so you
         * see it travel the full gap between members */
        float dx = tx - a->x;
        float dy = ty - a->y;
        float dist = sqrtf(dx*dx + dy*dy);

        if(dist > 2.f){
            /* move at PIECE_SPEED pixels per second */
            float step = PIECE_SPEED * (16.f / 1000.f); /* assume ~16ms frame */
            if(step >= dist){
                a->x = tx; a->y = ty;
            } else {
                a->x += (dx/dist)*step;
                a->y += (dy/dist)*step;
            }
        } else {
            a->x = tx; a->y = ty;
        }

        a->tx = tx; a->ty = ty;
    }
}

static void draw_pieces(int t){
    const cfg_t* cfg=&g_world->cfg;
    for(int p=0;p<cfg->num_pieces;p++){
        PieceAnim* a=&g_pa[t][p];
        if(!a->alive) continue;

        const float* c=PIECE_COLS[p%6];

        /* shadow */
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0,0,0,0.30f);
        rect(a->x-4+1,a->y-4-1,9,9);
        glDisable(GL_BLEND);

        /* body */
        glColor3fv(c);
        rect(a->x-4,a->y-4,9,9);
        glColor3f(c[0]*0.5f,c[1]*0.5f,c[2]*0.5f);
        rect_ol(a->x-4,a->y-4,9,9);

        /* tiny glint */
        glColor3f(1,1,1);
        disk(a->x+1,a->y+2,1.2f,6);
    }
}

/* ── TEAM LANE ──────────────────────────────────────────────────────── */
static void draw_lane(int t, float lane_y, float lane_h){
    const cfg_t* cfg=&g_world->cfg;
    const team_state_t* ts=&g_world->teams[t];
    int N=cfg->team_size;
    int top=(t==0);

    /* background */
    const float* l1=top?LA1:LB1;
    const float* l2=top?LA2:LB2;
    grad_v(0,lane_y,(float)g_win_w,lane_h,l1,l2);

    /* accent stripe at lane edge */
    const float* acc=top?ACC_A:ACC_B;
    col(acc);
    rect(0,top?(lane_y+lane_h-4):lane_y,(float)g_win_w,4);

    /* layout */
    float pad=60.f;
    float pw=80.f, phx=pad;
    float hw=115.f, hhx=(float)g_win_w-pad-hw;
    float cx0=phx+pw+50.f;
    float cx1=hhx-50.f;
    float cy=lane_y+lane_h*0.50f;
    float cw=cx1-cx0;

    /* pre-compute and cache member positions for this team */
    for(int m=0;m<N;m++){
        g_mx[t][m]=cx0+cw*((float)m/(float)(N-1));
        g_my[t][m]=cy;
    }

    /* team label + status */
    col(TXT_HI);
    txtf(20,lane_y+lane_h-30,GLUT_BITMAP_HELVETICA_18,
         "TEAM %c",(char)('A'+t));
    col(TXT_DIM);
    txtf(20,lane_y+lane_h-50,GLUT_BITMAP_HELVETICA_12,
         "delivered %d / %d",ts->delivered_in_round,cfg->num_pieces);

    /* win pips */
    float px2=(float)g_win_w-220.f;
    float py2=lane_y+lane_h-24.f;
    col(TXT_DIM);
    txt(px2-72,py2-5,GLUT_BITMAP_HELVETICA_12,"WINS:");
    draw_pips(px2,py2,ts->wins,cfg->wins_to_match,acc);

    /* chain track */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    cola(acc,0.10f);
    rect(cx0-6,cy-4,cw+12,8);
    glDisable(GL_BLEND);

    /* connecting line */
    col(TXT_DIM); glLineWidth(1.f);
    glBegin(GL_LINES);
        glVertex2f(cx0,cy); glVertex2f(cx1,cy);
    glEnd();

    /* tick lines */
    for(int m=0;m<N;m++){
        float mx=g_mx[t][m];
        col(TXT_DIM); glLineWidth(1.f);
        glBegin(GL_LINES);
            glVertex2f(mx,cy-10); glVertex2f(mx,cy+10);
        glEnd();
    }

    /* pile */
    int rem=cfg->num_pieces-ts->delivered_in_round;
    glColor3f(0.18f,0.12f,0.20f);
    rect(phx-2,cy-32,pw+4,4);
    draw_pile(phx+pw/2.f,cy,rem);
    col(TXT_DIM);
    txt(phx+22,cy-44,GLUT_BITMAP_HELVETICA_12,"PILE");

    /* members */
    for(int m=0;m<N;m++){
        float mx=g_mx[t][m];
        int role=(m==0)?0:(m==N-1)?2:1;
        float fatigue=(cfg->num_pieces>0)
            ?(float)ts->delivered_in_round/(float)cfg->num_pieces:0.f;
        draw_member(mx,cy,role,m,fatigue,g_elapsed);

        col(TXT_DIM);
        const char* rl=(m==0)?"src":(m==N-1)?"sink":"mid";
        int rw=0;
        for(const char* p2=rl;*p2;p2++)
            rw+=glutBitmapWidth(GLUT_BITMAP_HELVETICA_10,*p2);
        txt(mx-rw/2.f,cy-(top?40.f:24.f),GLUT_BITMAP_HELVETICA_10,rl);
    }

    /* pieces */
    draw_pieces(t);

    /* house */
    float frac=(cfg->num_pieces>0)
        ?(float)ts->delivered_in_round/(float)cfg->num_pieces:0.f;
    int won=(ts->delivered_in_round>=cfg->num_pieces);
    draw_house(hhx,cy-48.f,hw,105.f,frac,won);
    col(TXT_DIM);
    txt(hhx+hw/2.f-16,cy-64.f,GLUT_BITMAP_HELVETICA_12,"HOUSE");
}

/* ── SCOREBOARD ─────────────────────────────────────────────────────── */
static void draw_scoreboard(float y, float h){
    glColor3f(0.09f,0.05f,0.11f);
    rect(0,y,(float)g_win_w,h);

    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    cola(ACC_A,0.3f); glLineWidth(1.f);
    glBegin(GL_LINES);
        glVertex2f(0,y+h); glVertex2f((float)g_win_w,y+h);
    glEnd();
    cola(ACC_B,0.3f);
    glBegin(GL_LINES);
        glVertex2f(0,y); glVertex2f((float)g_win_w,y);
    glEnd();
    glDisable(GL_BLEND);

    const cfg_t* cfg=&g_world->cfg;
    float af=(cfg->num_pieces>0)
        ?(float)g_world->teams[0].delivered_in_round/(float)cfg->num_pieces:0;
    float bf=(cfg->num_pieces>0)
        ?(float)g_world->teams[1].delivered_in_round/(float)cfg->num_pieces:0;

    float bw=340.f, bh=13.f;
    float bx=(float)g_win_w/2.f-bw/2.f;
    float by=y+h/2.f;

    /* Team A bar */
    glColor3f(0.18f,0.10f,0.22f); rect(bx,by+4,bw,bh);
    col(ACC_A); rect(bx,by+4,bw*af,bh);
    col(TXT_HI); txt(bx-82,by+6,GLUT_BITMAP_HELVETICA_12,"TEAM A");
    txtf(bx+bw+10,by+6,GLUT_BITMAP_HELVETICA_12,
         "%d / %d",g_world->teams[0].delivered_in_round,cfg->num_pieces);

    /* Team B bar */
    glColor3f(0.18f,0.10f,0.22f); rect(bx,by-4-bh,bw,bh);
    col(ACC_B); rect(bx,by-4-bh,bw*bf,bh);
    col(TXT_HI); txt(bx-82,by-4-bh+2,GLUT_BITMAP_HELVETICA_12,"TEAM B");
    txtf(bx+bw+10,by-4-bh+2,GLUT_BITMAP_HELVETICA_12,
         "%d / %d",g_world->teams[1].delivered_in_round,cfg->num_pieces);

    /* winner */
    if(g_world->winner>=0){
        col(GOLD);
        char buf[64];
        snprintf(buf,sizeof buf,"✦  TEAM %c WINS THE COMPETITION  ✦",
                 (char)('A'+g_world->winner));
        int bw2=tw18(buf);
        txt((float)g_win_w/2.f-bw2/2.f,y+h-18,GLUT_BITMAP_HELVETICA_18,buf);
        sparkles((float)g_win_w/2.f,y+h-10,60.f,g_elapsed,12,GOLD);
    }
}

/* ── GLUT CALLBACKS ─────────────────────────────────────────────────── */

static void display(void){
    /* update wall clock for animations and header timer */
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC,&now_ts);
    g_elapsed = (now_ts.tv_sec - g_t0.tv_sec) + (now_ts.tv_nsec - g_t0.tv_nsec)/1e9;

    /* tick referee (non-blocking) */
    if(!g_world->shutting_down) referee_tick(g_world);

    /* update piece animations */
    update_pieces(0);
    update_pieces(1);

    glClear(GL_COLOR_BUFFER_BIT);
    grad_v(0,0,(float)g_win_w,(float)g_win_h,BG1,BG2);

    float header_h=52.f, sb_h=72.f;
    float avail=(float)g_win_h-header_h-sb_h;
    float lh=avail/2.f;
    draw_lane(0, sb_h+lh, lh);   /* Team A: upper */
    draw_lane(1, 0,       lh);   /* Team B: lower */
    draw_scoreboard(lh, sb_h);
    draw_header();

    glutSwapBuffers();
}

static void reshape(int w,int h){
    if(w<800) w=800;
    if(h<500) h=500;
    g_win_w=w; g_win_h=h;
    glViewport(0,0,w,h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluOrtho2D(0,w,0,h);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static void timer(int v){
    (void)v;
    glutPostRedisplay();
    if(g_world->shutting_down&&g_world->winner>=0){
        static int cd=90;
        if(--cd<=0){ referee_shutdown(g_world); glutLeaveMainLoop(); return; }
    }
    glutTimerFunc(16,timer,0);
}

static void keyboard(unsigned char k,int x,int y){
    (void)x;(void)y;
    if(k==27||k=='q'||k=='Q'){
        referee_shutdown(g_world); glutLeaveMainLoop();
    }
}

void gui_run(int* argc,char** argv,world_t* world){
    g_world=world;
    clock_gettime(CLOCK_MONOTONIC,&g_t0);
    memset(g_pa,0,sizeof g_pa);
    memset(g_mx,0,sizeof g_mx);
    memset(g_my,0,sizeof g_my);

    glutInit(argc,argv);
    glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGBA|GLUT_MULTISAMPLE);
    glutInitWindowSize(g_win_w,g_win_h);
    glutCreateWindow("Home Furnishing Competition  -  ENCS4330");

    glClearColor(0.18f, 0.10f, 0.20f, 1.f);   /* dark plum, but visible */
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT,GL_NICEST);
    glEnable(GL_MULTISAMPLE);
    glLineWidth(1.f);

    /* set up projection BEFORE first display() call, so the window is
     * not blank if the simulation is in a slow pause */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    gluOrtho2D(0, g_win_w, 0, g_win_h);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(0,timer,0);
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE,GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutMainLoop();
}

#else  /* !HAVE_GL */
void gui_run(int* argc,char** argv,world_t* world){
    (void)argc;(void)argv;
    fprintf(stderr,"[gui] OpenGL not available, falling back to headless.\n");
    headless_run(world);
}
#endif /* HAVE_GL */

/* ── HEADLESS FALLBACK ──────────────────────────────────────────────── */
void headless_run(world_t* world){
    int dbg=getenv("FURN_TRACE")!=NULL;
    int ticks=0;
    while(!world->shutting_down){
        referee_tick(world);
        if(dbg&&(ticks%30)==0)
            fprintf(stderr,"[parent] tick=%d A=%d/%d B=%d/%d round=%d wins=%d/%d\n",
                    ticks,
                    world->teams[0].delivered_in_round,world->cfg.num_pieces,
                    world->teams[1].delivered_in_round,world->cfg.num_pieces,
                    world->current_round,
                    world->teams[0].wins>world->teams[1].wins
                        ?world->teams[0].wins:world->teams[1].wins,
                    world->cfg.wins_to_match);
        ticks++;
        struct timespec ts={0,30*1000000L};
        nanosleep(&ts,NULL);
    }
    for(int i=0;i<10;i++){ referee_tick(world); usleep(30000); }
    referee_shutdown(world);
}
