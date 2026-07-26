/*
 * vdc8244.c — Intel 8244 (NTSC) / 8245 (PAL) VDC: scanline-renderer,
 * collision-bitmatrix, VBLANK/HBLANK/IRQ, audio (toon + ruis + lowpass).
 *
 * QUIRKS-dekking: V1..V12, A1..A3 (docs/QUIRKS.md). Interface en
 * kloktopologie: src/core_internal.h (bindend contract, niet gewijzigd).
 *
 * 100% from scratch. GEEN code/tabellen uit O2EM, MAME of andere emulators.
 * Gedrags-/registerfeiten komen uit publieke technische documentatie:
 *
 *  [B] "Odyssey 2 Technical Specs V1.1", Daniel Boris, 11/98 (publiek,
 *      atarihq.com/danb/files/o2doc.pdf) — registerkaart $00-$FF:
 *      sprites $00-$0F (Y,X,ctrl,unused) + shapes $80-$9F (byte=rij,
 *      bit=kolom); chars $10-$3F (Y,X,ptr,b3: bit0=ptr-bit8, b1-3=kleur);
 *      quads $40-$7F (16B/groep, positie uit de LAATSTE sub-char, "one
 *      character wide space between each"); $A0 control (b0 hor-IRQ,
 *      b1 pos-latch, b2 sound-IRQ, b3 grid aan + registerblokkade,
 *      b4 ext-overlap, b5 foreground aan + registerblokkade, b6 dot-grid,
 *      b7 fill); $A1 status (b0 HBL-status, b1 strobe, b2 sound empty,
 *      b3 VBL-puls, b6 ext, b7 char-overlap); $A2 collision
 *      (W=selecteer/R=detecteer: b0-3 sprites, b4 vert-grid,
 *      b5 hor-grid+dots, b6 ext, b7 chars); $A3 kleur (b0-2 grid,
 *      b3-5 achtergrond, b6 grid-luminantie); $A4/$A5 beam Y/X;
 *      $A7-$A9 24-bit sound-shiftregister + $AA (b0-3 volume, b4 noise,
 *      b5 shiftklok 983/3933 Hz, b6 recirculate, b7 enable);
 *      grid $C0-$C8 (kolom, bit=hor.lijn 0-7), $D0-$D8 (bit0=hor.lijn 8),
 *      $E0-$E9 (10 verticale lijnen, bit=rij); Appendix B kleurnamen;
 *      Appendix C charset-VOLGORDE (64 tekens).
 *  [K] Kloktopologie/htotal/vtotal: zie bronnenblok in core_internal.h
 *      (MCS-48 UM + MAME-gedragsfeiten; geen code overgenomen).
 *
 * CHARSET-HERKOMST (V9): de 64-tekens-VOLGORDE (0-9,:,$,spatie,?,L,P,+,W,
 * E,R,T,U,I,O,Q,S,D,F,G,H,J,K,A,Z,X,C,V,B,M,.,-,x,÷,=,Y,N,/,blok,"10",
 * bal, mannetjes, pijl, boom, hellingen, \, schepen, vliegtuig) komt uit
 * [B] Appendix C. De pixelpatronen hieronder zijn EIGEN ontwerp (van
 * scratch getekende 5x7-glyphs naar de gedocumenteerde tekenNAMEN) —
 * er is geen glyph-dump uit een emulator of chip gebruikt.
 *
 * PALET: kleurNAMEN per index uit [B] Appendix B (char/sprite-tabel en
 * dim/bright achtergrond/grid-tabellen). De RGBA-hexwaarden zijn eigen
 * benaderingen van die gedocumenteerde namen (de 8244 levert digitale
 * R/G/B + luminantie aan de encoder; exacte analoge tinten verschillen
 * per TV). Bit-decodering: achtergrondkleur LSB=blauw (1=donkerblauw,
 * 2=groen, 4=rood per [B]); char/sprite-kleur LSB=rood (1=rood, 2=groen,
 * 4=blauw per [B]; index 6 heet daar "light grey", de RGB-decodering
 * G+B geeft cyaan — wij renderen cyaan en vermelden het verschil).
 */
#include <string.h>

#include "core_internal.h"

/* -------- registerkaart-constanten (bron [B]) -------- */
#define VREG_CTRL   0xA0
#define VREG_STAT   0xA1
#define VREG_COLL   0xA2
#define VREG_COLOR  0xA3
#define VREG_Y      0xA4
#define VREG_X      0xA5
#define VREG_SND0   0xA7    /* shiftregister bits 0-7 (uitgangszijde)   */
#define VREG_SND1   0xA8    /* bits 8-15                                */
#define VREG_SND2   0xA9    /* bits 16-23                               */
#define VREG_SNDCTL 0xAA

#define CTRL_HIRQ   0x01    /* IRQ bij elke HBLANK                      */
#define CTRL_LATCH  0x02    /* beampositie latchen in $A4/$A5           */
#define CTRL_SNDIRQ 0x04    /* sound-IRQ (niet aan INT gekoppeld, zie   */
                            /* open punt in commentaar bij audio)       */
