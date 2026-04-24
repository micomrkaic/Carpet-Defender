/* ====================================================================
   housekeeper.c  –  "The Housekeeper"   (Space Invaders variant)
   C17 + SDL2.  Identical native and WASM builds.

   NATIVE:  gcc -std=c17 -O2 -Wall -o housekeeper housekeeper.c \
                $(sdl2-config --cflags --libs) -lm
   WASM:    emcc -std=c17 -O2 -o index.html housekeeper.c \
                -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH=1
   SERVE:   python3 -m http.server 8000

   Controls:  ← / →  or  A / D   move
              SPACE              shoot
              R / Enter          back to difficulty select
   Select screen:  ↑ ↓  or  1 2 3 4  then Enter / Space
   ==================================================================== */

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif
#include <SDL2/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── screen & layout ─────────────────────────────────────── */
#define SW         900
#define SH         650
#define HUD_H       40
#define CARPET_Y   550

/* ─── invader grid ────────────────────────────────────────── */
#define GCOLS       10
#define GROWS        4
#define CW          56
#define CH          46
#define GX         ((SW - GCOLS*CW)/2)
#define GY         (HUD_H + 16)

/* ─── gameplay constants ──────────────────────────────────── */
#define MAX_PROJ    64
#define MAX_SPLATS 200
#define N_BARS       4
#define BAR_HP_MAX   4
#define MAX_SOIL    30
#define INIT_LIVES   3
#define PLAYER_SPD 280.f
#define BULLET_SPD 480.f
#define DROP_SPD   155.f
#define MIN_MS     100u

/* ─── game states ─────────────────────────────────────────── */
#define ST_SELECT   2
#define ST_PLAY     0
#define ST_WIN      1
#define ST_LOSE    (-1)

/* ─── color helper ────────────────────────────────────────── */
#define RGB(r,gv,b) SDL_SetRenderDrawColor(ren,(r),(gv),(b),255)

/* ====================================================================
   DIFFICULTY TABLE
   ==================================================================== */
typedef struct {
    const char *name;
    const char *desc[2];
    int         max_bullets;
    Uint32      base_ms;
    Uint32      reload_ms;
} DiffRow;

static const DiffRow DIFFS[4] = {
    {
        "EASY",
        {"3 BULLETS SIMULTANEOUSLY", "SLOW INVADERS - NO RELOAD WAIT"},
        3, 1200, 0
    },
    {
        "MEDIUM",
        {"2 BULLETS AT ONCE", "250MS RELOAD BETWEEN SHOTS"},
        2, 900, 250
    },
    {
        "HARD",
        {"1 BULLET AT A TIME", "600MS RELOAD BETWEEN SHOTS"},
        1, 900, 600
    },
    {
        "NIGHTMARE",
        {"1 BULLET AT A TIME", "FAST INVADERS - 1.2S RELOAD"},
        1, 650, 1200
    },
};

static const SDL_Color DIFF_COL[4] = {
    {0x30,0xD0,0x60,255},
    {0x40,0xA0,0xFF,255},
    {0xFF,0xA0,0x20,255},
    {0xFF,0x30,0x30,255},
};

/* ====================================================================
   AUDIO  –  procedural 8-bit SFX, SDL_QueueAudio (no callback)
   Works on native and WASM (Emscripten SDL2 port uses Web Audio API).
   ==================================================================== */
#define SR        22050
#define MAX_AMP   12000
#define SCRATCH_S (SR * 4)   /* 4-second scratch buffer */

typedef enum {
    SFX_SHOOT = 0,
    SFX_FAT,
    SFX_DRIP,
    SFX_LIFE,
    SFX_START,
    SFX_WIN,
    SFX_LOSE,
    SFX_COUNT
} SfxId;

typedef struct { int16_t *buf; int n; } Sfx;
static Sfx               sfx[SFX_COUNT];
static SDL_AudioDeviceID audio_dev = 0;

/* Square-wave sweep: f0→f1 over ms milliseconds */
static int seg_sq(int16_t *b, float f0, float f1, int ms, int16_t amp) {
    int   n  = SR * ms / 1000;
    float ph = 0.f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float f = f0 + (f1 - f0) * t;
        ph += f / (float)SR;
        if (ph >= 1.f) ph -= 1.f;
        float env = 1.f - t * 0.75f;
        b[i] = (int16_t)((float)amp * env * (ph < 0.5f ? 1.f : -1.f));
    }
    return n;
}

/* White noise burst with decay */
static int seg_noise(int16_t *b, int ms, int16_t amp) {
    int      n = SR * ms / 1000;
    uint32_t s = 0xDEAD1234u;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int32_t v = (int32_t)(s >> 16) - 32768;
        b[i] = (int16_t)((int32_t)((float)amp * (1.f - t)) * v / 32767);
    }
    return n;
}

/* Silence */
static int seg_sil(int16_t *b, int ms) {
    int n = SR * ms / 1000;
    memset(b, 0, (size_t)n * sizeof(int16_t));
    return n;
}

static void bake(int16_t *tmp, int n, SfxId id) {
    sfx[id].n   = n;
    sfx[id].buf = malloc((size_t)n * sizeof(int16_t));
    if (sfx[id].buf) memcpy(sfx[id].buf, tmp, (size_t)n * sizeof(int16_t));
}

