/*
 * test_vdc.c — per-quirk-tests voor de 8244/8245 VDC (QUIRKS V1..V12,
 * A1..A3). BIOS-loos en ROM-loos: alle tests programmeren de VDC
 * rechtstreeks via synthetische registerconfiguraties en vergelijken
 * pixels, collision-bits en audio-samples.
 *
 * Registerkaart-feiten: Daniel Boris, "Odyssey 2 Technical Specs V1.1"
 * (publiek; zie bronnenblok in src/vdc8244.c).
 *
 * Registratie: de integrator roept register_vdc_tests() aan vanuit
 * tests/test_main.c.
 */
#include <string.h>

#include "../src/core_internal.h"

void g7k_register_test(const char *name, int (*fn)(void));

#define CHECK(cond) do { if (!(cond)) return 1; } while (0)

/* Verwachte paletwaarden — bewust onafhankelijk gedupliceerd uit de
 * gedocumenteerde kleurtabel (fb-layout 0xAABBGGRR). */
#define C_BLACK      0xFF000000u
#define C_WHITE      0xFFFFFFFFu   /* obj-kleur 7                       */
#define C_OBJ_RED    0xFF4040FFu   /* obj-kleur 1 (LSB=rood)            */
#define C_DIM_RED    0xFF0000A0u   /* bg/grid dim-kleur 4 (LSB=blauw)   */
#define C_BRIGHT_RED 0xFF4040FFu   /* bg/grid bright-kleur 4            */
#define C_BRIGHT_BG0 0xFF404040u   /* bright "zwart" = grijs (V11)      */

static vdc8244 V;
static uint32_t FB[G7K_FB_W * G7K_FB_H];

static void vreset(void)
{
    memset(FB, 0, sizeof(FB));
    vdc8244_init(&V, G7K_REGION_NTSC, FB, G7K_FB_W, G7K_FB_H);
}

static void run_lines(int from, int to)
{
    int l;
    for (l = from; l <= to; l++) {
        vdc8244_begin_line(&V, l);
        vdc8244_render_line(&V);
    }
}

static uint32_t px(int x, int y)
{
    return FB[(size_t)y * G7K_FB_W + (size_t)x];
}

/* sprite n op (x,y), kleur, ctl-extra's; shape = 8 bytes */
static void set_sprite(int n, uint8_t y, uint8_t x, uint8_t ctl,
                       const uint8_t *shape)
{
    int i;
    vdc8244_write(&V, (uint8_t)(4 * n + 0), y);
    vdc8244_write(&V, (uint8_t)(4 * n + 1), x);
    vdc8244_write(&V, (uint8_t)(4 * n + 2), ctl);
    for (i = 0; i < 8; i++)
        vdc8244_write(&V, (uint8_t)(0x80 + 8 * n + i), shape[i]);
}

/* char i op (x,y): charcode 0-63, kleur 0-7 (ptr = code*8, geen
 * Y-compensatie nodig omdat de tests op row-0-uitlijning werken) */
static void set_char(int i, uint8_t y, uint8_t x, uint8_t code,
                     uint8_t color)
{
    uint16_t ptr9 = (uint16_t)(code * 8);
    vdc8244_write(&V, (uint8_t)(0x10 + 4 * i + 0), y);
    vdc8244_write(&V, (uint8_t)(0x10 + 4 * i + 1), x);
    vdc8244_write(&V, (uint8_t)(0x10 + 4 * i + 2), (uint8_t)(ptr9 & 0xFF));
    vdc8244_write(&V, (uint8_t)(0x10 + 4 * i + 3),
                  (uint8_t)(((ptr9 >> 8) & 1) | (uint8_t)(color << 1)));
}

static const uint8_t k_shape_left1[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
static const uint8_t k_shape_full[8] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* -------------------- V1: mid-frame registerwrites -------------------- */

static int test_V1_midline_registerwrite(void)
{
    vreset();
    set_sprite(0, 10, 20, 0x38, k_shape_full);     /* wit, (20,10)      */
    vdc8244_write(&V, 0xA0, 0x20);                 /* foreground aan    */
    run_lines(0, 49);
    /* mid-frame verplaatsing zoals echte games: fg uit, write, fg aan */
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x00, 100);                  /* Y := 100          */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(50, 241);
    CHECK(px(40, 10) == C_WHITE);      /* oude positie, vóór de write   */
    CHECK(px(40, 100) == C_WHITE);     /* nieuwe positie, na de write   */
    CHECK(px(40, 60) == C_BLACK);      /* tussengebied leeg             */
    return 0;
}

/* ------------- V2: registerblokkade zolang display actief ------------- */

static int test_V2_write_blocked_while_fg_enabled(void)
{
    vreset();
    vdc8244_write(&V, 0x01, 55);
    CHECK(vdc8244_peek(&V, 0x01) == 55);
    vdc8244_write(&V, 0xA0, 0x20);                 /* foreground aan    */
    vdc8244_write(&V, 0x01, 77);                   /* moet genegeerd    */
    vdc8244_write(&V, 0x85, 0xAB);                 /* shape ook object  */
    CHECK(vdc8244_peek(&V, 0x01) == 55);
    CHECK(vdc8244_peek(&V, 0x85) == 0x00);
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x01, 77);
    CHECK(vdc8244_peek(&V, 0x01) == 77);
    /* zelfde blokkade voor gridregisters zolang grid aan */
    vdc8244_write(&V, 0xA0, 0x08);
    vdc8244_write(&V, 0xC0, 0xAA);
    CHECK(vdc8244_peek(&V, 0xC0) == 0x00);
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0xC0, 0xAA);
    CHECK(vdc8244_peek(&V, 0xC0) == 0xAA);
    return 0;
}

/* --------- V3: collision-bitmatrix + selecteer/detecteer-semantiek ---- */