#define CTRL_GRID   0x08    /* grid aan; gridregs geblokkeerd (V2)      */
#define CTRL_FG     0x20    /* foreground aan; objectregs geblokt (V2)  */
#define CTRL_DOTS   0x40    /* dot-grid (V10)                           */
#define CTRL_FILL   0x80    /* fill rechts van elke aan-verticale lijn  */

#define STAT_HBL    0x01    /* 1 = scan actief, 0 = HBLANK ([B] 4.7)    */
#define STAT_STROBE 0x02
#define STAT_SNDEMP 0x04
#define STAT_VBL    0x08
#define STAT_CHROVL 0x80

#define COLL_SP0    0x01
#define COLL_VGRID  0x10
#define COLL_HGRID  0x20    /* horizontale gridlijnen + dots ([B] 4.8)  */
#define COLL_CHARS  0x80

#define SND_VOLMASK 0x0F
#define SND_NOISE   0x10
#define SND_FREQ    0x20    /* 0 = 983 Hz, 1 = 3933 Hz shiftklok ([B])  */
#define SND_RECIRC  0x40
#define SND_ENABLE  0x80

/* Shiftklok-afleiding (A1): 3933 Hz = lijnfrequentie/4, 983 Hz =
 * lijnfrequentie/16 (NTSC 15734 Hz; PAL deelt dezelfde delers op zijn
 * eigen lijnfrequentie). Wij klokken dus 1 shift per 4 resp. 16 lijnen. */
#define SND_DIV_FAST 4
#define SND_DIV_SLOW 16

/* audio_shift-layout: bits 0-23 = het 24-bit schuifregister ([B] 4.10),
 * bits 24-27 = lijn-deler voor de shiftklok (interne prescaler; past in
 * het bestaande struct-veld zodat savestates hem meenemen). */
#define SND_SR_MASK  0x00FFFFFFu
#define SND_DIV_SH   24

/* -------- grid-geometrie --------
 * 8 rijen x 9 kolommen dozen ([B] 4.2). Celmaat 16 VDC-px breed x 24
 * scanlijnen hoog, oorsprong (8,24) in 160x242 VDC-ruimte; horizontale
 * lijnen 3 scanlijnen dik, verticale lijnen 2 VDC-px breed. (Afmetingen
 * afgeleid uit de publieke timing/verhoudingen; exacte randoffsets zijn
 * v0.1-canon en zitten vast in de V10-test.) */
#define GRID_X0     8
#define GRID_Y0     24
#define GRID_CELL_W 16
#define GRID_CELL_H 24
#define GRID_HTHICK 3
#define GRID_VWIDTH 2
#define GRID_COLS   9       /* dozen per rij; 10 verticale lijnen E0-E9 */
#define GRID_ROWS   8       /* dozen per kolom; 9 hor. lijnen (C+D)     */

/* -------- palet (herkomst: zie kop) -------- */
/* fb-layout: RGBA8888 little-endian woord 0xAABBGGRR (A hoogste byte,
 * conform sys.c/stubs: 0xFF000000 = opaak zwart). */
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | \
                      ((uint32_t)(g) << 8) | (uint32_t)(r))

/* achtergrond/grid dim — LSB = blauw. Kleurwaarden volgen de digitale
 * RGB-bitdecodering (bit0=B, bit1=G, bit2=R), NIET letterlijk de
 * observatienamen in [B] App. B: index 3 heet daar "Light Green" maar
 * decodeert als G+B en wordt hier blauwgroen 0x00A0A0 gerenderd
 * (gedeclareerde afwijking, QUIRKS noot 6). Overige namen: zwart,
 * donkerblauw, donkergroen, rood, violet, oranje/kaki, lichtgrijs. */
static const uint32_t k_pal_bg_dim[8] = {
    RGB(0x00, 0x00, 0x00), RGB(0x00, 0x10, 0xA0),
    RGB(0x00, 0xA0, 0x00), RGB(0x00, 0xA0, 0xA0),
    RGB(0xA0, 0x00, 0x00), RGB(0xA0, 0x00, 0xA0),
    RGB(0xA0, 0x60, 0x00), RGB(0xA0, 0xA0, 0xA0),
};

/* achtergrond/grid bright (zelfde volgorde, felle set). Index 0 heet in
 * [B] App. B "Black", maar op echt ijzer tilt de luminantie-bit het
 * hele beeld op ("alles fel", QUIRKS V11 / Killer Bees!-intro) — wij
 * renderen donkergrijs 0x404040 (gedeclareerde afwijking, noot 6). */
static const uint32_t k_pal_bg_bright[8] = {
    RGB(0x40, 0x40, 0x40), RGB(0x40, 0x60, 0xFF),
    RGB(0x40, 0xFF, 0x40), RGB(0x40, 0xFF, 0xFF),
    RGB(0xFF, 0x40, 0x40), RGB(0xFF, 0x40, 0xFF),
    RGB(0xFF, 0xA0, 0x40), RGB(0xFF, 0xFF, 0xFF),
};

