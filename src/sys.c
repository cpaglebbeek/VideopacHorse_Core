/*
 * sys.c — systeemlaag VideopacHorse_Core: publieke API, bus-routing en
 * scanline-lockstep-scheduler (v0.1, architect).
 *
 * Bindweefsel tussen cpu8048 / vdc8244 / cart / state; de interfaces staan
 * in core_internal.h (incl. kloktopologie + bronnen). Tot de echte
 * subsysteem-implementaties bestaan linkt dit via src/stubs_tmp.c.
 */
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"

/* ------------------------------------------------------------------ */
/* bus-callbacks (CPU -> systeem); routing-feiten: core_internal.h SYS */
/* ------------------------------------------------------------------ */

static uint8_t sys_read_rom(void *ctx, uint16_t addr)
{
    g7k_sys *sys = ctx;
    addr &= 0x0FFF;
    if (addr < 0x400)                       /* 1KB BIOS in de 8048 (M1) */
        return sys->bios_loaded ? sys->bios[addr] : 0xFF;
    return g7k_cart_read(&sys->cart, addr); /* extern venster (M1-M4)   */
}

/* CPU-machinecycli sinds de start van de huidige scanlijn (beam-fase) */
static int sys_cycles_into_line(const g7k_sys *sys)
{
    return (int)(sys->cpu.cycles - sys->line_start_cycles);
}

static uint8_t sys_read_ext(void *ctx, uint8_t addr)
{
    g7k_sys *sys = ctx;
    uint8_t data = 0xFF;                    /* open bus (M6)            */
    /* VDC-read: P13 (CS) én P16 laag; open-collector: AND-combineren   */
    if ((sys->p1 & 0x48) == 0) {
        vdc8244_set_line_phase(&sys->vdc, sys_cycles_into_line(sys));
        data &= vdc8244_read(&sys->vdc, addr);
    }
    /* 128B externe RAM: P14 laag én A7 laag (M5)                       */
    if (!(sys->p1 & 0x10) && !(addr & 0x80))
        data &= sys->extram[addr & 0x7F];
    return data;
}

static void sys_write_ext(void *ctx, uint8_t addr, uint8_t data)
{
    g7k_sys *sys = ctx;
    if (!(sys->p1 & 0x08)) {
        vdc8244_set_line_phase(&sys->vdc, sys_cycles_into_line(sys));
        vdc8244_write(&sys->vdc, addr, data);
    }
    /* externe write-strobe alleen actief als P16 laag is (bron [1] in
     * core_internal.h, io_write): met P16 hoog landt een MOVX-write
     * nergens. De VDC-write hierboven hangt NIET achter deze gate. */
    if (!(sys->p1 & 0x40) && !(sys->p1 & 0x10) && !(addr & 0x80))
        sys->extram[addr & 0x7F] = data;
}

static void sys_write_p1(void *ctx, uint8_t val)
{
    g7k_sys *sys = ctx;
    sys->p1 = val;
    g7k_cart_set_p1(&sys->cart, val);            /* bank live (M3)      */
    vdc8244_set_bright(&sys->vdc, !(val & 0x80)); /* P17 laag = fel (V11)*/
}

static uint8_t sys_read_p1(void *ctx, uint8_t latch)
{
    (void)ctx;                     /* niets drijft P1 extern laag        */
    return latch;
}

static void sys_write_p2(void *ctx, uint8_t val)
{
    g7k_sys *sys = ctx;
    sys->p2 = val;                 /* P2[0:2] = rij-select matrix        */
}

static uint8_t sys_read_p2(void *ctx, uint8_t latch)
{
    g7k_sys *sys = ctx;
    uint8_t ext = 0xFF;
    /* toetsenbordscan actief als P12 laag (74156-enable) */
    if (!(sys->p1 & 0x04)) {
        uint8_t row_bits = sys->keys_live[sys->p2 & 0x07];
        if (row_bits) {
            /* 74148-prioriteitsencoder: hoogste kolom wint;
             * GS -> P24 laag, uitgangen actief-laag -> P25-27 = ~kolom */
            uint8_t col = 7;
            while (col > 0 && !(row_bits & (uint8_t)(1u << col)))
                col--;
            ext = (uint8_t)(0x0F | (uint8_t)((~col & 0x07u) << 5));
        }
    }
    return latch & ext;            /* quasi-bidirectioneel: extern trekt laag */
}