static int test_V3_collision_matrix_sprite_char_grid(void)
{
    vreset();
    set_sprite(0, 60, 40, 0x38, k_shape_left1);    /* 1 px op (40,60)   */
    set_char(0, 60, 40, 47, 7);                    /* blok-glyph        */
    vdc8244_write(&V, 0xA2, 0x01);                 /* selecteer sprite0 */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(60, 60);
    CHECK(vdc8244_read(&V, 0xA2) == 0x80);   /* chars raakten sprite 0  */
    CHECK(vdc8244_read(&V, 0xA2) == 0x00);   /* read latcht én wist     */
    /* omgekeerde selectie: chars selecteren -> sprite 0 gerapporteerd */
    vdc8244_write(&V, 0xA2, 0x80);
    run_lines(61, 61);
    CHECK(vdc8244_read(&V, 0xA2) == 0x01);
    /* verticale-grid-klasse: lijn E0/rij 0 (x=8, y=24..47) vs sprite */
    vreset();
    set_sprite(0, 30, 8, 0x38, k_shape_left1);
    vdc8244_write(&V, 0xE0, 0x01);
    vdc8244_write(&V, 0xA3, 0x04);                 /* gridkleur rood    */
    vdc8244_write(&V, 0xA2, 0x01);
    vdc8244_write(&V, 0xA0, 0x28);                 /* grid + foreground */
    run_lines(30, 30);
    CHECK(vdc8244_read(&V, 0xA2) == 0x10);   /* verticale grid geraakt  */
    return 0;
}

/* ------------- V4: double-size sprites in stappen van 2 px ------------ */

static int test_V4_double_size_2px_steps(void)
{
    int first41, first40, x;
    vreset();
    set_sprite(0, 50, 41, 0x38 | 0x04, k_shape_left1);  /* X=41, dubbel */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(50, 50);
    first41 = -1;
    for (x = 0; x < G7K_FB_W; x++)
        if (px(x, 50) == C_WHITE) { first41 = x; break; }
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x01, 40);                        /* X=40         */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(51, 51);                     /* zelfde shape-rij (span 4) */
    first40 = -1;
    for (x = 0; x < G7K_FB_W; x++)
        if (px(x, 51) == C_WHITE) { first40 = x; break; }
    CHECK(first41 == 80);                  /* X=41 rendert als X=40     */
    CHECK(first40 == 80);
    /* dubbele pixelbreedte: 1 shape-px = 2 VDC-px = 4 fb-px */
    CHECK(px(80, 51) == C_WHITE && px(83, 51) == C_WHITE);
    CHECK(px(84, 51) == C_BLACK);
    return 0;
}

/* ----------------- V5: quad-chars eigen tekenpad ---------------------- */

static int test_V5_quad_char_own_path(void)
{
    vreset();
    /* quad 0: '0','1','2','3'; positie komt uit de LAATSTE sub-char */
    vdc8244_write(&V, 0x40, 200);              /* sub0 Y: mag niets doen */
    vdc8244_write(&V, 0x41, 20);               /* sub0 X: mag niets doen */
    vdc8244_write(&V, 0x42, 0 * 8);
    vdc8244_write(&V, 0x43, 7 << 1);
    vdc8244_write(&V, 0x46, 1 * 8);
    vdc8244_write(&V, 0x47, 7 << 1);
    vdc8244_write(&V, 0x4A, 2 * 8);
    vdc8244_write(&V, 0x4B, 7 << 1);
    vdc8244_write(&V, 0x4C, 80);               /* sub3 Y = quad-Y        */
    vdc8244_write(&V, 0x4D, 30);               /* sub3 X = quad-X        */
    vdc8244_write(&V, 0x4E, 3 * 8);
    vdc8244_write(&V, 0x4F, 7 << 1);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    /* '0'-rij0 (0x70): kolommen 1-3 vanaf x=30 -> fb 62..67           */
    CHECK(px(62, 80) == C_WHITE);
    /* '1'-rij0 (0x20): kolom 2 vanaf x=30+16 -> fb (46+2)*2 = 96      */
    CHECK(px(96, 80) == C_WHITE);
    /* spatie-gebied tussen sub-char 0 en 1 blijft leeg (pitch 16)     */
    CHECK(px(80, 80) == C_BLACK && px(88, 80) == C_BLACK);
    /* positie uit sub-char 0 (20,200) is NIET gebruikt: zou '0' op
     * fb-x 42..47 bij y=200 zetten                                    */
    CHECK(px(42, 200) == C_BLACK);
    return 0;
}

/* ----------------- V6: sprite/char-prioriteitsvolgorde ---------------- */

static int test_V6_priority_order(void)
{
    vreset();
    set_char(0, 60, 40, 47, 1);                    /* rood blok         */
    set_sprite(0, 60, 40, 0x38, k_shape_full);     /* wit sprite 0      */
    set_sprite(3, 60, 40, 0x08, k_shape_full);     /* rood sprite 3     */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(60, 60);
    CHECK(px(80, 60) == C_WHITE);      /* sprite 0 boven char én spr 3  */
    /* zonder sprite 0: sprite 3 boven char */
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x02, 0x38);
    vdc8244_write(&V, 0x00, 200);                  /* sprite 0 weg      */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(61, 61);
    CHECK(px(80, 61) == C_OBJ_RED);    /* sprite 3 wint van char        */
    /* char boven grid: verticale lijn onder het blok-glyph */
    vreset();
    vdc8244_write(&V, 0xE0, 0x02);                 /* rij 1: y=48..71   */
    vdc8244_write(&V, 0xA3, 0x04);                 /* grid dim-rood     */
    set_char(0, 50, 8, 47, 7);                     /* wit blok op x=8   */
    vdc8244_write(&V, 0xA0, 0x28);
    run_lines(50, 50);
    CHECK(px(16, 50) == C_WHITE);      /* char wint van gridlijn        */
    return 0;
}

