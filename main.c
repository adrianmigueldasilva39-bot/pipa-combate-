/*
 * PIPA COMBATE - homebrew para PS3 (PSL1GHT)
 * Inspirado em "Pipa Combate" (Maiworm Digital Strategy).
 *
 * Controles:
 *   D-pad  : move sua pipa (vermelha)
 *   X      : acelera (corte mais forte)
 *   START  : reinicia a partida
 *
 * Regra: quando a linha (cerol) de uma pipa cruza a linha da outra,
 * a outra cai. Quem estiver mais rápido corta. Se empatar, as duas caem
 * sem ponto. Quem fizer 5 cortes vence.
 *
 * Render por software em um buffer 1280x720 XRGB, exibido via RSX.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>

#define SCR_W     1280
#define SCR_H     720
#define GROUND_Y  640
#define WIN_CUTS  5
#define CB_SIZE   0x100000
#define HOST_SIZE (32 * 1024 * 1024)

/* Índices de botão do pad (ordem padrão libps3 / io/pad.h).
 * Se algum botão não responder, confira estes valores no seu io/pad.h. */
#define BT_START  3
#define BT_UP     4
#define BT_RIGHT  5
#define BT_DOWN   6
#define BT_LEFT   7
#define BT_CROSS  14

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

typedef struct {
    float x, y, vx, vy;
    float base_x, base_y;   /* posição do cabo (na mão do jogador) */
    int   alive;
    int   timer;            /* frames até reviver quando cai */
    int   cuts;             /* cortes feitos */
    u32   color;
    int   is_player;
} Kite;

/* ---------- Vídeo / RSX ---------- */
static gcmContextData *ctx;
static void *bufs[2];
static u32   offs[2];
static int   cur = 0;
static u32  *fb;

/* ---------- Estado do jogo ---------- */
static padData pad;
static Kite  P, C;
static int   wind_t = 0;
static int   end_timer = 0;
static int   winner = -1;   /* -1 = jogando, 0 = jogador, 1 = CPU */

/* ---------- Utilitários ---------- */
static float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline void put(int x, int y, u32 c)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
        fb[y * SCR_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, u32 c)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            put(i, j, c);
}

/* Linha grossa por amostragem */
static void draw_line(float x0, float y0, float x1, float y1, int t, u32 c)
{
    float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int steps = (int)len + 1;
    int s, dx, dy;
    for (s = 0; s <= steps; s++) {
        float k = (float)s / steps;
        int x = (int)(x0 + (x1 - x0) * k);
        int y = (int)(y0 + (y1 - y0) * k);
        for (dy = -t / 2; dy <= t / 2; dy++)
            for (dx = -t / 2; dx <= t / 2; dx++)
                put(x + dx, y + dy, c);
    }
}