static uint8_t sys_read_bus(void *ctx)
{
    g7k_sys *sys = ctx;
    uint8_t data = 0xFF;
    /* joysticks delen de matrix-select: rij 0/1 bij actieve scan.
     * Bitvolgorde (actief-laag) volgens O2-tech-documentatie:
     * b0=op b1=rechts b2=neer b3=links b4=vuur — S4-test verifieert. */
    if (!(sys->p1 & 0x04)) {
        uint8_t sel = sys->p2 & 0x07;
        if (sel < 2) {
            uint8_t m = sys->joy_live[sel];
            if (m & G7K_JOY_UP)    data &= (uint8_t)~0x01u;
            if (m & G7K_JOY_RIGHT) data &= (uint8_t)~0x02u;
            if (m & G7K_JOY_DOWN)  data &= (uint8_t)~0x04u;
            if (m & G7K_JOY_LEFT)  data &= (uint8_t)~0x08u;
            if (m & G7K_JOY_FIRE)  data &= (uint8_t)~0x10u;
        }
    }
    return data;
}

static void sys_write_bus(void *ctx, uint8_t val)
{
    (void)ctx; (void)val;          /* BUS-writes landen nergens (open)   */
}

static bool sys_read_t0(void *ctx)
{
    (void)ctx;                     /* T0 ongebruikt op O2/G7000          */
    return false;
}

static bool sys_read_t1(void *ctx)
{
    g7k_sys *sys = ctx;
    int into_line = (int)(sys->cpu.cycles - sys->line_start_cycles);
    /* T1 = VBL OF HBL (C7) */
    return vdc8244_vblank(&sys->vdc) ||
           vdc8244_hblank_at(&sys->vdc, into_line);
}

/* ------------------------------------------------------------------ */
/* levenscyclus                                                        */
/* ------------------------------------------------------------------ */

g7k_sys *g7k_create(void)
{
    g7k_sys *sys = calloc(1, sizeof(*sys)); /* enige malloc in de core  */
    if (!sys)
        return NULL;

    sys->region = G7K_REGION_PAL;           /* G7000 = PAL-machine (S2) */

    cpu8048_bus bus = {
        .ctx       = sys,
        .read_rom  = sys_read_rom,
        .read_ext  = sys_read_ext,
        .write_ext = sys_write_ext,
        .write_p1  = sys_write_p1,
        .read_p1   = sys_read_p1,
        .write_p2  = sys_write_p2,
        .read_p2   = sys_read_p2,
        .read_bus  = sys_read_bus,
        .write_bus = sys_write_bus,
        .read_t0   = sys_read_t0,
        .read_t1   = sys_read_t1,
    };
    cpu8048_init(&sys->cpu, &bus);
    vdc8244_init(&sys->vdc, sys->region, sys->fb, G7K_FB_W, G7K_FB_H);
    g7k_cart_reset(&sys->cart);

    g7k_reset(sys, true);
    return sys;
}

void g7k_destroy(g7k_sys *sys)
{
    free(sys);
}

int g7k_load_bios(g7k_sys *sys, const uint8_t *data, size_t size)
{
    if (!sys || !data || size != sizeof(sys->bios))
        return -1;
    memcpy(sys->bios, data, size);
    sys->bios_loaded = true;
    return 0;
}

int g7k_load_cart(g7k_sys *sys, const uint8_t *data, size_t size)
{
    if (!sys)
        return -1;
    return g7k_cart_load(&sys->cart, data, size);
}

/* S1: koud = power-cycle (RAM/VDC/cycli gewist); warm = reset-toets, die
 * alleen aan de 8048-RESET-pin hangt: RAM- en VDC-state blijven staan. */