static void init_audio(void) {
    SDL_AudioSpec want = {
        .freq     = SR,
        .format   = AUDIO_S16SYS,
        .channels = 1,
        .samples  = 512,
        .callback = NULL,
    };
    SDL_AudioSpec got;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!audio_dev) { SDL_Log("Audio unavailable: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(audio_dev, 0);

    int16_t *t = malloc((size_t)SCRATCH_S * sizeof(int16_t));
    if (!t) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; return; }

    int n;

    /* SHOOT – high-to-low "pew" */
    n  = seg_sq(t, 620, 110, 65, MAX_AMP);
    bake(t, n, SFX_SHOOT);

    /* FAT BLOB lands – noise thud + low rumble */
    n  = seg_noise(t, 55, MAX_AMP);
    n += seg_sq(t+n, 110, 45, 90, MAX_AMP*3/4);
    bake(t, n, SFX_FAT);

    /* MARTINI DRIP – liquid tinkle */
    n  = seg_sq(t,   520, 220, 45, MAX_AMP);
    n += seg_sq(t+n, 220,  90, 55, MAX_AMP/2);
    bake(t, n, SFX_DRIP);

    /* LOSE A LIFE – descending arpeggio wail */
    {
        static const float nf[] = {440.f, 330.f, 220.f, 165.f};
        static const int   nd[] = {100, 110, 130, 200};
        n = 0;
        for (int i = 0; i < 4; i++)
            n += seg_sq(t+n, nf[i], nf[i]*0.78f, nd[i], MAX_AMP);
        bake(t, n, SFX_LIFE);
    }

    /* GAME START – ascending power-up fanfare */
    {
        static const float nf[] = {262.f,330.f,392.f,523.f,659.f,784.f};
        static const int   nd[] = {70,70,70,70,70,280};
        n = 0;
        for (int i = 0; i < 6; i++) {
            n += seg_sq(t+n, nf[i], nf[i], nd[i], MAX_AMP);
            n += seg_sil(t+n, 12);
        }
        bake(t, n, SFX_START);
    }

    /* GAME WIN – happy victory sting */
    {
        static const float nf[] = {523.f,659.f,784.f,659.f,784.f,1047.f};
        static const int   nd[] = {90,90,90,90,90,380};
        n = 0;
        for (int i = 0; i < 6; i++) {
            n += seg_sq(t+n, nf[i], nf[i], nd[i], MAX_AMP);
            n += seg_sil(t+n, 18);
        }
        bake(t, n, SFX_WIN);
    }

    /* GAME LOSE – sad descending wail */
    {
        static const float nf[] = {392.f,330.f,262.f,196.f};
        static const int   nd[] = {140,180,240,480};
        n = 0;
        for (int i = 0; i < 4; i++)
            n += seg_sq(t+n, nf[i], nf[i]*0.68f, nd[i], MAX_AMP);
        bake(t, n, SFX_LOSE);
    }

    free(t);
}

/* priority=1 clears the audio queue first (important one-shot sounds) */
static void play_sfx(SfxId id, int priority) {
    if (!audio_dev || !sfx[id].buf) return;
    if (priority) {
        SDL_ClearQueuedAudio(audio_dev);
    } else {
        /* skip soft sounds if > 0.25 s already queued */
        if (SDL_GetQueuedAudioSize(audio_dev) > (Uint32)(SR * sizeof(int16_t) / 4))
            return;
    }
    SDL_QueueAudio(audio_dev, sfx[id].buf, (Uint32)((size_t)sfx[id].n * sizeof(int16_t)));
}

static void free_audio(void) {
    if (audio_dev) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; }
    for (int i = 0; i < SFX_COUNT; i++) { free(sfx[i].buf); sfx[i].buf = NULL; }
}

/* ====================================================================
   TYPES
   ==================================================================== */
typedef enum  { T_BURGER, T_MARTINI } InvT;

typedef struct { float x, y; int alive; InvT type;              } Invader;
typedef struct { float x, y, vy; int active, is_bullet; InvT dtype; } Proj;
typedef struct { int x, y, hp;                                  } Bar;
typedef struct { int x, y; InvT type;                           } Splat;

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;

    /* preserved across init_game */
    int    difficulty;
    int    sel_diff;

    /* per-difficulty */
    int    max_bullets;
    Uint32 base_step_ms;
    Uint32 reload_ms;
    Uint32 last_shot;

    /* invader grid */
    Invader inv[GROWS][GCOLS];
    int     alive_count;
    float   gox, goy;
    int     gdir;
    Uint32  last_step;
    Uint32  step_ms;

    Proj   proj[MAX_PROJ];
    Bar    bar[N_BARS];
    Splat  splat[MAX_SPLATS];
    int    n_splat;

    float  px, py;
    int    pdead;
    int    lives;
    int    score;

    int    state;
    int    running;
    Uint32 prev;
} G;

static G g;

/* ====================================================================
   5×7 BITMAP FONT  (ASCII 0x20..0x5A)
   ==================================================================== */