/* char/sprite-kleuren ([B] App. B: donkergrijs, rood, groen, oranje,
 * blauw, violet, [cyaan], wit) — LSB = rood */
static const uint32_t k_pal_obj[8] = {
    RGB(0x70, 0x70, 0x70), RGB(0xFF, 0x40, 0x40),
    RGB(0x40, 0xFF, 0x40), RGB(0xFF, 0xA0, 0x40),
    RGB(0x40, 0x60, 0xFF), RGB(0xFF, 0x40, 0xFF),
    RGB(0x40, 0xFF, 0xFF), RGB(0xFF, 0xFF, 0xFF),
};

/* -------- charset (V9): volgorde uit [B] App. C, glyphs eigen ontwerp.
 * 64 tekens x 8 bytes; bit 7 = linkerkolom binnen een glyph-rij. -------- */
static const uint8_t k_charset[64 * 8] = {
    /* 00 '0' */ 0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00,
    /* 01 '1' */ 0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00,
    /* 02 '2' */ 0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00,
    /* 03 '3' */ 0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00,
    /* 04 '4' */ 0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00,
    /* 05 '5' */ 0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00,
    /* 06 '6' */ 0x70,0x88,0x80,0xF0,0x88,0x88,0x70,0x00,
    /* 07 '7' */ 0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00,
    /* 08 '8' */ 0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00,
    /* 09 '9' */ 0x70,0x88,0x88,0x78,0x08,0x88,0x70,0x00,
    /* 10 ':' */ 0x00,0x30,0x30,0x00,0x30,0x30,0x00,0x00,
    /* 11 '$' */ 0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00,
    /* 12 ' ' */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 13 '?' */ 0x70,0x88,0x08,0x10,0x20,0x00,0x20,0x00,
    /* 14 'L' */ 0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00,
    /* 15 'P' */ 0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00,
    /* 16 '+' */ 0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00,
    /* 17 'W' */ 0x88,0x88,0x88,0xA8,0xA8,0xA8,0x50,0x00,
    /* 18 'E' */ 0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00,
    /* 19 'R' */ 0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00,
    /* 20 'T' */ 0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00,
    /* 21 'U' */ 0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00,
    /* 22 'I' */ 0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00,
    /* 23 'O' */ 0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00,
    /* 24 'Q' */ 0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00,
    /* 25 'S' */ 0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00,
    /* 26 'D' */ 0xF0,0x88,0x88,0x88,0x88,0x88,0xF0,0x00,
    /* 27 'F' */ 0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00,
    /* 28 'G' */ 0x70,0x88,0x80,0xB8,0x88,0x88,0x78,0x00,
    /* 29 'H' */ 0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00,
    /* 30 'J' */ 0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00,
    /* 31 'K' */ 0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00,
    /* 32 'A' */ 0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00,
    /* 33 'Z' */ 0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00,
    /* 34 'X' */ 0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00,
    /* 35 'C' */ 0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00,
    /* 36 'V' */ 0x88,0x88,0x88,0x88,0x88,0x50,0x20,0x00,
    /* 37 'B' */ 0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00,
    /* 38 'M' */ 0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00,
    /* 39 '.' */ 0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00,
    /* 40 '-' */ 0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00,
    /* 41 'x' (maal) */ 0x00,0x88,0x50,0x20,0x50,0x88,0x00,0x00,
    /* 42 (deel)     */ 0x00,0x20,0x00,0xF8,0x00,0x20,0x00,0x00,
    /* 43 '=' */ 0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00,
    /* 44 'Y' */ 0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00,
    /* 45 'N' */ 0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00,
    /* 46 '/' */ 0x08,0x08,0x10,0x20,0x40,0x80,0x80,0x00,
    /* 47 blok */ 0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0x00,
    /* 48 "10" */ 0x5C,0x54,0x54,0x54,0x54,0x54,0x5C,0x00,
    /* 49 bal  */ 0x70,0xF8,0xF8,0xF8,0x70,0x00,0x00,0x00,
    /* 50 man rechts       */ 0x30,0x30,0x20,0x78,0x20,0x50,0x88,0x00,
    /* 51 man rechts loop  */ 0x30,0x30,0x20,0x78,0x20,0x50,0x50,0x00,
    /* 52 man links loop   */ 0x18,0x18,0x08,0x3C,0x08,0x14,0x14,0x00,
    /* 53 man links        */ 0x18,0x18,0x08,0x3C,0x08,0x14,0x22,0x00,
    /* 54 pijl rechts */ 0x00,0x20,0x10,0xF8,0x10,0x20,0x00,0x00,
    /* 55 boom  */ 0x20,0x70,0xF8,0x70,0xF8,0x20,0x20,0x00,
    /* 56 helling links  */ 0x80,0xC0,0xE0,0xF0,0xF8,0x00,0x00,0x00,
    /* 57 helling rechts */ 0x08,0x18,0x38,0x78,0xF8,0x00,0x00,0x00,
    /* 58 man voorover */ 0x20,0x70,0xA8,0x20,0x20,0x50,0x88,0x00,
    /* 59 '\' */ 0x80,0x80,0x40,0x20,0x10,0x08,0x08,0x00,
    /* 60 schip 1 */ 0x20,0x70,0x70,0xF8,0xD8,0x00,0x00,0x00,
    /* 61 vliegtuig */ 0x08,0x1C,0xFE,0x1C,0x08,0x00,0x00,0x00,
    /* 62 schip 2 */ 0x20,0x70,0xA8,0xF8,0x88,0x00,0x00,0x00,
    /* 63 schip 3 */ 0x70,0xD8,0xF8,0xA8,0x20,0x00,0x00,0x00,
};