void g7k_reset(g7k_sys *sys, bool cold)
{
    if (!sys)
        return;
    if (cold) {
        memset(sys->cpu.iram, 0, sizeof(sys->cpu.iram));
        memset(sys->extram, 0, sizeof(sys->extram));
        memset(sys->fb, 0, sizeof(sys->fb));
        vdc8244_reset(&sys->vdc, true);
        g7k_cart_reset(&sys->cart);
        sys->cpu.cycles = 0;
        sys->cyc_frac = 0;
        sys->cyc_debt = 0;
        sys->line_start_cycles = 0;
        sys->frames = 0;
    }
    /* zet P1=P2=0xFF en pompt dat via de callbacks door: cart boot uit
     * bank 3 (C8/M3), VDC-selects inactief, helderheid normaal. */
    cpu8048_reset(&sys->cpu);
    sys->p1 = sys->cpu.p1_latch;
    sys->p2 = sys->cpu.p2_latch;
}

void g7k_set_region(g7k_sys *sys, g7k_region region)
{
    if (!sys)
        return;
    sys->region = region;          /* gaat in bij het volgende frame (S2) */
    vdc8244_set_region(&sys->vdc, region);
}

g7k_region g7k_get_region(const g7k_sys *sys)
{
    return sys ? sys->region : G7K_REGION_PAL;
}

/* ------------------------------------------------------------------ */
/* scheduler: scanline-lockstep (V1)                                   */
/* ------------------------------------------------------------------ */

void g7k_run_frame(g7k_sys *sys)
{
    if (!sys)
        return;

    /* input latchen op framegrens: deterministisch voor record/replay */
    memcpy(sys->joy_live, sys->joy_pending, sizeof(sys->joy_live));
    memcpy(sys->keys_live, sys->keys_pending, sizeof(sys->keys_live));

    /* audio-contract g7000.h: "samples van het laatste frame" — ring
     * leegt op framegrens, ongeacht of de frontend heeft gelezen */
    sys->vdc.sample_count = 0;

    const bool ntsc  = (sys->region == G7K_REGION_NTSC);
    const int  lines = ntsc ? G7K_NTSC_LINES   : G7K_PAL_LINES;
    const int  num   = ntsc ? G7K_NTSC_CYC_NUM : G7K_PAL_CYC_NUM;
    const int  den   = ntsc ? G7K_NTSC_CYC_DEN : G7K_PAL_CYC_DEN;

    for (int line = 0; line < lines; line++) {
        vdc8244_begin_line(&sys->vdc, line);
        cpu8048_set_irq(&sys->cpu, vdc8244_irq_asserted(&sys->vdc));

        /* exact cyclusbudget voor deze lijn: breuk-accumulator (91/4
         * NTSC, 76/3 PAL — zie kloktopologie) minus overshoot-schuld
         * van de vorige lijn; over een frame is de drift exact nul. */
        sys->cyc_frac += num;
        int budget = sys->cyc_frac / den - sys->cyc_debt;
        sys->cyc_frac %= den;
        sys->cyc_debt = 0;
        sys->line_start_cycles = sys->cpu.cycles;

        int done = 0;
        while (done < budget) {
            done += cpu8048_step(&sys->cpu);
            /* level-triggered: statusread in de instructie kan de
             * VDC-IRQ juist gecleard hebben */
            cpu8048_set_irq(&sys->cpu, vdc8244_irq_asserted(&sys->vdc));
        }
        if (done > budget)
            sys->cyc_debt = done - budget; /* 2-cycle-instructie over de rand */

        /* na de CPU-burst: lijn renderen -> mid-line writes van deze
         * lijn zijn zichtbaar (V1); collision + audio-tick inbegrepen */
        vdc8244_render_line(&sys->vdc);
    }

    sys->frames++;
}

/* ------------------------------------------------------------------ */
/* video / audio                                                       */
/* ------------------------------------------------------------------ */

const uint32_t *g7k_framebuffer(const g7k_sys *sys)
{
    return sys ? sys->fb : NULL;
}

int g7k_fb_width(const g7k_sys *sys)  { (void)sys; return G7K_FB_W; }
int g7k_fb_height(const g7k_sys *sys) { (void)sys; return G7K_FB_H; }