/* --------------- V7: geen wrap aan de schermranden -------------------- */

static int test_V7_no_edge_wrap(void)
{
    int x;
    vreset();
    set_sprite(0, 30, 157, 0x38, k_shape_full);    /* rechterrand       */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    CHECK(px(314, 30) == C_WHITE);     /* zichtbaar deel                */
    for (x = 0; x < 314; x++)
        CHECK(px(x, 30) == C_BLACK);   /* NIETS teruggevouwen links     */
    /* volledig voorbij de rand: helemaal niets op de lijn */
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x01, 200);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(30, 30);
    for (x = 0; x < G7K_FB_W; x++)
        CHECK(px(x, 30) == C_BLACK);
    /* onderrand: sprite bij Y=235 loopt door tot het frame-einde en
     * duikt NIET bovenaan weer op (kolom fb-120 gekozen buiten het
     * bereik van de all-zero default-chars/quads linksboven) */
    vreset();
    set_sprite(0, 235, 60, 0x38, k_shape_full);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    CHECK(px(120, 235) == C_WHITE);
    for (x = 0; x < 16; x++)
        CHECK(px(120, x) == C_BLACK);
    return 0;
}

/* --------------- V8: shifted/smooth-modus (pseudo-hi-res) ------------- */

static int test_V8_shifted_smooth_mode(void)
{
    vreset();
    /* even-shift (bit 1): even shape-rijen +1 px, oneven rijen niet */
    set_sprite(0, 100, 40, 0x38 | 0x02, k_shape_left1);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(100, 103);
    CHECK(px(82, 100) == C_WHITE && px(80, 100) == C_BLACK); /* rij 0   */
    CHECK(px(80, 102) == C_WHITE && px(82, 102) == C_BLACK); /* rij 1   */
    /* full-shift (bit 0) er bovenop: alles nog 1 px extra naar rechts */
    vdc8244_write(&V, 0xA0, 0x00);
    vdc8244_write(&V, 0x02, 0x38 | 0x02 | 0x01);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(104, 107);
    CHECK(px(84, 104) == C_WHITE);                           /* rij 2   */
    CHECK(px(82, 106) == C_WHITE && px(84, 106) == C_BLACK); /* rij 3   */
    return 0;
}

/* --------------- V9: 64-tekens charset uit chip-documentatie ---------- */

static int test_V9_charset_digits_distinct(void)
{
    /* Signatuur per digit-glyph uit het gerenderde beeld; alle tien
     * moeten onderling verschillen en de spatie (code 12) leeg zijn. */
    uint64_t sig[10];
    int d, e, r, b;
    vreset();
    for (d = 0; d < 10; d++)
        set_char(d, 32, (uint8_t)(4 + 12 * d), (uint8_t)d, 7);
    set_char(10, 150, 40, 12, 7);                  /* spatie            */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    for (d = 0; d < 10; d++) {
        sig[d] = 0;
        for (r = 0; r < 8; r++)
            for (b = 0; b < 8; b++)
                if (px((4 + 12 * d + b) * 2, 32 + 2 * r) == C_WHITE)
                    sig[d] |= 1ULL << (r * 8 + b);
        CHECK(sig[d] != 0);            /* elk cijfer heeft pixels       */
    }
    for (d = 0; d < 10; d++)
        for (e = d + 1; e < 10; e++)
            CHECK(sig[d] != sig[e]);
    for (r = 150; r < 166; r++)
        for (b = 80; b < 96; b++)
            CHECK(px(b, r) == C_BLACK);            /* spatie leeg       */
    return 0;
}

/* ------- V10: dot-grid incl. rechterkolom, lijnhoogtes, onderlijn ----- */

static int test_V10_grid_dots_heights_bottomline(void)
{
    vreset();
    vdc8244_write(&V, 0xE0, 0x01);     /* verticale lijn 0, rij 0       */
    vdc8244_write(&V, 0xC0, 0x01);     /* hor. lijn 0, kolom 0          */
    vdc8244_write(&V, 0xD3, 0x01);     /* onderste hor. lijn, kolom 3   */
    vdc8244_write(&V, 0xA3, 0x04);     /* gridkleur dim-rood            */
    vdc8244_write(&V, 0xA0, 0x08);
    run_lines(0, 241);
    /* verticaal segment: exact één celhoogte (24 lijnen, y=24..47) */
    CHECK(px(16, 23) == C_BLACK);
    CHECK(px(16, 24) == C_DIM_RED);
    CHECK(px(16, 47) == C_DIM_RED);
    CHECK(px(16, 48) == C_BLACK);
    /* horizontale lijn: 3 scanlijnen dik */
    CHECK(px(30, 24) == C_DIM_RED);
    CHECK(px(30, 26) == C_DIM_RED);
    CHECK(px(30, 27) == C_BLACK);
    /* onderste lijn (D-registers): y = 24 + 8*24 = 216..218, kolom 3 */
    CHECK(px(120, 216) == C_DIM_RED);
    CHECK(px(120, 218) == C_DIM_RED);
    CHECK(px(120, 219) == C_BLACK);
    CHECK(px(120, 215) == C_BLACK);
    /* dot-grid: dots op ALLE kruispunten, ook de rechterkolom (x=152) */
    vreset();
    vdc8244_write(&V, 0xA3, 0x04);
    vdc8244_write(&V, 0xA0, 0x40);     /* alleen dots                   */
    run_lines(0, 241);
    CHECK(px(304, 24) == C_DIM_RED);   /* rechterkolom, bovenste rij    */
    CHECK(px(304, 216) == C_DIM_RED);  /* rechterkolom, onderste rij    */
    CHECK(px(16, 216) == C_DIM_RED);   /* linkerkolom, onderste rij     */
    CHECK(px(32, 24) == C_BLACK);      /* tussen kruispunten leeg       */
    return 0;
}