/* ==================================================================== */
/* levenscyclus                                                          */
/* ==================================================================== */

void vdc8244_init(vdc8244 *vdc, g7k_region region, uint32_t *fb,
                  int fb_w, int fb_h)
{
    memset(vdc, 0, sizeof(*vdc));
    vdc->region = region;
    vdc->fb = fb;
    vdc->fb_w = fb_w;
    vdc->fb_h = fb_h;
    vdc8244_reset(vdc, true);
}

void vdc8244_reset(vdc8244 *vdc, bool cold)
{
    /* S1: de VDC hangt NIET aan de reset-knop (geen reset-lijn); warm
     * reset laat alle VDC-state staan. Alleen power-cycle wist. */
    if (!cold)
        return;
    memset(vdc->regs, 0, sizeof(vdc->regs));
    vdc->status = 0;
    vdc->collision = 0;
    vdc->irq = false;
    vdc->bright = false;
    vdc->line = 0;
    vdc->audio_shift = 0;
    vdc->audio_lfsr = 0x7FFF;   /* LFSR-seed: nooit de all-zero lockup   */
    vdc->sample_count = 0;
}

void vdc8244_set_region(vdc8244 *vdc, g7k_region region)
{
    vdc->region = region;
}

void vdc8244_set_bright(vdc8244 *vdc, bool bright)
{
    /* V11: P17 laag = luminantie-ingang actief -> achtergrond en dim-
     * gridkleuren renderen uit de felle tabel (Killer Bees!-intro). */
    vdc->bright = bright;
}

/* ==================================================================== */
/* registertoegang (V2-blokkade, V12-leesgedrag)                         */
/* ==================================================================== */

static bool reg_is_object(uint8_t reg)
{
    return reg <= 0x9F;         /* sprites+chars+quads+shapes ([B] 4.6) */
}

static bool reg_is_grid(uint8_t reg)
{
    return (reg >= 0xC0 && reg <= 0xC8) ||
           (reg >= 0xD0 && reg <= 0xD8) ||
           (reg >= 0xE0 && reg <= 0xE9);
}

static bool reg_is_defined(uint8_t reg)
{
    if (reg_is_object(reg) || reg_is_grid(reg))
        return true;
    switch (reg) {
    case VREG_CTRL: case VREG_STAT: case VREG_COLL: case VREG_COLOR:
    case VREG_Y:    case VREG_X:
    case VREG_SND0: case VREG_SND1: case VREG_SND2: case VREG_SNDCTL:
        return true;
    default:
        return false;           /* $A6, $AB-$BF, $C9, $D9-$DF, $EA-$FF */
    }
}

void vdc8244_write(vdc8244 *vdc, uint8_t reg, uint8_t data)
{
    uint8_t ctrl = vdc->regs[VREG_CTRL];

    /* V2: echt ijzer negeert writes op objectregisters zolang de
     * foreground aanstaat, en op gridregisters zolang het grid aanstaat
     * ([B] 4.6: "cannot be changed when this is set to 1"). Geen
     * opt-out: homebrew dat op de oude emulator-afwijking leunt
     * ("Puzzle Piece Panic emu-versie") is een ongeldige testcase. */
    if (reg_is_object(reg) && (ctrl & CTRL_FG))
        return;
    if (reg_is_grid(reg) && (ctrl & CTRL_GRID))
        return;

    switch (reg) {
    case VREG_CTRL:
        /* 0->1-flank op de latch-bit bevriest de beampositie in $A4/$A5 */
        if ((data & CTRL_LATCH) && !(ctrl & CTRL_LATCH)) {
            vdc->regs[VREG_Y] = (uint8_t)vdc->line;
            vdc->regs[VREG_X] = 0;  /* geen intra-lijn-fase beschikbaar */
        }
        vdc->regs[VREG_CTRL] = data;
        break;
    case VREG_STAT:
        break;                  /* status is read-only                  */
    case VREG_COLL:
        /* schrijf = klassen selecteren ([B] 4.8); nieuwe meting start */
        vdc->regs[VREG_COLL] = data;
        vdc->collision = 0;
        break;
    case VREG_SND0:
        vdc->audio_shift = (vdc->audio_shift & ~0x000000FFu) | data;
        vdc->regs[reg] = data;
        break;
    case VREG_SND1:
        vdc->audio_shift = (vdc->audio_shift & ~0x0000FF00u) |
                           ((uint32_t)data << 8);
        vdc->regs[reg] = data;
        break;
    case VREG_SND2:
        vdc->audio_shift = (vdc->audio_shift & ~0x00FF0000u) |
                           ((uint32_t)data << 16);
        vdc->regs[reg] = data;
        break;
    default:
        vdc->regs[reg] = data;
        break;
    }
}