int g7k_audio_read(g7k_sys *sys, int16_t *out, int max_samples)
{
    if (!sys || !out || max_samples <= 0)
        return 0;
    return vdc8244_audio_read(&sys->vdc, out, max_samples);
}

int g7k_audio_sample_rate(const g7k_sys *sys)
{
    if (sys && sys->region == G7K_REGION_NTSC)
        return G7K_NTSC_AUDIO_RATE;
    return G7K_PAL_AUDIO_RATE;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

void g7k_joystick_set(g7k_sys *sys, int player, uint8_t mask)
{
    if (sys && (player == 0 || player == 1))
        sys->joy_pending[player] = mask;
}

void g7k_key_set(g7k_sys *sys, uint8_t matrix_code, bool down)
{
    if (!sys || matrix_code >= 64)
        return;
    uint8_t row = (uint8_t)(matrix_code >> 3);
    uint8_t bit = (uint8_t)(1u << (matrix_code & 0x07));
    if (down)
        sys->keys_pending[row] |= bit;
    else
        sys->keys_pending[row] &= (uint8_t)~bit;
}

uint8_t g7k_key_from_char(char c)
{
    /* S4: teken -> matrixcode (rij*8 + kolom) van het O2/G7000-membraan.
     * Rij-indeling per scanrij (gedragsfeit MAME odyssey2.cpp KEY.0-KEY.5;
     * rijen 6-7 bestaan maar zijn leeg):
     *   0: 0 1 2 3 4 5 6 7
     *   1: 8 9 . . SPC ? L P     (kolom 2-3 niet aangesloten)
     *   2: + W E R T U I O
     *   3: Q S D F G H J K
     *   4: A Z X C V B M .
     *   5: - * / = Y N CLR ENT   (CLR='\b' 0x08, ENT='\n'/'\r')
     * Kolomantwoord loopt via de 74148-encoder in sys_read_p2. */
    static const char rows[6][9] = {
        "01234567", "89\0\0 ?LP", "+WERTUIO",
        "QSDFGHJK", "AZXCVBM.", "-*/=YN\b\n",
    };
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c == '\r')
        c = '\n';
    for (int r = 0; r < 6; r++)
        for (int k = 0; k < 8; k++)
            if (rows[r][k] == c && c != '\0')
                return (uint8_t)(r * 8 + k);
    return G7K_KEY_NONE;
}

/* ------------------------------------------------------------------ */
/* savestate (delegatie naar state.c)                                  */
/* ------------------------------------------------------------------ */

size_t g7k_state_size(const g7k_sys *sys)
{
    return sys ? g7k_state_blob_size(sys) : 0;
}

int g7k_state_save(const g7k_sys *sys, uint8_t *buf, size_t size)
{
    if (!sys || !buf)
        return -1;
    return g7k_state_blob_save(sys, buf, size);
}

int g7k_state_load(g7k_sys *sys, const uint8_t *buf, size_t size)
{
    if (!sys || !buf)
        return -1;
    return g7k_state_blob_load(sys, buf, size);
}

/* ------------------------------------------------------------------ */
/* introspectie / test-hooks                                           */
/* ------------------------------------------------------------------ */

uint64_t g7k_cycle_count(const g7k_sys *sys)
{
    return sys ? sys->cpu.cycles : 0;
}

uint8_t g7k_bus_peek(const g7k_sys *sys, uint16_t addr)
{
    if (!sys)
        return 0xFF;
    addr &= 0x0FFF;
    if (addr < 0x400)
        return sys->bios_loaded ? sys->bios[addr] : 0xFF;
    return g7k_cart_read(&sys->cart, addr);
}

uint8_t g7k_intram_peek(const g7k_sys *sys, uint8_t addr)
{
    return sys ? sys->cpu.iram[addr & 0x3F] : 0xFF;
}

uint8_t g7k_extram_peek(const g7k_sys *sys, uint8_t addr)
{
    return sys ? sys->extram[addr & 0x7F] : 0xFF;
}

uint8_t g7k_vdc_peek(const g7k_sys *sys, uint8_t reg)
{
    return sys ? vdc8244_peek(&sys->vdc, reg) : 0xFF;
}

const char *g7k_version(void)
{
    return G7K_VERSION_STRING;
}