static const uint8_t FONT[0x5B][7] = {
[0x20]={0,0,0,0,0,0,0},
[0x21]={4,4,4,4,0,4,0},
[0x2D]={0,0,31,0,0,0,0},
[0x3A]={0,12,12,0,12,12,0},
[0x30]={14,17,19,21,25,17,14},
[0x31]={4,12,4,4,4,4,14},
[0x32]={14,17,1,6,8,16,31},
[0x33]={31,2,4,2,1,17,14},
[0x34]={2,6,10,18,31,2,2},
[0x35]={31,16,30,1,1,17,14},
[0x36]={6,8,16,30,17,17,14},
[0x37]={31,1,2,4,8,8,8},
[0x38]={14,17,17,14,17,17,14},
[0x39]={14,17,17,15,1,2,12},
[0x41]={4,10,17,17,31,17,17},
[0x42]={30,17,17,30,17,17,30},
[0x43]={14,17,16,16,16,17,14},
[0x44]={28,18,17,17,17,18,28},
[0x45]={31,16,16,30,16,16,31},
[0x46]={31,16,16,30,16,16,16},
[0x47]={14,17,16,23,17,17,15},
[0x48]={17,17,17,31,17,17,17},
[0x49]={14,4,4,4,4,4,14},
[0x4A]={7,2,2,2,2,18,12},
[0x4B]={17,18,20,24,20,18,17},
[0x4C]={16,16,16,16,16,16,31},
[0x4D]={17,27,21,17,17,17,17},
[0x4E]={17,25,21,19,17,17,17},
[0x4F]={14,17,17,17,17,17,14},
[0x50]={30,17,17,30,16,16,16},
[0x51]={14,17,17,17,21,18,13},
[0x52]={30,17,17,30,20,18,17},
[0x53]={14,17,16,14,1,17,14},
[0x54]={31,4,4,4,4,4,4},
[0x55]={17,17,17,17,17,17,14},
[0x56]={17,17,17,17,17,10,4},
[0x57]={17,17,17,21,21,27,17},
[0x58]={17,17,10,4,10,17,17},
[0x59]={17,17,10,4,4,4,4},
[0x5A]={31,1,2,4,8,16,31},
};

/* ====================================================================
   DRAWING PRIMITIVES
   ==================================================================== */
static void fill_circle(SDL_Renderer *ren, int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r*r - dy*dy));
        SDL_RenderDrawLine(ren, cx-dx, cy+dy, cx+dx, cy+dy);
    }
}

static void draw_char(SDL_Renderer *ren, int x, int y, uint8_t c, int s) {
    if (c < 0x20 || c >= 0x5B) c = 0x20;
    const uint8_t *row = FONT[c];
    for (int r = 0; r < 7; r++)
        for (int co = 0; co < 5; co++)
            if (row[r] & (uint8_t)(0x10u >> co)) {
                SDL_Rect p = {x + co*s, y + r*s, s, s};
                SDL_RenderFillRect(ren, &p);
            }
}

static void draw_str(SDL_Renderer *ren, int x, int y, const char *s, int sc) {
    for (; *s; s++, x += 6*sc) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        draw_char(ren, x, y, c, sc);
    }
}

static void draw_strf(SDL_Renderer *ren, int x, int y, int sc,
                      const char *fmt, ...) {
    char buf[64];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    draw_str(ren, x, y, buf, sc);
}

static int str_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }

static void draw_cx(SDL_Renderer *ren, int cx, int y, const char *s, int sc) {
    draw_str(ren, cx - str_w(s,sc)/2, y, s, sc);
}

/* ====================================================================
   ENTITY DRAWING
   ==================================================================== */
static void draw_burger(SDL_Renderer *ren, int cx, int cy) {
    RGB(0xD4,0x7C,0x1E);
    for (int dy=-11; dy<=0; dy++) {
        float t=(float)(dy+11)/11.f;
        int hw=(int)(16.f*sqrtf(t));
        SDL_RenderDrawLine(ren,cx-hw,cy+dy,cx+hw,cy+dy);
    }
    RGB(0xF5,0xEE,0xD0);
    SDL_Rect sd1={cx-7,cy-9,3,2},sd2={cx+3,cy-10,3,2},sd3={cx-2,cy-10,3,2};
    SDL_RenderFillRect(ren,&sd1);SDL_RenderFillRect(ren,&sd2);SDL_RenderFillRect(ren,&sd3);
    RGB(0xF0,0xCC,0x30);
    SDL_Rect ch={cx-15,cy+1,30,5}; SDL_RenderFillRect(ren,&ch);
    RGB(0xD0,0xA8,0x20);
    SDL_Rect fold={cx-15,cy+1,6,5}; SDL_RenderFillRect(ren,&fold);
    RGB(0x52,0x26,0x06);
    SDL_Rect pt={cx-14,cy+6,28,7}; SDL_RenderFillRect(ren,&pt);
    RGB(0x70,0x38,0x10);
    SDL_Rect ph={cx-12,cy+7,10,2}; SDL_RenderFillRect(ren,&ph);
    RGB(0x22,0x88,0x22);
    for (int i=-14;i<=10;i+=4){int z=((i/4)&1)?2:0;SDL_Rect lt={cx+i,cy+13+z,4,3};SDL_RenderFillRect(ren,&lt);}
    RGB(0xB8,0x64,0x10);
    for (int dy=0;dy<=7;dy++){float t=(float)(7-dy)/7.f;int hw=(int)(14.f*sqrtf(1.f-t*t)+2);SDL_RenderDrawLine(ren,cx-hw,cy+16+dy,cx+hw,cy+16+dy);}
}

static void draw_martini(SDL_Renderer *ren, int cx, int cy) {
    const int TW=17;
    for (int dy=0;dy<=20;dy++){float t=(float)dy/20.f;int hw=(int)(TW*(1.f-t));if(dy<3){RGB(0x70,0xE0,0xFF);}else{RGB(0x28,0xA0,0xD0);}SDL_RenderDrawLine(ren,cx-hw,cy-13+dy,cx+hw,cy-13+dy);}
    RGB(0xC0,0xF0,0xFF);
    SDL_RenderDrawLine(ren,cx-TW,cy-13,cx,cy+7);
    SDL_RenderDrawLine(ren,cx+TW,cy-13,cx,cy+7);
    SDL_RenderDrawLine(ren,cx-TW,cy-13,cx+TW,cy-13);
    RGB(0xB0,0xE8,0xF8);
    SDL_RenderDrawLine(ren,cx,cy+7,cx,cy+17);
    SDL_RenderDrawLine(ren,cx-9,cy+17,cx+9,cy+17);
    RGB(0x30,0x90,0x20); fill_circle(ren,cx-4,cy-8,3);
    RGB(0xE0,0x30,0x30); fill_circle(ren,cx-4,cy-8,1);
    RGB(0xFF,0xCC,0x40); SDL_RenderDrawLine(ren,cx-4,cy-11,cx+6,cy-5);
}