/* --------------- V11: P17 laag = felle kleuren ------------------------ */

static int test_V11_p17_bright(void)
{
    vreset();
    vdc8244_write(&V, 0xE0, 0x01);
    vdc8244_write(&V, 0xA3, 0x04);     /* bg zwart, grid dim-rood       */
    vdc8244_write(&V, 0xA0, 0x08);
    run_lines(30, 30);
    CHECK(px(16, 30) == C_DIM_RED);
    CHECK(px(100, 30) == C_BLACK);
    vdc8244_set_bright(&V, true);      /* P17 laag                      */
    run_lines(31, 31);
    CHECK(px(16, 31) == C_BRIGHT_RED); /* dim-gridkleur wordt fel       */
    CHECK(px(100, 31) == C_BRIGHT_BG0);/* achtergrond wordt fel (grijs) */
    return 0;
}

/* --------------- V12: leesgedrag status/unused/geblokte regs ---------- */

static int test_V12_status_and_read_behaviour(void)
{
    uint8_t st;
    vreset();
    vdc8244_begin_line(&V, 242);       /* VBLANK-start                  */
    CHECK(vdc8244_irq_asserted(&V));
    st = vdc8244_read(&V, 0xA1);
    CHECK((st & 0x08) != 0);           /* VBL-bit                       */
    CHECK((st & 0x01) == 0);           /* geen actieve scan             */
    CHECK((st & 0x04) != 0);           /* shiftregister leeg            */
    CHECK(!vdc8244_irq_asserted(&V));  /* statusread cleart INT         */
    st = vdc8244_read(&V, 0xA1);
    CHECK((st & 0x08) == 0);           /* VBL-latch gewist              */
    vdc8244_begin_line(&V, 10);
    st = vdc8244_read(&V, 0xA1);
    CHECK((st & 0x01) != 0);           /* actieve scan                  */
    /* sound-empty verdwijnt zodra het shiftregister geladen is */
    vdc8244_write(&V, 0xA7, 0xFF);
    CHECK((vdc8244_read(&V, 0xA1) & 0x04) == 0);
    /* ongebruikte registers lezen 0x00 */
    vdc8244_write(&V, 0xB0, 0x55);
    CHECK(vdc8244_read(&V, 0xB0) == 0x00);
    /* objectregisters zijn ook voor lezen afgekoppeld zolang fg aan */
    vdc8244_write(&V, 0x01, 55);
    vdc8244_write(&V, 0xA0, 0x20);
    CHECK(vdc8244_read(&V, 0x01) == 0x00);
    vdc8244_write(&V, 0xA0, 0x00);
    CHECK(vdc8244_read(&V, 0x01) == 55);
    return 0;
}

/* --------------- A1: toon-shiftregister + klokafleiding ---------------- */

/* afstand tussen grote sample-sprongen = shiftklok-periode in samples
 * (2 samples per lijn; deler 4 of 16 lijnen per shift) */
static int flip_interval(const int16_t *s, int n, int expect)
{
    int i, last = -1, checked = 0;
    for (i = 1; i < n; i++) {
        int d = (int)s[i] - (int)s[i - 1];
        if (d < 0)
            d = -d;
        if (d >= 15 * 600) {
            if (last >= 0) {
                if (i - last != expect)
                    return 0;
                checked++;
            }
            last = i;
        }
    }
    return checked >= 3;
}

static int test_A1_tone_pitch_clock_derivation(void)
{
    static int16_t buf[600];
    int n;
    vreset();
    vdc8244_write(&V, 0xA7, 0x55);     /* alternerend patroon 0x555555  */
    vdc8244_write(&V, 0xA8, 0x55);
    vdc8244_write(&V, 0xA9, 0x55);
    vdc8244_write(&V, 0xAA, 0x80 | 0x40 | 0x20 | 0x0F); /* aan+loop+snel */
    run_lines(0, 199);
    n = vdc8244_audio_read(&V, buf, 600);
    CHECK(n == 400);
    /* snelle klok: shift per 4 lijnen -> togglen per 8 samples (3933Hz-
     * afleiding van de lijnfrequentie) */
    CHECK(flip_interval(buf, n, 8));
    /* trage klok: shift per 16 lijnen -> togglen per 32 samples (983Hz) */
    vreset();
    vdc8244_write(&V, 0xA7, 0x55);
    vdc8244_write(&V, 0xA8, 0x55);
    vdc8244_write(&V, 0xA9, 0x55);
    vdc8244_write(&V, 0xAA, 0x80 | 0x40 | 0x0F);
    run_lines(0, 199);
    n = vdc8244_audio_read(&V, buf, 600);
    CHECK(flip_interval(buf, n, 32));
    /* zonder recirculate loopt het register leeg -> status "empty" */
    vreset();
    vdc8244_write(&V, 0xA7, 0xFF);
    vdc8244_write(&V, 0xAA, 0x80 | 0x20 | 0x0F);   /* geen recirculate  */
    CHECK((vdc8244_read(&V, 0xA1) & 0x04) == 0);
    run_lines(0, 40);                  /* >= 8 shifts van 4 lijnen      */
    CHECK((vdc8244_read(&V, 0xA1) & 0x04) != 0);
    return 0;
}

/* --------------- A2: witte-ruis-LFSR ----------------------------------- */