uint8_t vdc8244_read(vdc8244 *vdc, uint8_t reg)
{
    uint8_t ctrl = vdc->regs[VREG_CTRL];

    switch (reg) {
    case VREG_STAT: {
        /* V12: statusread cleart de VBL-latch en de INT-lijn (contract
         * core_internal.h; [B] 4.7 beschrijft de bitindeling). HBL-bit
         * intra-lijn: 1 = actieve scan, 0 = HBLANK — de beam-fase komt
         * uit line_phase (door sys.c gezet vóór elke MOVX-read, zie
         * vdc8244_set_line_phase); tijdens VBLANK altijd 0. */
        uint8_t val = vdc->status;
        if (vdc->line < G7K_VBLANK_START_LINE &&
            !vdc8244_hblank_at(vdc, vdc->line_phase))
            val |= STAT_HBL;
        /* [B] 4.7 bit 1: "1 = Follow Beam, 0 = Latched" — de bit is dus
         * HOOG zolang er NIET gelatcht wordt (review-finding 2026-07-26:
         * eerdere versie had de polariteit omgekeerd). */
        if (!(ctrl & CTRL_LATCH))
            val |= STAT_STROBE;
        if ((vdc->audio_shift & SND_SR_MASK) == 0)
            val |= STAT_SNDEMP;
        vdc->status &= (uint8_t)~STAT_VBL;
        vdc->irq = false;
        return val;
    }
    case VREG_COLL: {
        /* V3: read latcht het meetresultaat en wist de accumulator */
        uint8_t val = vdc->collision;
        vdc->collision = 0;
        return val;
    }
    case VREG_Y:
        return (ctrl & CTRL_LATCH) ? vdc->regs[VREG_Y]
                                   : (uint8_t)vdc->line;
    case VREG_X:
        return (ctrl & CTRL_LATCH) ? vdc->regs[VREG_X] : 0x00;
    default:
        /* V12: object-/gridregisters zijn ook voor LEZEN afgekoppeld
         * zolang hun renderpad actief is (zelfde interne bus-arbitrage
         * als de V2-schrijfblokkade); ongebruikte adressen lezen 0x00. */
        if (reg_is_object(reg) && (ctrl & CTRL_FG))
            return 0x00;
        if (reg_is_grid(reg) && (ctrl & CTRL_GRID))
            return 0x00;
        if (!reg_is_defined(reg))
            return 0x00;
        return vdc->regs[reg];
    }
}

uint8_t vdc8244_peek(const vdc8244 *vdc, uint8_t reg)
{
    /* side-effect-vrij (g7k_vdc_peek/tests): ruwe latches; status en
     * collision tonen de interne accumulatoren zonder te clearen. */
    if (reg == VREG_STAT)
        return vdc->status;
    if (reg == VREG_COLL)
        return vdc->collision;
    return vdc->regs[reg];
}

/* ==================================================================== */
/* scanline-status (V1-lockstep, C7-T1)                                  */
/* ==================================================================== */

void vdc8244_set_line_phase(vdc8244 *vdc, int cycles_into_line)
{
    /* beam-fase binnen de huidige lijn (CPU-machinecycli sinds
     * lijnstart); sys.c zet dit vóór elke VDC-registertoegang zodat
     * het HBL-statusbit ([B] 4.7 bit 0) intra-lijn klopt. */
    vdc->line_phase = cycles_into_line;
}

void vdc8244_begin_line(vdc8244 *vdc, int line)
{
    vdc->line = line;
    vdc->line_phase = 0;
    if (line == G7K_VBLANK_START_LINE) {
        vdc->status |= STAT_VBL;    /* VBL-puls ([B] 4.7)               */
        vdc->irq = true;            /* externe IRQ -> 8048 INT (C5)     */
    }
    /* optionele horizontale interrupt ([B] 4.6 bit 0) */
    if ((vdc->regs[VREG_CTRL] & CTRL_HIRQ) &&
        line < G7K_VBLANK_START_LINE)
        vdc->irq = true;
}

bool vdc8244_vblank(const vdc8244 *vdc)
{
    return vdc->line >= G7K_VBLANK_START_LINE;
}

bool vdc8244_hblank_at(const vdc8244 *vdc, int cycles_into_line)
{
    /* CPU-machinecycli -> pixelklokken. Exact geheeltallig (zie
     * kloktopologie in core_internal.h): NTSC 455 px / 22.75 cyc = 20
     * px per cyclus; PAL 456 px / (76/3) cyc = 18 px per cyclus. */
    int per_cycle = (vdc->region == G7K_REGION_NTSC) ? 20 : 18;
    int htotal    = (vdc->region == G7K_REGION_NTSC) ? 455 : 456;
    int px = (cycles_into_line * per_cycle) % htotal;
    if (px < 0)
        px += htotal;
    return px >= G7K_HBLANK_START_PX;
}