static void draw_housekeeper(SDL_Renderer *ren, int cx, int cy) {
    if (g.pdead > 0 && (g.pdead/4)%2 == 0) return;
    RGB(0x18,0x18,0x28); SDL_Rect body={cx-9,cy-10,18,24}; SDL_RenderFillRect(ren,&body);
    RGB(0xF0,0xF0,0xF0);
    SDL_Rect bib={cx-4,cy-12,8,8}; SDL_RenderFillRect(ren,&bib);
    SDL_Rect skirt={cx-5,cy-2,10,14}; SDL_RenderFillRect(ren,&skirt);
    RGB(0xF8,0xD0,0xD0);
    SDL_Rect b1={cx-7,cy-3,5,4},b2={cx+2,cy-3,5,4};
    SDL_RenderFillRect(ren,&b1); SDL_RenderFillRect(ren,&b2);
    RGB(0xE8,0xC8,0xA0); fill_circle(ren,cx,cy-18,7);
    RGB(0xF0,0xF0,0xF0); SDL_Rect cap={cx-7,cy-27,14,10}; SDL_RenderFillRect(ren,&cap);
    RGB(0x18,0x18,0x28); SDL_Rect cb={cx-7,cy-19,14,3}; SDL_RenderFillRect(ren,&cb);
    RGB(0x10,0x10,0x10);
    SDL_Rect el={cx-4,cy-20,2,2},er={cx+2,cy-20,2,2};
    SDL_RenderFillRect(ren,&el); SDL_RenderFillRect(ren,&er);
    RGB(0x80,0xC0,0xE0); SDL_Rect sb={cx+9,cy-8,5,10}; SDL_RenderFillRect(ren,&sb);
    RGB(0xA0,0xD8,0xF0); SDL_Rect st={cx+10,cy-14,3,7}; SDL_RenderFillRect(ren,&st);
    RGB(0x60,0xA0,0xC0); SDL_RenderDrawLine(ren,cx+11,cy-14,cx+16,cy-19);
    RGB(0xE8,0xC8,0xA0);
    SDL_RenderDrawLine(ren,cx+8,cy-4,cx+9,cy-8);
    SDL_RenderDrawLine(ren,cx-8,cy-4,cx-12,cy+2);
}

static void draw_barricade(SDL_Renderer *ren, int cx, int cy, int hp) {
    if (hp<=0) return;
    const int BW=36,BH=18;
    RGB(0xC8,0x82,0x30); SDL_Rect body={cx-BW,cy-BH/2,BW*2,BH}; SDL_RenderFillRect(ren,&body);
    RGB(0xA0,0x66,0x20);
    SDL_RenderDrawLine(ren,cx-BW,cy-3,cx+BW,cy-3);
    SDL_RenderDrawLine(ren,cx-BW,cy+3,cx+BW,cy+3);
    SDL_RenderDrawLine(ren,cx,cy-BH/2,cx,cy+BH/2);
    RGB(0xE8,0xD0,0x70);
    SDL_Rect p1={cx-16,cy-2,4,4},p2={cx+12,cy-2,4,4};
    SDL_RenderFillRect(ren,&p1); SDL_RenderFillRect(ren,&p2);
    if (hp>=2){RGB(0xA8,0x68,0x24);int ly=cy+BH/2;SDL_RenderDrawLine(ren,cx-BW+5,ly,cx-BW+3,ly+14);SDL_RenderDrawLine(ren,cx+BW-5,ly,cx+BW-3,ly+14);}
    if (hp>=3){RGB(0xA8,0x68,0x24);int ly=cy+BH/2;SDL_RenderDrawLine(ren,cx-10,ly,cx-11,ly+14);SDL_RenderDrawLine(ren,cx+10,ly,cx+11,ly+14);}
    SDL_SetRenderDrawColor(ren,0x0A,0x0A,0x14,255);
    if (hp<=2){SDL_Rect n1={cx-BW,cy-BH/2,12,9}; SDL_RenderFillRect(ren,&n1);}
    if (hp==1){SDL_Rect n2={cx+BW-12,cy+1,12,8};  SDL_RenderFillRect(ren,&n2);}
}

static void draw_bullet(SDL_Renderer *ren, int cx, int cy) {
    RGB(0xFF,0xFF,0x60); SDL_Rect r={cx-2,cy-6,4,12}; SDL_RenderFillRect(ren,&r);
    RGB(0xFF,0xFF,0xFF); SDL_Rect hi={cx-1,cy-6,2,4}; SDL_RenderFillRect(ren,&hi);
}

static void draw_fat_blob(SDL_Renderer *ren, int cx, int cy) {
    RGB(0xD8,0xB8,0x18); fill_circle(ren,cx,cy,5);
    RGB(0xFF,0xE8,0x60); fill_circle(ren,cx-1,cy-1,2);
}