static int test_A2_noise_lfsr(void)
{
    static int16_t buf[600];
    int n, i, pos = 0, neg = 0;
    vreset();
    CHECK(V.audio_lfsr == 0x7FFF);     /* deterministische seed         */
    vdc8244_write(&V, 0xA7, 0x01);     /* inhoud irrelevant bij noise   */
    vdc8244_write(&V, 0xAA, 0x80 | 0x40 | 0x20 | 0x10 | 0x0F);
    run_lines(0, 199);
    CHECK(V.audio_lfsr != 0x7FFF);     /* LFSR is doorgeklokt           */
    n = vdc8244_audio_read(&V, buf, 600);
    CHECK(n == 400);
    for (i = 0; i < n; i++) {
        if (buf[i] > 0)
            pos++;
        if (buf[i] < 0)
            neg++;
    }
    CHECK(pos > 0 && neg > 0);         /* beide polariteiten = ruis     */
    return 0;
}

/* --------------- A3: lowpass op de analoge uitgang --------------------- */

static int test_A3_lowpass_smoothing(void)
{
    static int16_t buf[600];
    int n;
    vreset();
    run_lines(0, 9);                   /* 20 samples stilte             */
    vdc8244_write(&V, 0xA7, 0xFF);     /* constante '1'-uitgang         */
    vdc8244_write(&V, 0xA8, 0xFF);
    vdc8244_write(&V, 0xA9, 0xFF);
    vdc8244_write(&V, 0xAA, 0x80 | 0x40 | 0x20 | 0x0F);
    run_lines(10, 59);
    n = vdc8244_audio_read(&V, buf, 600);
    CHECK(n == 120);
    CHECK(buf[19] == 0);               /* stilte vóór de stap           */
    /* de stap wordt uitgesmeerd: eerste sample halverwege, daarna
     * monotoon richting de asymptoot 15*1024 */
    CHECK(buf[20] > 0 && buf[20] < 15 * 1024);
    CHECK(buf[21] > buf[20]);
    CHECK(buf[119] >= 15 * 1024 - 64); /* geconvergeerd                 */
    CHECK(buf[119] <= 15 * 1024);
    return 0;
}

/* -------- C7 (VDC-kant): vblank/hblank-omrekening per regio ------------ */

static int test_C7_vblank_hblank_units(void)
{
    vreset();                              /* NTSC */
    vdc8244_begin_line(&V, 241);
    CHECK(!vdc8244_vblank(&V));
    vdc8244_begin_line(&V, 242);           /* VBLANK-startlijn */
    CHECK(vdc8244_vblank(&V));
    vdc8244_begin_line(&V, 262);
    CHECK(vdc8244_vblank(&V));
    /* NTSC: 20 px per cyclus, htotal 455, HBLANK vanaf px 366 */
    CHECK(!vdc8244_hblank_at(&V, 0));
    CHECK(!vdc8244_hblank_at(&V, 18));     /* 360 < 366                 */
    CHECK(vdc8244_hblank_at(&V, 19));      /* 380 >= 366                */
    CHECK(vdc8244_hblank_at(&V, 22));      /* 440                       */
    CHECK(!vdc8244_hblank_at(&V, 23));     /* 460 % 455 = 5 (wrap)      */
    /* PAL: 18 px per cyclus, htotal 456 */
    vdc8244_set_region(&V, G7K_REGION_PAL);
    CHECK(!vdc8244_hblank_at(&V, 0));
    CHECK(!vdc8244_hblank_at(&V, 20));     /* 360 < 366                 */
    CHECK(vdc8244_hblank_at(&V, 21));      /* 378 >= 366                */
    CHECK(vdc8244_hblank_at(&V, 25));      /* 450                       */
    CHECK(!vdc8244_hblank_at(&V, 26));     /* 468 % 456 = 12 (wrap)     */
    return 0;
}

/* ---- V3 (vervolg): sprite<->sprite, hor. grid en dots ([B] 4.8) ------- */

static int test_V3b_collision_sprite_sprite_hgrid_dots(void)
{
    /* sprite <-> sprite (bits 0-3 onderling) */
    vreset();
    set_sprite(0, 60, 40, 0x38, k_shape_left1);
    set_sprite(1, 60, 40, 0x08, k_shape_left1);
    vdc8244_write(&V, 0xA2, 0x01);         /* selecteer sprite 0        */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(60, 60);
    CHECK(vdc8244_read(&V, 0xA2) == 0x02); /* sprite 1 raakte sprite 0  */
    vdc8244_write(&V, 0xA2, 0x02);         /* omgekeerde selectie       */
    run_lines(61, 61);
    CHECK(vdc8244_read(&V, 0xA2) == 0x01);
    vdc8244_write(&V, 0xA2, 0x03);         /* beide geselecteerd        */
    run_lines(62, 62);
    CHECK(vdc8244_read(&V, 0xA2) == 0x03);
    /* horizontale grid (bit 5) <-> sprite: lijn 0 kolom 0 = y24..26 */
    vreset();
    set_sprite(0, 24, 10, 0x38, k_shape_left1);
    vdc8244_write(&V, 0xC0, 0x01);
    vdc8244_write(&V, 0xA3, 0x04);
    vdc8244_write(&V, 0xA2, 0x01);
    vdc8244_write(&V, 0xA0, 0x28);         /* grid + foreground         */
    run_lines(24, 24);
    CHECK(vdc8244_read(&V, 0xA2) == 0x20);
    vdc8244_write(&V, 0xA2, 0x20);         /* omgekeerd: hgrid select   */
    run_lines(25, 25);
    CHECK(vdc8244_read(&V, 0xA2) == 0x01);
    /* dots delen bit 5 met de horizontale grid ([B] 4.8) */
    vreset();
    set_sprite(0, 24, 8, 0x38, k_shape_left1);
    vdc8244_write(&V, 0xA3, 0x04);
    vdc8244_write(&V, 0xA2, 0x01);
    vdc8244_write(&V, 0xA0, 0x60);         /* dots + foreground         */
    run_lines(24, 24);
    CHECK(vdc8244_read(&V, 0xA2) == 0x20);
    /* chars <-> horizontale grid */
    vreset();
    set_char(0, 24, 10, 47, 7);
    vdc8244_write(&V, 0xC0, 0x01);
    vdc8244_write(&V, 0xA3, 0x04);
    vdc8244_write(&V, 0xA2, 0x80);
    vdc8244_write(&V, 0xA0, 0x28);
    run_lines(24, 24);
    CHECK(vdc8244_read(&V, 0xA2) == 0x20);
    return 0;
}