bool vdc8244_irq_asserted(const vdc8244 *vdc)
{
    return vdc->irq;
}

/* ==================================================================== */
/* scanline-renderer                                                     */
/* ==================================================================== */

typedef struct lctx {
    vdc8244  *v;
    uint32_t *lb;               /* lijn-kleurbuffer (fb-resolutie)      */
    uint8_t  *cb;               /* lijn-klassenmasker voor collision    */
    int       w;                /* bruikbare breedte                    */
} lctx;

/* Eén fb-pixel zetten: kleur (painter's order = prioriteit, V6),
 * collision-accumulatie (V3) en char-overlap-status. Clipt hard aan de
 * randen — objecten wrappen NOOIT (V7). */
static void put_px(lctx *c, int x, uint32_t col, uint8_t cls)
{
    uint8_t hits, sel;

    if (x < 0 || x >= c->w)
        return;
    hits = c->cb[x];
    if (hits) {
        sel = c->v->regs[VREG_COLL];
        /* gerapporteerd wordt elke klasse die een GESELECTEERDE klasse
         * raakt ([B] 4.8: W = enable, R = detect) */
        if (cls & sel)
            c->v->collision |= hits;
        if (hits & sel)
            c->v->collision |= cls;
        if (cls & hits & COLL_CHARS)
            c->v->status |= STAT_CHROVL;    /* [B] 4.7 bit 7 */
    }
    c->cb[x] |= cls;
    c->lb[x] = col;
}

/* Horizontale run van VDC-pixels (fb = 2x horizontaal verdubbeld). */
static void put_run_vdc(lctx *c, int x_vdc, int w_vdc, uint32_t col,
                        uint8_t cls)
{
    int x;
    for (x = x_vdc * 2; x < (x_vdc + w_vdc) * 2; x++)
        put_px(c, x, col, cls);
}

/* Gemeenschappelijk teken-pad voor single chars én quad-subchars.
 * Charset-adressering: 9-bit bytepointer (b3-bit0 = bit 8, [B] 4.4);
 * de rij-teller telt per 2 scanlijnen op bij de pointer (glyphrijen
 * worden verticaal verdubbeld), 9-bit wrap over de 512-byte charset. */
static void draw_char_slice(lctx *c, int line, int x_vdc, int y,
                            uint16_t ptr9, uint8_t colidx)
{
    int row, b;
    uint8_t bits;
    uint32_t col;

    if (line < y)
        return;
    row = (line - y) >> 1;
    if (row >= 8)
        return;
    bits = k_charset[(uint16_t)(ptr9 + (uint16_t)row) & 0x1FF];
    col = k_pal_obj[colidx & 7];
    for (b = 0; b < 8; b++)
        if (bits & (uint8_t)(0x80u >> b))       /* charset: bit7=links  */
            put_run_vdc(c, x_vdc + b, 1, col, COLL_CHARS);
}

static void draw_chars(lctx *c, int line)
{
    int i;
    for (i = 0; i < 12; i++) {
        const uint8_t *r = &c->v->regs[0x10 + 4 * i];
        uint16_t ptr9 = (uint16_t)(r[2] | ((uint16_t)(r[3] & 0x01) << 8));
        draw_char_slice(c, line, r[1], r[0], ptr9, (uint8_t)(r[3] >> 1));
    }
}

static void draw_quads(lctx *c, int line)
{
    /* V5: quad-chars hebben een EIGEN tekenpad: 4 sub-chars naast
     * elkaar; Y en X komen uit de LAATSTE sub-char en de pitch is
     * 16 VDC-px ("one character wide space between each", [B] 4.5). */
    int q, j;
    for (q = 0; q < 4; q++) {
        const uint8_t *base = &c->v->regs[0x40 + 16 * q];
        uint8_t y = base[12];
        uint8_t x = base[13];
        for (j = 0; j < 4; j++) {
            const uint8_t *r = &base[4 * j];
            uint16_t ptr9 =
                (uint16_t)(r[2] | ((uint16_t)(r[3] & 0x01) << 8));
            draw_char_slice(c, line, (int)x + 16 * j, y, ptr9,
                            (uint8_t)(r[3] >> 1));
        }
    }
}