static void draw_martini_drip(SDL_Renderer *ren, int cx, int cy) {
    RGB(0x20,0xB8,0xE0); fill_circle(ren,cx,cy,4);
    RGB(0x80,0xE8,0xFF); fill_circle(ren,cx,cy-2,2);
    RGB(0x20,0xB8,0xE0); SDL_RenderDrawLine(ren,cx,cy+4,cx,cy+8);
}

static void draw_splat(SDL_Renderer *ren, int sx, int sy, InvT type) {
    if (type==T_BURGER){RGB(0xA8,0x88,0x10);}else{RGB(0x18,0x88,0xB0);}
    fill_circle(ren,sx,sy,5);
    for (int a=0;a<360;a+=60){float rad=(float)a*3.14159f/180.f;int ex=sx+(int)(8.f*cosf(rad));int ey=sy+(int)(5.f*sinf(rad));fill_circle(ren,ex,ey,2);}
}

static void draw_soil_meter(SDL_Renderer *ren) {
    int pct=(g.n_splat*100)/MAX_SOIL; if(pct>100)pct=100;
    int bh=SH-HUD_H-20, filled=bh*pct/100;
    RGB(0x18,0x18,0x18); SDL_Rect bg={8,HUD_H+10,14,bh}; SDL_RenderFillRect(ren,&bg);
    if(pct<50){RGB(0x30,0xA8,0x30);}else if(pct<80){RGB(0xD0,0xA0,0x20);}else{RGB(0xD0,0x30,0x30);}
    if(filled>0){SDL_Rect f={9,HUD_H+10+bh-filled,12,filled};SDL_RenderFillRect(ren,&f);}
    RGB(0x80,0x80,0x80); SDL_RenderDrawRect(ren,&bg);
    draw_str(ren,6,HUD_H+bh+14,"SOIL",1);
}

/* ====================================================================
   GAME INIT
   ==================================================================== */
static void init_game(void) {
    SDL_Window   *win  = g.win;
    SDL_Renderer *ren  = g.ren;
    int           diff = g.difficulty;
    int           sel  = g.sel_diff;
    memset(&g, 0, sizeof(g));
    g.win = win; g.ren = ren;
    g.difficulty = diff; g.sel_diff = sel;

    g.max_bullets  = DIFFS[diff].max_bullets;
    g.base_step_ms = DIFFS[diff].base_ms;
    g.reload_ms    = DIFFS[diff].reload_ms;
    g.step_ms      = g.base_step_ms;

    for (int r=0;r<GROWS;r++)
        for (int c=0;c<GCOLS;c++)
            g.inv[r][c]=(Invader){
                .x=(float)(GX+c*CW+CW/2),
                .y=(float)(GY+r*CH+CH/2),
                .alive=1,
                .type=(r<2)?T_MARTINI:T_BURGER,
            };
    g.alive_count=GROWS*GCOLS;
    g.gdir=1;
    g.last_step=SDL_GetTicks();

    static const int BXS[N_BARS]={170,360,540,730};
    for (int i=0;i<N_BARS;i++) g.bar[i]=(Bar){BXS[i],CARPET_Y-80,BAR_HP_MAX};

    g.px=(float)(SW/2); g.py=(float)(CARPET_Y-16);
    g.lives=INIT_LIVES;
    g.state=ST_PLAY; g.running=1;
    g.prev=SDL_GetTicks();
}

/* ====================================================================
   INPUT
   ==================================================================== */
static void handle_event(const SDL_Event *e) {
    if (e->type==SDL_QUIT){g.running=0;return;}
    if (e->type!=SDL_KEYDOWN) return;
    SDL_Keycode k=e->key.keysym.sym;

    /* ── SELECT SCREEN ── */
    if (g.state==ST_SELECT) {
        if (k==SDLK_UP   ||k==SDLK_w){g.sel_diff=(g.sel_diff+3)%4;return;}
        if (k==SDLK_DOWN ||k==SDLK_s){g.sel_diff=(g.sel_diff+1)%4;return;}
        if (k==SDLK_1){g.sel_diff=0;}
        if (k==SDLK_2){g.sel_diff=1;}
        if (k==SDLK_3){g.sel_diff=2;}
        if (k==SDLK_4){g.sel_diff=3;}
        if (k==SDLK_RETURN||k==SDLK_KP_ENTER||k==SDLK_SPACE||
            k==SDLK_1||k==SDLK_2||k==SDLK_3||k==SDLK_4) {
            g.difficulty=g.sel_diff;
            init_game();
            play_sfx(SFX_START,1);
        }
        return;
    }

    /* ── WIN / LOSE OVERLAY → back to select ── */
    if (g.state!=ST_PLAY) {
        if (k==SDLK_r||k==SDLK_RETURN||k==SDLK_KP_ENTER) g.state=ST_SELECT;
        return;
    }

    /* ── IN GAME: fire ── */
    if (k==SDLK_SPACE && !g.pdead) {
        int cnt=0;
        for (int i=0;i<MAX_PROJ;i++)
            if (g.proj[i].active&&g.proj[i].is_bullet) cnt++;

        Uint32 now=SDL_GetTicks();
        int reload_ok=(g.reload_ms==0)||(now-g.last_shot>=g.reload_ms);

        if (cnt<g.max_bullets&&reload_ok) {
            for (int i=0;i<MAX_PROJ;i++) {
                if (!g.proj[i].active) {
                    g.proj[i]=(Proj){.active=1,.is_bullet=1,
                        .x=g.px,.y=g.py-20.f,.vy=-BULLET_SPD};
                    g.last_shot=now;
                    play_sfx(SFX_SHOOT,0);
                    break;
                }
            }
        }
    }
}