/* --------- V7 (vervolg): chars en quad-chars wrappen ook niet ---------- */

static int test_V7b_char_quad_no_edge_wrap(void)
{
    int x, j;
    /* char over de rechterrand: zichtbaar deel, niets teruggevouwen */
    vreset();
    set_char(0, 30, 157, 47, 7);           /* blok-glyph, 5 px kolommen */
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    CHECK(px(314, 30) == C_WHITE && px(319, 30) == C_WHITE);
    for (x = 0; x < 314; x++)
        CHECK(px(x, 30) == C_BLACK);
    /* char volledig voorbij de rand: hele lijn leeg */
    vreset();
    set_char(0, 30, 200, 47, 7);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(30, 30);
    for (x = 0; x < G7K_FB_W; x++)
        CHECK(px(x, 30) == C_BLACK);
    /* onderrand: char op Y=238 loopt door tot frame-einde (fb toont
     * t/m lijn 239, 2 lijnen overscan-crop), duikt NIET bovenaan op */
    vreset();
    set_char(0, 238, 60, 47, 7);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    CHECK(px(120, 238) == C_WHITE && px(120, 239) == C_WHITE);
    for (x = 0; x < 16; x++)
        CHECK(px(120, x) == C_BLACK);
    /* quad over de rechterrand: sub-chars 2/3 clippen weg, geen wrap */
    vreset();
    for (j = 0; j < 4; j++) {
        vdc8244_write(&V, (uint8_t)(0x40 + 4 * j + 2), 0x78);  /* 47*8  */
        vdc8244_write(&V, (uint8_t)(0x40 + 4 * j + 3), 0x0F);  /* b8+wit*/
    }
    vdc8244_write(&V, 0x4C, 100);          /* quad-Y uit sub-char 3     */
    vdc8244_write(&V, 0x4D, 140);          /* quad-X: subs op 140/156/  */
    vdc8244_write(&V, 0xA0, 0x20);         /* 172/188 (2 buiten beeld)  */
    run_lines(100, 100);
    CHECK(px(280, 100) == C_WHITE);        /* sub 0 zichtbaar           */
    CHECK(px(312, 100) == C_WHITE && px(319, 100) == C_WHITE); /* sub 1 */
    for (x = 0; x < 280; x++)
        CHECK(px(x, 100) == C_BLACK);      /* niets teruggevouwen       */
    return 0;
}

/* --------- V9 (vervolg): alle 64 glyphs uniek, spatie leeg ------------- */

static uint64_t glyph_sig(uint8_t code)
{
    uint64_t sig = 0;
    int r, b;
    vreset();
    set_char(0, 32, 20, code, 7);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(32, 47);
    for (r = 0; r < 8; r++)
        for (b = 0; b < 8; b++)
            if (px((20 + b) * 2, 32 + 2 * r) == C_WHITE)
                sig |= 1ULL << (r * 8 + b);
    return sig;
}

static int test_V9b_charset_all64_distinct(void)
{
    static uint64_t sig[64];
    int c, d;
    for (c = 0; c < 64; c++)
        sig[c] = glyph_sig((uint8_t)c);
    CHECK(sig[12] == 0);                   /* spatie is leeg            */
    for (c = 0; c < 64; c++)
        if (c != 12)
            CHECK(sig[c] != 0);            /* elk ander teken tekent    */
    for (c = 0; c < 64; c++)
        for (d = c + 1; d < 64; d++)
            if (c != 12 && d != 12)
                CHECK(sig[c] != sig[d]);   /* alle glyphs onderling uniek */
    /* NB: dit klikt de EIGEN-ontwerp-glyphs vast als regressieanker; de
     * echte 8244-maskvormen zijn niet publiek gedocumenteerd (alleen de
     * volgorde, [B] App. C) — gedeclareerd in QUIRKS.md noot 4/10. */
    return 0;
}

/* ------ char-pointer/Y-interactie: relatief model = v0.1-canon --------- */

static int test_V_char_y_ptr_canon(void)
{
    int b;
    /* '1' (code 1) op oneven Y=33: rij = (lijn-Y)>>1 (relatief model,
     * QUIRKS noot 4) — lijn 34 toont glyphrij 0 (0x20: alleen kolom 2) */
    vreset();
    set_char(0, 33, 40, 1, 7);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(0, 241);
    CHECK(px((40 + 2) * 2, 34) == C_WHITE);
    CHECK(px((40 + 1) * 2, 34) == C_BLACK);
    /* lijn 45: rij (45-33)>>1 = 6 (0x70: kolommen 1-3) */
    CHECK(px((40 + 1) * 2, 45) == C_WHITE);
    CHECK(px((40 + 3) * 2, 45) == C_WHITE);
    CHECK(px((40 + 4) * 2, 45) == C_BLACK);
    /* 9-bit pointer-wrap: ptr 511, rij 1 wrapt naar charset-byte 0 */
    vreset();
    vdc8244_write(&V, 0x10, 60);
    vdc8244_write(&V, 0x11, 40);
    vdc8244_write(&V, 0x12, 0xFF);
    vdc8244_write(&V, 0x13, (uint8_t)(0x01 | (7 << 1)));
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(60, 63);
    for (b = 0; b < 8; b++)                /* charset[511] = 0x00       */
        CHECK(px((40 + b) * 2, 60) == C_BLACK);
    CHECK(px((40 + 1) * 2, 62) == C_WHITE);   /* charset[0] = 0x70      */
    CHECK(px((40 + 3) * 2, 62) == C_WHITE);
    CHECK(px((40 + 0) * 2, 62) == C_BLACK);
    CHECK(px((40 + 4) * 2, 62) == C_BLACK);
    return 0;
}