static void draw_sprite(lctx *c, int line, int n)
{
    const uint8_t *r = &c->v->regs[4 * n];
    uint8_t ctl = r[2];
    int y = r[0];
    int x = r[1];
    bool dbl = (ctl & 0x04) != 0;
    int zoom = dbl ? 2 : 1;         /* VDC-px per shape-pixel           */
    int rowspan = 2 * zoom;         /* scanlijnen per shape-rij         */
    int row, b, shift;
    uint8_t bits;
    uint32_t col;

    /* V4: double-size sprites positioneren per 2 pixels (X-bit 0 doet
     * niet mee zodra de zoom-teller op halve snelheid loopt). */
    if (dbl)
        x &= ~1;

    if (line < y)
        return;
    row = (line - y) / rowspan;
    if (row >= 8)
        return;

    bits = c->v->regs[0x80 + 8 * n + row];
    col = k_pal_obj[(ctl >> 3) & 7];

    /* V8: shifted/smooth-modus ([B] 4.3.1): bit 0 schuift de hele
     * sprite 1 px rechts; bit 1 schuift alleen de EVEN shape-rijen
     * 1 px rechts (pseudo-hi-res randen, Q*bert). */
    shift = (ctl & 0x01) ? 1 : 0;
    if ((ctl & 0x02) && (row % 2) == 0)
        shift += 1;

    for (b = 0; b < 8; b++)
        if (bits & (uint8_t)(1u << b))          /* shapes: bit0=links   */
            put_run_vdc(c, x + shift + b * zoom, zoom, col,
                        (uint8_t)(COLL_SP0 << n));
}

static void draw_grid(lctx *c, int line)
{
    const vdc8244 *v = c->v;
    uint8_t ctrl = v->regs[VREG_CTRL];
    uint8_t a3 = v->regs[VREG_COLOR];
    bool bright = ((a3 & 0x40) != 0) || v->bright;  /* lum-bit of V11   */
    uint32_t col = (bright ? k_pal_bg_bright : k_pal_bg_dim)[a3 & 7];
    int n, cx, r;

    if (ctrl & CTRL_GRID) {
        /* horizontale lijnen 0-7 ($C0-$C8, bit=lijn) en lijn 8
         * ($D0-$D8, bit 0) — per kolom-register, 9 segmenten (V10) */
        for (n = 0; n <= GRID_ROWS; n++) {
            int y = GRID_Y0 + n * GRID_CELL_H;
            if (line < y || line >= y + GRID_HTHICK)
                continue;
            for (cx = 0; cx < GRID_COLS; cx++) {
                bool on = (n < 8)
                    ? (v->regs[0xC0 + cx] & (uint8_t)(1u << n)) != 0
                    : (v->regs[0xD0 + cx] & 0x01) != 0;
                if (on)
                    put_run_vdc(c, GRID_X0 + cx * GRID_CELL_W,
                                GRID_CELL_W + GRID_VWIDTH, col,
                                COLL_HGRID);
            }
        }
        /* verticale lijnen 0-9 ($E0-$E9, bit=rij); segmenthoogte =
         * exact één celhoogte (V10-gridlijn-hoogtes) */
        for (n = 0; n < GRID_COLS + 1; n++) {
            uint8_t bitsv = v->regs[0xE0 + n];
            if (!bitsv)
                continue;
            for (r = 0; r < GRID_ROWS; r++) {
                int y = GRID_Y0 + r * GRID_CELL_H;
                if (!(bitsv & (uint8_t)(1u << r)))
                    continue;
                if (line < y || line >= y + GRID_CELL_H)
                    continue;
                put_run_vdc(c, GRID_X0 + n * GRID_CELL_W, GRID_VWIDTH,
                            col, COLL_VGRID);
                /* fill-mode: doos rechts van elke AAN-verticale lijn
                 * dicht ([B] 4.2/4.6 bit 7; kleur = gridkleur, de twee
                 * tekstplaatsen in [B] spreken elkaar tegen — 4.2
                 * aangehouden) */
                if (ctrl & CTRL_FILL)
                    put_run_vdc(c,
                                GRID_X0 + n * GRID_CELL_W + GRID_VWIDTH,
                                GRID_CELL_W - GRID_VWIDTH, col,
                                COLL_VGRID);
            }
        }
    }

    /* dot-grid ([B] 4.6 bit 6): dots op ALLE kruispunten van het
     * gridraster, dus ook de rechterkolom en de onderste lijn (V10);
     * collision-klasse = horizontale grid ([B] 4.8 bit 5). */
    if (ctrl & CTRL_DOTS) {
        for (n = 0; n <= GRID_ROWS; n++) {
            int y = GRID_Y0 + n * GRID_CELL_H;
            if (line < y || line >= y + GRID_HTHICK)
                continue;
            for (cx = 0; cx <= GRID_COLS; cx++)
                put_run_vdc(c, GRID_X0 + cx * GRID_CELL_W, GRID_VWIDTH,
                            col, COLL_HGRID);
        }
    }
}

/* -------- audio (A1/A2/A3) -------- */