/* ====================================================================
   UPDATE
   ==================================================================== */
static void update(float dt) {
    if (g.state!=ST_PLAY) return;

    if (g.pdead>0) {
        g.pdead--;
        if (g.pdead==0) {
            if (g.lives<=0){g.state=ST_LOSE;play_sfx(SFX_LOSE,1);return;}
            g.px=(float)(SW/2);
        }
        return;
    }

    const uint8_t *ks=SDL_GetKeyboardState(NULL);
    float spd=PLAYER_SPD*dt;
    if (ks[SDL_SCANCODE_LEFT] ||ks[SDL_SCANCODE_A]) g.px-=spd;
    if (ks[SDL_SCANCODE_RIGHT]||ks[SDL_SCANCODE_D]) g.px+=spd;
    if (g.px<25.f) g.px=25.f;
    if (g.px>(float)(SW-25)) g.px=(float)(SW-25);

    Uint32 now=SDL_GetTicks();
    if (now-g.last_step>=g.step_ms) {
        g.last_step=now;

        float lx=99999.f,rx=-99999.f;
        for (int r=0;r<GROWS;r++)
            for (int c=0;c<GCOLS;c++) {
                if (!g.inv[r][c].alive) continue;
                float ax=g.inv[r][c].x+g.gox;
                if (ax<lx) lx=ax;
                if (ax>rx) rx=ax;
            }

        float step=(float)g.gdir*CW*0.55f;
        int bounce=0;
        if (g.gdir== 1&&rx+step>SW-28.f){g.gdir=-1;bounce=1;}
        if (g.gdir==-1&&lx+step<28.f)   {g.gdir= 1;bounce=1;}

        if (bounce) {
            g.goy+=20.f;
            for (int r=0;r<GROWS;r++)
                for (int c=0;c<GCOLS;c++)
                    if (g.inv[r][c].alive&&g.inv[r][c].y+g.goy>=CARPET_Y)
                        {g.state=ST_LOSE;play_sfx(SFX_LOSE,1);return;}
        } else { g.gox+=step; }

        int active_drops=0;
        for (int i=0;i<MAX_PROJ;i++)
            if (g.proj[i].active&&!g.proj[i].is_bullet) active_drops++;
        int max_drops=1+(GROWS*GCOLS-g.alive_count)/7;
        if (max_drops>5) max_drops=5;

        if (active_drops<max_drops) {
            int col=rand()%GCOLS, cr=-1;
            for (int r=GROWS-1;r>=0;r--) if (g.inv[r][col].alive){cr=r;break;}
            if (cr>=0) {
                float dx=g.inv[cr][col].x+g.gox;
                float dy=g.inv[cr][col].y+g.goy+16.f;
                for (int i=0;i<MAX_PROJ;i++) {
                    if (!g.proj[i].active) {
                        g.proj[i]=(Proj){.active=1,.is_bullet=0,
                            .x=dx,.y=dy,.vy=DROP_SPD,.dtype=g.inv[cr][col].type};
                        break;
                    }
                }
            }
        }
    }

    for (int i=0;i<MAX_PROJ;i++) {
        if (!g.proj[i].active) continue;
        g.proj[i].y+=g.proj[i].vy*dt;

        if (g.proj[i].is_bullet) {
            if (g.proj[i].y<HUD_H){g.proj[i].active=0;continue;}
            int hit=0;
            for (int r=0;r<GROWS&&!hit;r++)
                for (int c=0;c<GCOLS&&!hit;c++) {
                    if (!g.inv[r][c].alive) continue;
                    float ax=g.inv[r][c].x+g.gox,ay=g.inv[r][c].y+g.goy;
                    if (fabsf(g.proj[i].x-ax)<16.f&&fabsf(g.proj[i].y-ay)<14.f) {
                        g.inv[r][c].alive=0; g.alive_count--;
                        g.proj[i].active=0; hit=1;
                        g.score+=(g.inv[r][c].type==T_MARTINI)?20:10;
                        float frac=1.f-(float)g.alive_count/(GROWS*GCOLS);
                        Uint32 range=g.base_step_ms-MIN_MS;
                        g.step_ms=g.base_step_ms-(Uint32)((float)range*frac);
                        if (g.step_ms<MIN_MS) g.step_ms=MIN_MS;
                        if (g.alive_count==0){g.state=ST_WIN;play_sfx(SFX_WIN,1);return;}
                    }
                }
            if (!hit)
                for (int b=0;b<N_BARS;b++) {
                    if (g.bar[b].hp<=0) continue;
                    if (fabsf(g.proj[i].x-g.bar[b].x)<38.f&&fabsf(g.proj[i].y-g.bar[b].y)<14.f)
                        {g.bar[b].hp--;g.proj[i].active=0;break;}
                }
        } else {
            if (g.proj[i].y>SH){g.proj[i].active=0;continue;}
            int hit=0;
            for (int b=0;b<N_BARS&&!hit;b++) {
                if (g.bar[b].hp<=0) continue;
                if (fabsf(g.proj[i].x-g.bar[b].x)<38.f&&fabsf(g.proj[i].y-g.bar[b].y)<16.f)
                    {g.bar[b].hp--;g.proj[i].active=0;hit=1;}
            }
            if (!hit) {
                if (fabsf(g.proj[i].x-g.px)<12.f&&fabsf(g.proj[i].y-g.py)<18.f) {
                    InvT dt2=g.proj[i].dtype;
                    g.proj[i].active=0; hit=1;
                    g.lives--; g.pdead=90;
                    play_sfx(SFX_LIFE,1);
                    if (g.n_splat<MAX_SPLATS) {
                        g.splat[g.n_splat++]=(Splat){(int)g.px+(rand()%24-12),CARPET_Y+15+rand()%20,dt2};
                        if (g.n_splat>=MAX_SOIL&&g.state==ST_PLAY){g.state=ST_LOSE;play_sfx(SFX_LOSE,1);}
                    }
                }
            }
            if (!hit&&g.proj[i].y>=CARPET_Y) {
                InvT dt2=g.proj[i].dtype;
                g.proj[i].active=0;
                play_sfx(dt2==T_BURGER?SFX_FAT:SFX_DRIP,0);
                if (g.n_splat<MAX_SPLATS) {
                    g.splat[g.n_splat++]=(Splat){(int)g.proj[i].x+(rand()%8-4),CARPET_Y+8+rand()%25,dt2};
                    if (g.n_splat>=MAX_SOIL&&g.state==ST_PLAY){g.state=ST_LOSE;play_sfx(SFX_LOSE,1);}
                }
            }
        }
    }
}