/* ------ V12 (vervolg): strobe-polariteit, intra-lijn HBL, CHROVL ------- */

static int test_V12b_strobe_hbl_chrovl(void)
{
    vreset();
    vdc8244_begin_line(&V, 10);
    /* [B] 4.7 bit 1: 1 = Follow Beam, 0 = Latched */
    CHECK((vdc8244_read(&V, 0xA1) & 0x02) != 0);
    vdc8244_write(&V, 0xA0, 0x02);         /* latch aan                 */
    CHECK((vdc8244_read(&V, 0xA1) & 0x02) == 0);
    vdc8244_write(&V, 0xA0, 0x00);
    /* HBL-bit intra-lijn ([B] 4.7 bit 0 + 4.11): begin van de lijn
     * actief (bit=1), staart van de lijn = HBLANK (bit=0) */
    vdc8244_set_line_phase(&V, 0);
    CHECK((vdc8244_read(&V, 0xA1) & 0x01) != 0);
    vdc8244_set_line_phase(&V, 20);        /* NTSC: 400 px >= 366       */
    CHECK((vdc8244_read(&V, 0xA1) & 0x01) == 0);
    vdc8244_set_line_phase(&V, 23);        /* wrap naar volgende lijn   */
    CHECK((vdc8244_read(&V, 0xA1) & 0x01) != 0);
    vdc8244_begin_line(&V, 250);           /* VBLANK: altijd 0          */
    CHECK((vdc8244_read(&V, 0xA1) & 0x01) == 0);
    /* char-overlap (bit 7) is tegenwoordige tijd, geen eeuwige latch */
    vreset();
    set_char(0, 60, 40, 47, 7);
    set_char(1, 60, 40, 47, 1);
    vdc8244_write(&V, 0xA0, 0x20);
    run_lines(60, 60);
    CHECK((vdc8244_peek(&V, 0xA1) & 0x80) != 0);
    run_lines(61, 73);                     /* blok-rijen 0-6 overlappen */
    CHECK((vdc8244_peek(&V, 0xA1) & 0x80) != 0);
    run_lines(74, 100);                    /* rij 7 v.h. blok is leeg + */
    CHECK((vdc8244_peek(&V, 0xA1) & 0x80) == 0);   /* voorbij de chars  */
    return 0;
}

/* --------- fill-mode ($A0 bit 7): kleur + collision-klasse ------------- */

static int test_V_fill_mode_color_collision(void)
{
    vreset();
    vdc8244_write(&V, 0xE0, 0x01);         /* verticale lijn 0, rij 0   */
    vdc8244_write(&V, 0xA3, 0x04);         /* gridkleur dim-rood        */
    vdc8244_write(&V, 0xA0, 0x88);         /* grid + fill               */
    run_lines(0, 241);
    CHECK(px(16, 30) == C_DIM_RED);        /* de lijn zelf (vdc x8..9)  */
    CHECK(px(20, 30) == C_DIM_RED);        /* fill vdc x10..23          */
    CHECK(px(46, 30) == C_DIM_RED);
    CHECK(px(48, 30) == C_BLACK);          /* volgende cel: geen fill   */
    CHECK(px(16, 23) == C_BLACK);          /* verticaal begrensd op cel */
    CHECK(px(16, 48) == C_BLACK);
    /* collision-klasse van het vlak = verticale grid (v0.1-canon) */
    vreset();
    set_sprite(0, 30, 15, 0x38, k_shape_left1);
    vdc8244_write(&V, 0xE0, 0x01);
    vdc8244_write(&V, 0xA3, 0x04);
    vdc8244_write(&V, 0xA2, 0x01);
    vdc8244_write(&V, 0xA0, 0xA8);         /* grid + fill + foreground  */
    run_lines(30, 30);
    CHECK(vdc8244_read(&V, 0xA2) == 0x10);
    return 0;
}

/* --------- horizontale interrupt ($A0 bit 0), v0.1-canon --------------- */

static int test_V_hirq_active_lines(void)
{
    vreset();
    vdc8244_write(&V, 0xA0, 0x01);         /* HIRQ aan                  */
    vdc8244_begin_line(&V, 0);
    CHECK(vdc8244_irq_asserted(&V));
    (void)vdc8244_read(&V, 0xA1);          /* statusread cleart INT     */
    CHECK(!vdc8244_irq_asserted(&V));
    vdc8244_begin_line(&V, 1);
    CHECK(vdc8244_irq_asserted(&V));       /* elke actieve lijn opnieuw */
    (void)vdc8244_read(&V, 0xA1);
    vdc8244_begin_line(&V, 241);
    CHECK(vdc8244_irq_asserted(&V));
    (void)vdc8244_read(&V, 0xA1);
    vdc8244_begin_line(&V, 242);
    CHECK(vdc8244_irq_asserted(&V));       /* VBL-IRQ                   */
    (void)vdc8244_read(&V, 0xA1);
    /* v0.1-canon (QUIRKS noot 4): HIRQ vuurt bij lijnSTART van actieve
     * lijnen en NIET tijdens VBLANK-lijnen 243+ */
    vdc8244_begin_line(&V, 243);
    CHECK(!vdc8244_irq_asserted(&V));
    /* zonder HIRQ-bit: geen IRQ op actieve lijnen */
    vreset();
    vdc8244_begin_line(&V, 5);
    CHECK(!vdc8244_irq_asserted(&V));
    return 0;
}

/* --------- beam-latch $A4/$A5 via CTRL-bit-1-flank ---------------------- */