static void audio_line(vdc8244 *vdc)
{
    uint8_t aa = vdc->regs[VREG_SNDCTL];
    int divider = (int)((vdc->audio_shift >> SND_DIV_SH) & 0x0F);
    int period = (aa & SND_FREQ) ? SND_DIV_FAST : SND_DIV_SLOW;
    int16_t target = 0;
    int i;

    /* A1: shiftklok = lijnfrequentie/4 of /16 (983/3933 Hz op NTSC;
     * PAL leidt dezelfde delers van zijn eigen lijnfrequentie af). */
    divider++;
    if (divider >= period) {
        divider = 0;
        if (aa & SND_ENABLE) {
            uint32_t sr = vdc->audio_shift & SND_SR_MASK;
            uint32_t out = sr & 1u;
            sr >>= 1;
            if (aa & SND_RECIRC)
                sr |= out << 23;
            vdc->audio_shift = (vdc->audio_shift & ~SND_SR_MASK) | sr;
            /* A2: ruis-LFSR, 15-bit maximal length x^15+x^14+1 (eigen
             * keuze; het echte chip-polynoom is niet publiek
             * gedocumenteerd — gedrag: witte ruis). Klokt mee op de
             * geselecteerde shiftklok. */
            if (vdc->audio_lfsr == 0)
                vdc->audio_lfsr = 0x7FFF;
            {
                uint16_t fb =
                    (uint16_t)((vdc->audio_lfsr ^
                                (vdc->audio_lfsr >> 1)) & 1u);
                vdc->audio_lfsr =
                    (uint16_t)((vdc->audio_lfsr >> 1) | (fb << 14));
            }
        }
    }
    vdc->audio_shift = (vdc->audio_shift & SND_SR_MASK) |
                       ((uint32_t)divider << SND_DIV_SH);

    if (aa & SND_ENABLE) {
        uint32_t bit = (aa & SND_NOISE) ? (vdc->audio_lfsr & 1u)
                                        : (vdc->audio_shift & 1u);
        int vol = aa & SND_VOLMASK;
        target = (int16_t)((bit ? 1 : -1) * vol * 1024);
    }

    /* A3: one-pole lowpass als benadering van de analoge uitgangstrap
     * (RC-gedrag); toestand = laatste sample in de ring, zodat er geen
     * extra struct-veld nodig is. Eerste sample van een frame start op
     * de doelwaarde (geen klik). */
    for (i = 0; i < G7K_AUDIO_SAMPLES_PER_LINE; i++) {
        int16_t prev = (vdc->sample_count > 0)
            ? vdc->samples[vdc->sample_count - 1] : target;
        int16_t out = (int16_t)(prev + (int16_t)((target - prev) / 2));
        if (vdc->sample_count < G7K_AUDIO_RING_SIZE)
            vdc->samples[vdc->sample_count++] = out;
    }
}

void vdc8244_render_line(vdc8244 *vdc)
{
    int l = vdc->line;

    if (l >= 0 && l < G7K_ACTIVE_LINES) {
        uint32_t lb[G7K_FB_W];
        uint8_t cb[G7K_FB_W];
        lctx c;
        uint8_t a3 = vdc->regs[VREG_COLOR];
        const uint32_t *bgpal =
            vdc->bright ? k_pal_bg_bright : k_pal_bg_dim;   /* V11 */
        uint32_t bg = bgpal[(a3 >> 3) & 7];
        int x;

        c.v = vdc;
        c.lb = lb;
        c.cb = cb;
        c.w = (vdc->fb_w < G7K_FB_W) ? vdc->fb_w : G7K_FB_W;

        /* [B] 4.7 bit 7 is tegenwoordige tijd ("set when 2 or more
         * character objects ARE overlapping") — geen eeuwige latch.
         * Scanlijn-granulariteit (v0.1-canon, QUIRKS noot 4): de bit
         * wordt per gerenderde lijn opnieuw bepaald; tijdens VBLANK
         * blijft de waarde van de laatste actieve lijn staan. */
        vdc->status &= (uint8_t)~STAT_CHROVL;

        for (x = 0; x < c.w; x++) {
            lb[x] = bg;
            cb[x] = 0;
        }

        /* vaste prioriteitsstapel (V6): achtergrond < grid < chars/
         * quads < sprites, sprite 0 bovenop (painter's order). */
        draw_grid(&c, l);
        if (vdc->regs[VREG_CTRL] & CTRL_FG) {
            draw_chars(&c, l);
            draw_quads(&c, l);
            draw_sprite(&c, l, 3);
            draw_sprite(&c, l, 2);
            draw_sprite(&c, l, 1);
            draw_sprite(&c, l, 0);
        }

        if (vdc->fb && l < vdc->fb_h)
            memcpy(vdc->fb + (size_t)l * (size_t)vdc->fb_w, lb,
                   (size_t)c.w * sizeof(uint32_t));
    }

    audio_line(vdc);            /* 2 samples per scanlijn, ook in VBL   */
}

/* ==================================================================== */
/* audio-ringbuffer                                                      */
/* ==================================================================== */

int vdc8244_audio_read(vdc8244 *vdc, int16_t *out, int max_samples)
{
    int n = vdc->sample_count;
    if (n > max_samples)
        n = max_samples;
    if (n > 0)
        memcpy(out, vdc->samples, (size_t)n * sizeof(int16_t));
    vdc->sample_count = 0;
    return n;
}