/* ====================================================================
   RENDER – SELECT SCREEN
   ==================================================================== */
static void render_select(void) {
    SDL_Renderer *ren=g.ren;
    RGB(0x08,0x06,0x14); SDL_RenderClear(ren);
    RGB(0x0C,0x09,0x1C);
    for (int x=40;x<SW;x+=60) SDL_RenderDrawLine(ren,x,0,x,SH);

    /* title */
    RGB(0x80,0x60,0xC8); draw_cx(ren,SW/2,42,"THE HOUSEKEEPER",3);
    RGB(0x60,0x60,0x90); draw_cx(ren,SW/2,90,"SELECT DIFFICULTY",2);

    /* preview sprites */
    draw_housekeeper(ren,90,220);
    draw_burger(ren,SW-78,200);
    draw_martini(ren,SW-78,255);

    /* option boxes */
    int bx=170,by0=128,bw=560,bh=68,gap=10;
    for (int d=0;d<4;d++) {
        int by=by0+d*(bh+gap);
        int sel=(d==g.sel_diff);
        /* background */
        if (sel) SDL_SetRenderDrawColor(ren,DIFF_COL[d].r/5,DIFF_COL[d].g/5,DIFF_COL[d].b/5,255);
        else      RGB(0x10,0x0E,0x20);
        SDL_Rect bg={bx,by,bw,bh}; SDL_RenderFillRect(ren,&bg);
        /* border */
        if (sel) SDL_SetRenderDrawColor(ren,DIFF_COL[d].r,DIFF_COL[d].g,DIFF_COL[d].b,255);
        else      RGB(0x28,0x22,0x44);
        SDL_RenderDrawRect(ren,&bg);

        /* key */
        RGB(0x70,0x70,0x90); char kb[2]={'1'+(char)d,0}; draw_str(ren,bx+10,by+10,kb,2);
        /* name */
        SDL_SetRenderDrawColor(ren,DIFF_COL[d].r,DIFF_COL[d].g,DIFF_COL[d].b,255);
        draw_str(ren,bx+36,by+10,DIFFS[d].name,2);
        /* desc */
        RGB(0x90,0x88,0xA8);
        draw_str(ren,bx+36,by+34,DIFFS[d].desc[0],1);
        draw_str(ren,bx+36,by+46,DIFFS[d].desc[1],1);
        /* arrow */
        if (sel) {
            SDL_SetRenderDrawColor(ren,DIFF_COL[d].r,DIFF_COL[d].g,DIFF_COL[d].b,255);
            int ax=bx+bw-16,ay=by+bh/2;
            SDL_RenderDrawLine(ren,ax-8,ay-7,ax,ay);
            SDL_RenderDrawLine(ren,ax-8,ay+7,ax,ay);
        }
    }

    /* footer */
    RGB(0x48,0x48,0x68);
    draw_cx(ren,SW/2,by0+4*(bh+gap)+16,"UP DOWN OR 1 2 3 4  THEN ENTER",1);
    SDL_RenderPresent(ren);
}

/* ====================================================================
   RENDER – GAME
   ==================================================================== */