/* Triângulo preenchido (scanline) */
static void fill_tri(float ax, float ay, float bx, float by,
                     float cx, float cy, u32 c)
{
    float xs[3] = { ax, bx, cx };
    float ys[3] = { ay, by, cy };
    float minY = fminf(ay, fminf(by, cy));
    float maxY = fmaxf(ay, fmaxf(by, cy));
    int y, i;

    for (y = (int)minY; y <= (int)maxY; y++) {
        float fy = y + 0.5f, lo = 1e9f, hi = -1e9f;
        for (i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            float y0 = ys[i], y1 = ys[j];
            if (y0 != y1 && fy >= fminf(y0, y1) && fy <= fmaxf(y0, y1)) {
                float t = (fy - y0) / (y1 - y0);
                float x = xs[i] + t * (xs[j] - xs[i]);
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
        }
        if (lo <= hi) {
            int x;
            for (x = (int)lo; x <= (int)hi; x++)
                put(x, y, c);
        }
    }
}

/* Interseção de segmentos AB e CD */
static int seg_cross(float ax, float ay, float bx, float by,
                     float cx, float cy, float dx, float dy)
{
    float d1 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float d2 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
    float d3 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
    float d4 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
           ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

/* ---------- Pipas ---------- */
static void init_kite(Kite *k, int is_player)
{
    memset(k, 0, sizeof(*k));
    k->is_player = is_player;
    k->base_x = is_player ? 260.0f : (float)(SCR_W - 260);
    k->base_y = 300.0f;
    k->x = k->base_x;
    k->y = k->base_y;
    k->alive = 1;
    k->color = is_player ? RGB(230, 50, 50) : RGB(50, 110, 230);
}

static void reset_all(void)
{
    init_kite(&P, 1);
    init_kite(&C, 0);
    winner = -1;
    end_timer = 0;
}

static void kill_kite(Kite *k)
{
    if (!k->alive) return;
    k->alive = 0;
    k->timer = 150;
    k->vy = -3.0f;
    k->vx = k->is_player ? -2.0f : 2.0f;
}

static void move_kite(Kite *k)
{
    k->vx *= 0.9f;
    k->vy *= 0.9f;
    k->vx += sinf(wind_t * 0.013f) * 0.05f;   /* vento */
    k->x += k->vx;
    k->y += k->vy;
    k->x = clampf(k->x, 60.0f, SCR_W - 60.0f);
    k->y = clampf(k->y, 60.0f, GROUND_Y - 60.0f);
}

static void update_player(Kite *k)
{
    float acc = pad.button[BT_CROSS] ? 1.1f : 0.5f;
    if (pad.button[BT_LEFT])  k->vx -= acc;
    if (pad.button[BT_RIGHT]) k->vx += acc;
    if (pad.button[BT_UP])    k->vy -= acc;
    if (pad.button[BT_DOWN])  k->vy += acc;
}

/* CPU: tenta ficar do outro lado da pipa do jogador para cortar a linha */
static void update_cpu(Kite *k, const Kite *t)
{
    float dx = (t->x + 50.0f) - k->x;
    float dy = (t->y - 40.0f) - k->y;
    k->vx += clampf(dx * 0.004f, -0.35f, 0.35f);
    k->vy += clampf(dy * 0.004f, -0.35f, 0.35f);
    if ((wind_t % 90) == 0)
        k->vx += (float)(rand() % 3 - 1) * 1.5f;   /* rajada de ataque */
}

static void update_dead(Kite *k)
{
    k->vy += 0.35f;
    k->vx *= 0.96f;
    k->x += k->vx;
    k->y += k->vy;
    if (k->y > GROUND_Y - 30) {
        k->y = GROUND_Y - 30;
        k->vy = 0;
    }
    if (--k->timer <= 0) {
        k->alive = 1;
        k->x = k->base_x;
        k->y = k->base_y;
        k->vx = k->vy = 0;
    }
}

/* Corte: a linha de uma pipa cruza a da outra */
static void check_cut(void)
{
    float sp_p, sp_c;
    if (!P.alive || !C.alive) return;
    if (!seg_cross(P.x, P.y, P.base_x, GROUND_Y,
                   C.x, C.y, C.base_x, GROUND_Y))
        return;

    sp_p = sqrtf(P.vx * P.vx + P.vy * P.vy);
    sp_c = sqrtf(C.vx * C.vx + C.vy * C.vy);

    if (sp_p > sp_c + 0.2f) {
        kill_kite(&C);
        P.cuts++;
    } else if (sp_c > sp_p + 0.2f) {
        kill_kite(&P);
        C.cuts++;
    } else {                    /* empate: as duas caem, sem ponto */
        kill_kite(&P);
        kill_kite(&C);
    }

    if (P.cuts >= WIN_CUTS) { winner = 0; end_timer = 180; }
    else if (C.cuts >= WIN_CUTS) { winner = 1; end_timer = 180; }
}

/* ---------- Lógica ---------- */
static void update(void)
{
    wind_t++;

    if (pad.button[BT_START]) {
        reset_all();
        return;
    }

    if (winner >= 0) {
        if (--end_timer <= 0) reset_all();
        return;
    }

    if (P.alive) { update_player(&P); move_kite(&P); }
    else         update_dead(&P);

    if (C.alive) { update_cpu(&C, &P); move_kite(&C); }
    else         update_dead(&C);

    check_cut();
}

/* ---------- Desenho ---------- */
static void draw_kite(const Kite *k)
{
    float s = 34.0f;
    float x = k->x, y = k->y;
    u32 body = k->alive ? k->color : RGB(120, 120, 120);

    if (k->alive)
        draw_line(x, y, k->base_x, GROUND_Y, 2, RGB(250, 250, 250)); /* cerol */

    fill_tri(x, y - s, x + s * 0.7f, y, x, y + s, body);
    fill_tri(x, y - s, x - s * 0.7f, y, x, y + s, body);
    draw_line(x, y - s, x, y + s, 2, RGB(255, 255, 255));
    draw_line(x - s * 0.7f, y, x + s * 0.7f, y, 2, RGB(255, 255, 255));
    draw_line(x, y + s, x - 6, y + s + 60, 2, RGB(240, 240, 240)); /* rabiola */
}

static void render(void)
{
    int x, y;

    /* Céu de pôr do sol */
    for (y = 0; y < SCR_H; y++) {
        float t = (float)y / SCR_H;
        u32 c = RGB((u32)(70 + 185 * t), (u32)(110 + 60 * t), (u32)(200 - 120 * t));
        u32 *row = fb + y * SCR_W;
        for (x = 0; x < SCR_W; x++)
            row[x] = c;
    }

    /* Pão de Açúcar, morros e chão */
    fill_tri(-100, GROUND_Y, 420, 300, 900, GROUND_Y, RGB(110, 95, 95));
    fill_tri(0, GROUND_Y, 380, GROUND_Y - 90, 800, GROUND_Y, RGB(70, 140, 70));
    fill_tri(700, GROUND_Y, 1040, GROUND_Y - 110, SCR_W + 200, GROUND_Y, RGB(60, 125, 60));
    fill_rect(0, GROUND_Y, SCR_W, SCR_H - GROUND_Y, RGB(55, 120, 55));

    /* Cabos (mãos) */
    fill_rect((int)C.base_x - 6, GROUND_Y - 6, 12, 12, C.color);
    fill_rect((int)P.base_x - 6, GROUND_Y - 6, 12, 12, P.color);

    draw_kite(&C);
    draw_kite(&P);

    /* HUD: cortes de cada lado */
    for (x = 0; x < P.cuts; x++)
        fill_rect(30 + x * 40, 20, 30, 16, P.color);
    for (x = 0; x < C.cuts; x++)
        fill_rect(SCR_W - 60 - x * 40, 20, 30, 16, C.color);

    /* Faixa de vitória */
    if (winner >= 0)
        fill_rect(SCR_W / 2 - 200, SCR_H / 2 - 40, 400, 80,
                  winner == 0 ? P.color : C.color);
}

/* ---------- Vídeo: inicialização e flip ---------- */
static void video_init(void)
{
    videoState state;
    videoConfiguration vc;
    void *host;
    int i;

    videoGetState(0, 0, &state);

    memset(&vc, 0, sizeof(vc));
    vc.resolution = VIDEO_RESOLUTION_720;
    vc.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch      = SCR_W * 4;
    vc.aspect     = VIDEO_ASPECT_AUTO;
    videoConfigure(0, &vc, NULL, 0);
    videoGetState(0, 0, &state);

    host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&ctx, CB_SIZE, HOST_SIZE, host);

    for (i = 0; i < 2; i++) {
        bufs[i] = rsxMemalign(64, SCR_W * SCR_H * 4);
        rsxAddressToOffset(bufs[i], &offs[i]);
        gcmSetDisplayBuffer(i, offs[i], SCR_W * 4, SCR_W, SCR_H);
    }
    fb = bufs[cur];
}

static void flip(void)
{
    gcmSetFlip(ctx, cur);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);       /* sincroniza com o vsync */
    cur ^= 1;
    fb = bufs[cur];
}

/* ---------- Main ---------- */
int main(void)
{
    video_init();
    ioPadInit(7);
    reset_all();

    while (1) {
        ioPadGetData(0, &pad);
        update();
        render();
        flip();
    }

    return 0;
}