static int test_V_beam_latch_a4_a5(void)
{
    vreset();
    vdc8244_begin_line(&V, 100);
    CHECK(vdc8244_read(&V, 0xA4) == 100);  /* follow beam = huidige lijn */
    vdc8244_write(&V, 0xA0, 0x02);         /* 0->1-flank: latch          */
    vdc8244_begin_line(&V, 150);
    CHECK(vdc8244_read(&V, 0xA4) == 100);  /* Y bevroren op flankmoment  */
    CHECK(vdc8244_read(&V, 0xA5) == 0);    /* X: geen intra-lijn-fase    */
    vdc8244_write(&V, 0xA0, 0x02);         /* geen flank: blijft 100     */
    CHECK(vdc8244_read(&V, 0xA4) == 100);
    vdc8244_write(&V, 0xA0, 0x00);         /* latch los: follow beam     */
    CHECK(vdc8244_read(&V, 0xA4) == 150);
    vdc8244_write(&V, 0xA0, 0x02);         /* nieuwe flank op lijn 150   */
    CHECK(vdc8244_read(&V, 0xA4) == 150);
    return 0;
}

/* --------- palet: alle 24 kleuren vastgeklikt (regressie-anker) --------- */

static int test_V_palette_all24(void)
{
    /* Onafhankelijk gedupliceerde verwachtingen (fb-woord 0xAABBGGRR);
     * herkomst/afwijkingen van de [B] App. B-namen: zie paletkop in
     * vdc8244.c + QUIRKS noot 6. */
    static const uint32_t exp_dim[8] = {
        0xFF000000u, 0xFFA01000u, 0xFF00A000u, 0xFFA0A000u,
        0xFF0000A0u, 0xFFA000A0u, 0xFF0060A0u, 0xFFA0A0A0u,
    };
    static const uint32_t exp_bright[8] = {
        0xFF404040u, 0xFFFF6040u, 0xFF40FF40u, 0xFFFFFF40u,
        0xFF4040FFu, 0xFFFF40FFu, 0xFF40A0FFu, 0xFFFFFFFFu,
    };
    static const uint32_t exp_obj[8] = {
        0xFF707070u, 0xFF4040FFu, 0xFF40FF40u, 0xFF40A0FFu,
        0xFFFF6040u, 0xFFFF40FFu, 0xFFFFFF40u, 0xFFFFFFFFu,
    };
    int idx;
    for (idx = 0; idx < 8; idx++) {
        vreset();
        vdc8244_write(&V, 0xA3, (uint8_t)(idx << 3));  /* achtergrond   */
        run_lines(5, 5);
        CHECK(px(10, 5) == exp_dim[idx]);
        vdc8244_set_bright(&V, true);                  /* V11-pad       */
        run_lines(6, 6);
        CHECK(px(10, 6) == exp_bright[idx]);
    }
    for (idx = 0; idx < 8; idx++) {
        vreset();
        set_char(0, 20, 20, 47, (uint8_t)idx);         /* blok-glyph    */
        vdc8244_write(&V, 0xA0, 0x20);
        run_lines(20, 20);
        CHECK(px(40, 20) == exp_obj[idx]);
    }
    return 0;
}

/* ----------------------------------------------------------------------- */

void register_vdc_tests(void);

void register_vdc_tests(void)
{
    g7k_register_test("V1_midline_registerwrite",
                      test_V1_midline_registerwrite);
    g7k_register_test("V2_write_blocked_while_fg_enabled",
                      test_V2_write_blocked_while_fg_enabled);
    g7k_register_test("V3_collision_matrix_sprite_char_grid",
                      test_V3_collision_matrix_sprite_char_grid);
    g7k_register_test("V4_double_size_2px_steps",
                      test_V4_double_size_2px_steps);
    g7k_register_test("V5_quad_char_own_path",
                      test_V5_quad_char_own_path);
    g7k_register_test("V6_priority_order",
                      test_V6_priority_order);
    g7k_register_test("V7_no_edge_wrap",
                      test_V7_no_edge_wrap);
    g7k_register_test("V8_shifted_smooth_mode",
                      test_V8_shifted_smooth_mode);
    g7k_register_test("V9_charset_digits_distinct",
                      test_V9_charset_digits_distinct);
    g7k_register_test("V10_grid_dots_heights_bottomline",
                      test_V10_grid_dots_heights_bottomline);
    g7k_register_test("V11_p17_bright",
                      test_V11_p17_bright);
    g7k_register_test("V12_status_and_read_behaviour",
                      test_V12_status_and_read_behaviour);
    g7k_register_test("A1_tone_pitch_clock_derivation",
                      test_A1_tone_pitch_clock_derivation);
    g7k_register_test("A2_noise_lfsr",
                      test_A2_noise_lfsr);
    g7k_register_test("A3_lowpass_smoothing",
                      test_A3_lowpass_smoothing);
    g7k_register_test("C7_vblank_hblank_units",
                      test_C7_vblank_hblank_units);
    g7k_register_test("V3b_collision_sprite_sprite_hgrid_dots",
                      test_V3b_collision_sprite_sprite_hgrid_dots);
    g7k_register_test("V7b_char_quad_no_edge_wrap",
                      test_V7b_char_quad_no_edge_wrap);
    g7k_register_test("V9b_charset_all64_distinct",
                      test_V9b_charset_all64_distinct);
    g7k_register_test("V_char_y_ptr_canon",
                      test_V_char_y_ptr_canon);
    g7k_register_test("V12b_strobe_hbl_chrovl",
                      test_V12b_strobe_hbl_chrovl);
    g7k_register_test("V_fill_mode_color_collision",
                      test_V_fill_mode_color_collision);
    g7k_register_test("V_hirq_active_lines",
                      test_V_hirq_active_lines);
    g7k_register_test("V_beam_latch_a4_a5",
                      test_V_beam_latch_a4_a5);
    g7k_register_test("V_palette_all24",
                      test_V_palette_all24);
}