static void render_game(void) {
    SDL_Renderer *ren=g.ren;
    RGB(0x0C,0x0A,0x18); SDL_RenderClear(ren);
    RGB(0x0F,0x0C,0x1E);
    for (int x=30;x<SW;x+=60) SDL_RenderDrawLine(ren,x,HUD_H,x,CARPET_Y);

    RGB(0x38,0x20,0x0C); SDL_Rect carpet={0,CARPET_Y,SW,SH-CARPET_Y}; SDL_RenderFillRect(ren,&carpet);
    RGB(0x2A,0x16,0x08);
    for (int x=0;x<SW;x+=14) SDL_RenderDrawLine(ren,x,CARPET_Y,x,SH);
    for (int y=CARPET_Y;y<SH;y+=10) SDL_RenderDrawLine(ren,0,y,SW,y);

    for (int i=0;i<g.n_splat;i++) draw_splat(ren,g.splat[i].x,g.splat[i].y,g.splat[i].type);
    for (int i=0;i<N_BARS;i++)    draw_barricade(ren,g.bar[i].x,g.bar[i].y,g.bar[i].hp);

    for (int r=0;r<GROWS;r++)
        for (int c=0;c<GCOLS;c++) {
            if (!g.inv[r][c].alive) continue;
            int ax=(int)(g.inv[r][c].x+g.gox),ay=(int)(g.inv[r][c].y+g.goy);
            if (g.inv[r][c].type==T_BURGER) draw_burger(ren,ax,ay);
            else                            draw_martini(ren,ax,ay);
        }

    for (int i=0;i<MAX_PROJ;i++) {
        if (!g.proj[i].active) continue;
        int px2=(int)g.proj[i].x,py2=(int)g.proj[i].y;
        if      (g.proj[i].is_bullet)           draw_bullet(ren,px2,py2);
        else if (g.proj[i].dtype==T_BURGER)     draw_fat_blob(ren,px2,py2);
        else                                    draw_martini_drip(ren,px2,py2);
    }
    draw_housekeeper(ren,(int)g.px,(int)g.py);

    /* HUD */
    RGB(0x06,0x06,0x10); SDL_Rect hud={0,0,SW,HUD_H}; SDL_RenderFillRect(ren,&hud);
    RGB(0x30,0x30,0x48); SDL_RenderDrawLine(ren,0,HUD_H,SW,HUD_H);
    RGB(0xE0,0xE0,0xE0);
    draw_str(ren,35,10,"SCORE:",2);
    draw_strf(ren,155,10,2,"%d",g.score);
    draw_str(ren,375,10,"LIVES:",2);
    draw_strf(ren,495,10,2,"%d",g.lives);
    for (int i=0;i<g.lives&&i<3;i++) {
        int ix=523+i*26;
        RGB(0xD4,0x7C,0x1E); SDL_Rect bn={ix,13,18,9}; SDL_RenderFillRect(ren,&bn);
        RGB(0x52,0x26,0x06); SDL_Rect pt={ix+1,19,16,5}; SDL_RenderFillRect(ren,&pt);
    }
    SDL_SetRenderDrawColor(ren,DIFF_COL[g.difficulty].r,DIFF_COL[g.difficulty].g,DIFF_COL[g.difficulty].b,255);
    draw_str(ren,632,10,DIFFS[g.difficulty].name,2);

    /* reload bar – shown only when a cooldown applies and is active */
    if (g.reload_ms>0 && !g.pdead) {
        Uint32 elapsed=SDL_GetTicks()-g.last_shot;
        if (elapsed<g.reload_ms) {
            int fw=120, filled=(int)((float)elapsed/(float)g.reload_ms*(float)fw);
            int bx2=(int)g.px-fw/2, by2=(int)g.py+20;
            RGB(0x28,0x28,0x28); SDL_Rect bg2={bx2,by2,fw,4}; SDL_RenderFillRect(ren,&bg2);
            RGB(0xFF,0xE0,0x40); SDL_Rect fg={bx2,by2,filled,4}; SDL_RenderFillRect(ren,&fg);
        }
    }

    draw_soil_meter(ren);
    RGB(0x80,0x60,0xC0); draw_str(ren,808,10,"HK",2);

    /* overlay */
    if (g.state!=ST_PLAY) {
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren,0,0,0,175);
        SDL_Rect ol={60,180,SW-120,230}; SDL_RenderFillRect(ren,&ol);
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_NONE);
        if (g.state==ST_WIN) {
            RGB(0x60,0xFF,0x80); draw_cx(ren,SW/2,218,"YOU WIN!",4);
            RGB(0xC0,0xC0,0xC0); draw_cx(ren,SW/2,298,"THE CARPET IS SPOTLESS!",2);
        } else {
            RGB(0xFF,0x40,0x40); draw_cx(ren,SW/2,218,"GAME OVER",4);
            RGB(0xC0,0xC0,0xC0); draw_cx(ren,SW/2,298,"THE CARPET IS RUINED!",2);
        }
        RGB(0xE0,0xE0,0x60); draw_strf(ren,200,330,2,"FINAL SCORE: %d",g.score);
        RGB(0xD0,0xD0,0x40); draw_cx(ren,SW/2,370,"R OR ENTER: BACK TO SELECT",2);
    }
    SDL_RenderPresent(ren);
}

/* ====================================================================
   MAIN LOOP
   ==================================================================== */
static void main_loop(void) {
    Uint32 now=SDL_GetTicks();
    float dt=(float)(now-g.prev)/1000.f;
    if (dt>0.05f) dt=0.05f;
    g.prev=now;

    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(&e);

    if (g.state==ST_SELECT) render_select();
    else { update(dt); render_game(); }
}

/* ====================================================================
   MAIN
   ==================================================================== */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)<0) {
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1;
    }
    g.win=SDL_CreateWindow("The Housekeeper",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,SW,SH,0);
    if (!g.win){fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError());SDL_Quit();return 1;}
    g.ren=SDL_CreateRenderer(g.win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!g.ren) g.ren=SDL_CreateRenderer(g.win,-1,SDL_RENDERER_SOFTWARE);
    if (!g.ren){fprintf(stderr,"SDL_CreateRenderer: %s\n",SDL_GetError());SDL_DestroyWindow(g.win);SDL_Quit();return 1;}

    srand((unsigned int)SDL_GetTicks());
    init_audio();

    g.state=ST_SELECT; g.sel_diff=0; g.running=1; g.prev=SDL_GetTicks();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(main_loop,0,1);
#else
    while (g.running) {
        Uint32 t0=SDL_GetTicks();
        main_loop();
        Uint32 el=SDL_GetTicks()-t0;
        if (el<16) SDL_Delay(16-el);
    }
    free_audio();
    SDL_DestroyRenderer(g.ren);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
#endif
    return 0;
}
