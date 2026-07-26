/*
 * core_internal.h — interne interfaces VideopacHorse_Core (v0.1)
 *
 * BINDEND CONTRACT tussen de vier subsystemen. Eigenaarschap:
 *
 *   sectie CPU   -> geimplementeerd door src/cpu8048.c   (bouwer A)
 *   sectie VDC   -> geimplementeerd door src/vdc8244.c   (bouwer B)
 *   sectie CART  -> geimplementeerd door src/cart.c      (bouwer C)
 *   sectie STATE -> geimplementeerd door src/state.c     (bouwer C of D)
 *   sectie SYS   -> geimplementeerd door src/sys.c       (architect, KLAAR)
 *
 * Tot de echte implementaties bestaan linkt alles via src/stubs_tmp.c
 * (tijdelijk; de integrator verwijdert dat bestand als alle vier de echte
 * bestanden er zijn).
 *
 * WIJZIGINGSREGEL: een bouwer mag ALLEEN velden toevoegen binnen het
 * struct-blok van zijn eigen subsysteem (nodig voor savestates: state.c
 * serialiseert deze structs). Functiesignaturen wijzigen = overleg met
 * architect + versie-impact (zie ARCHITECTURE.md appendix).
 *
 * Geen globals; geen malloc (alle subsysteem-state ligt embedded in
 * struct g7k_sys, gealloceerd in g7k_create). C11, -Wall -Wextra -Werror.
 *
 * ============================================================================
 * KLOKTOPOLOGIE & FRAME-OPBOUW (onderzoek 2026-07-26, bronnen onderaan blok)
 * ============================================================================
 *
 *  NTSC (Magnavox Odyssey², VDC = Intel 8244):
 *    XTAL             7.159090 MHz  (= 2 x NTSC-kleursubdraaggolf 3.579545 MHz)
 *    CPU-pinklok      XTAL * 3/4        = 5.369318 MHz   [1]
 *    CPU-machinecyclus= pinklok / 15    = XTAL / 20      = 357954.5 Hz  [2]
 *    VDC-klok         XTAL / 2          = 3.579545 MHz   [1]
 *    Pixelklok        VDC-klok * 2      = XTAL           = 7.159090 MHz [3]
 *    Scanline         455 pixelklokken  -> lijnfrequentie XTAL/455 = 15734.3 Hz
 *    Frame            263 lijnen totaal; actief beeld lijn 0..241,
 *                     VBLANK lijn 242..262                              [3]
 *    Framerate        XTAL / (455*263)  = 59.83 Hz
 *    CPU-cycles/lijn  (XTAL/20)/(XTAL/455) = 455/20 = 22.75 = EXACT 91/4
 *    CPU-cycles/frame 263 * 22.75 = 5983.25
 *
 *  PAL (Philips Videopac G7000, VDC = Intel 8245):
 *    XTAL             17.734475 MHz (= 4 x PAL-subdraaggolf 4.43361875 MHz;
 *                     kristal-opdruk varieert: 17.734462/17.734476)
 *    CPU-pinklok      XTAL / 3          = 5.911492 MHz   [1]  (PAL-CPU is dus
 *                     ~10% SNELLER dan NTSC — regiogedrag is meetbaar, C7)
 *    CPU-machinecyclus= pinklok / 15    = XTAL / 45      = 394099.4 Hz  [2]
 *    VDC-klok         XTAL / 5          = 3.546895 MHz   [1]
 *    Pixelklok        VDC-klok * 2      = 7.093790 MHz   [3]
 *    Scanline         456 pixelklokken  -> lijnfrequentie 15556.6 Hz
 *                     (bewust NIET broadcast-PAL 15625 Hz: echt ijzer wijkt af;
 *                      de 8245 staat tijdens VBLANK in slave-mode, timing deels
 *                      extern afgeleid [3] — v0.1 canon = interne 8245-telling)
 *    Frame            313 lijnen totaal; actief beeld lijn 0..241,
 *                     VBLANK lijn 242..312                              [3]
 *    Framerate        ~49.70 Hz
 *    CPU-cycles/lijn  1140/45 = 25.333... = EXACT 76/3
 *    CPU-cycles/frame 313 * 76/3 = 7929.33
 *
 *  Audio-samplerate-afleiding: de VDC produceert G7K_AUDIO_SAMPLES_PER_LINE
 *  samples per scanline-tick -> samplerate = lijnfrequentie * 2:
 *    NTSC 31469 Hz, PAL 31113 Hz (afgerond; frontends resamplen zelf).
 *
 *  Scheduler-consequentie: de cycles-per-lijn-ratio's zijn exact rationaal
 *  (NTSC 91 cycles per 4 lijnen, PAL 76 per 3). sys.c gebruikt een
 *  breuk-accumulator (num/den) + instructie-overshoot-schuld, zodat er over
 *  een heel frame NUL drift is. Zie g7k_run_frame in sys.c.
 *
 *  Bronnen (gedrags-/datasheet-feiten, geen code overgenomen):
 *  [1] MAME src/mame/philips/odyssey2.cpp — klokafleiding op het mainboard:
 *      NTSC 8048 = 7.15909 MHz * 3/4, 8244 = /2; PAL 8048 = 17.734476/3,
 *      8245 = /5. (Niet-voor-de-hand-liggend: veel webbronnen noemen
 *      "1.79 MHz" — dat is de interne state-klok pinklok/3, niet de pinklok.)
 *  [2] Intel MCS-48 User's Manual: 1 machinecyclus = 15 XTAL-perioden
 *      (5 states x 3 klokken); instructies duren 1 of 2 machinecycli.
 *  [3] MAME src/devices/video/i8244.cpp — htotal 455 (8244) / 456 (8245),
 *      vtotal 263/313, vblank_start 242, hblank_start 366, pixelklok = 2x
 *      chip-klok; PAL slave-mode-opmerking. Kruisverifieerbaar met de
 *      8244-datasheet zodra beschikbaar.
 * ============================================================================
 */
#ifndef G7K_CORE_INTERNAL_H
#define G7K_CORE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "g7000.h"

/* -------- timing-constanten (zie kloktopologie hierboven) -------- */
#define G7K_NTSC_XTAL_HZ        7159090
#define G7K_PAL_XTAL_HZ         17734475

#define G7K_NTSC_LINES          263     /* totaal scanlines per frame        */
#define G7K_PAL_LINES           313
#define G7K_ACTIVE_LINES        242     /* lijn 0..241 actief, beide regio's */
#define G7K_VBLANK_START_LINE   242     /* T1/STATUS-VBL + externe IRQ       */

/* CPU-machinecycli per scanline, exact rationaal: num/den */
#define G7K_NTSC_CYC_NUM        91      /* 91/4  = 22.75  cycles per lijn */
#define G7K_NTSC_CYC_DEN        4
#define G7K_PAL_CYC_NUM         76      /* 76/3  = 25.33  cycles per lijn */
#define G7K_PAL_CYC_DEN         3

/* HBLANK begint op pixelklok 366 van 455/456 (bron [3]) ->
 * in CPU-cycles vanaf lijnstart: 366/455*22.75 ~= 18.3 (NTSC),
 * 366/456*25.33 ~= 20.3 (PAL). De VDC rekent dit zelf om (hblank_at). */
#define G7K_HBLANK_START_PX     366

/* framebuffer: 160 VDC-pixels horizontaal verdubbeld naar 320; 240 van de
 * 242 actieve lijnen getoond (2 lijnen overscan-crop onderaan). Runtime
 * opvraagbaar via g7k_fb_width/height, dus later vrij aanpasbaar. */
#define G7K_FB_W                320
#define G7K_FB_H                240

#define G7K_AUDIO_SAMPLES_PER_LINE 2
#define G7K_AUDIO_RING_SIZE     1024    /* > 2*313 samples per frame */
#define G7K_NTSC_AUDIO_RATE     31469   /* 2 * 15734.3 Hz */
#define G7K_PAL_AUDIO_RATE      31113   /* 2 * 15556.6 Hz */

/* ============================================================================
 * CPU — Intel 8048 (MCS-48)                    implementatie: src/cpu8048.c
 * ============================================================================
 * QUIRKS-dekking: C1..C9. Kloktopologie: de CPU kent zijn eigen frequentie
 * NIET — sys.c bepaalt hoeveel machinecycli per scanline draaien; de CPU
 * telt alleen verbruikte cycli per instructie (C6: 1 of 2 per datasheet).
 */

/* Bus-callbacks: alle omgevingstoegang loopt via sys.c (ctx = g7k_sys*).
 * De CPU raakt NOOIT rechtstreeks BIOS/cart/RAM/VDC aan. */
typedef struct cpu8048_bus {
    void *ctx;
    /* programmageheugen-fetch, 12-bit adres 0x000-0xFFF.
     * sys routeert: <0x400 = interne BIOS-ROM, >=0x400 = cart (M1). */
    uint8_t (*read_rom)(void *ctx, uint16_t addr);
    /* MOVX-dataruimte, 8-bit adres; sys decodeert via P1 (RAM/VDC). */
    uint8_t (*read_ext)(void *ctx, uint8_t addr);
    void    (*write_ext)(void *ctx, uint8_t addr, uint8_t data);
    /* poort-latches: CPU meldt ELKE write (ook OUTL met zelfde waarde);
     * cart-banking (M3) en P17-helderheid (V11) hangen hieraan. */
    void    (*write_p1)(void *ctx, uint8_t val);
    uint8_t (*read_p1)(void *ctx, uint8_t latch);  /* quasi-bidirectioneel  */
    void    (*write_p2)(void *ctx, uint8_t val);
    uint8_t (*read_p2)(void *ctx, uint8_t latch);
    uint8_t (*read_bus)(void *ctx);                /* INS A,BUS             */
    void    (*write_bus)(void *ctx, uint8_t val);  /* OUTL BUS,A            */
    bool    (*read_t0)(void *ctx);                 /* JT0/JNT0; O2: ongebruikt */
    bool    (*read_t1)(void *ctx);                 /* JT1/JNT1 + counter-klok;
                                                      O2: VBL OF HBL (C7)   */
} cpu8048_bus;

typedef struct cpu8048 {
    /* --- architectuurstate (state.c serialiseert dit hele blok) --- */
    uint8_t  a;                 /* accumulator                            */
    uint16_t pc;                /* 11-bit PC + A11 uit DBF bij fetch (C2/C9) */
    uint8_t  psw;               /* CY AC F0 BS 1 SP2 SP1 SP0              */
    uint8_t  iram[64];          /* interne RAM: regbanken/stack/data (M5) */
    uint8_t  p1_latch;          /* 0xFF na reset (C8, MCS-48-datasheet)   */
    uint8_t  p2_latch;
    uint8_t  bus_latch;
    bool     f1;
    bool     dbf;               /* memory bank flip-flop MB0/MB1 (C9)     */
    bool     a11_irq_hold;      /* DBF onderdrukt tijdens interrupt (C5)  */
    uint8_t  timer;             /* 8-bit timer/counter                    */
    uint8_t  prescaler;         /* /32-prescaler in timer-mode            */
    bool     timer_running;
    bool     counter_mode;      /* STRT CNT: klokt op T1 1->0-flank       */
    bool     last_t1;           /* flankdetectie counter-mode             */
    bool     tf;                /* timer-overflowvlag                     */
    bool     timer_irq_pending;
    bool     irq_enabled;       /* EN I / DIS I                           */
    bool     tcnti_enabled;     /* EN TCNTI / DIS TCNTI                   */
    bool     irq_line;          /* externe INT-lijn (true = asserted)     */
    bool     in_irq;            /* tussen vector en RETR                  */
    uint64_t cycles;            /* machinecycli sinds koude reset         */
    /* --- wiring (niet serialiseren) --- */
    cpu8048_bus bus;
} cpu8048;

/* Koppelt callbacks en zet de hele CPU-state op nul (koude start). */
void cpu8048_init(cpu8048 *cpu, const cpu8048_bus *bus);

/* RESET-pin-semantiek (MCS-48): PC=0, PSW/stack leeg, DBF=0, timer stopt,
 * interrupts uit, P1=P2=BUS=0xFF. Roept bus.write_p1(0xFF)/write_p2(0xFF)
 * aan zodat cart-bank (bank 3! C8/M3) en VDC-selects consistent zijn.
 * Wist iram NIET (dat is g7k_reset(cold)-beleid in sys.c, S1). */
void cpu8048_reset(cpu8048 *cpu);

/* Voert precies EEN instructie uit (incl. timer-tick per verbruikte cyclus
 * en interrupt-dispatch VOOR de fetch: ext-IRQ vector 0x003 (C5),
 * timer-IRQ vector 0x007). Return: verbruikte machinecycli (1 of 2). */
int cpu8048_step(cpu8048 *cpu);

/* Externe INT-lijn (actief-laag op echt ijzer; hier: asserted=true betekent
 * "lijn laag"). Level-triggered: sys.c zet dit elke step op de actuele
 * VDC-IRQ-status. */
void cpu8048_set_irq(cpu8048 *cpu, bool asserted);

/* ============================================================================
 * VDC — Intel 8244 (NTSC) / 8245 (PAL)         implementatie: src/vdc8244.c
 * ============================================================================
 * QUIRKS-dekking: V1..V12, A1..A3. Scanline-lockstep (V1): sys.c roept per
 * lijn eerst begin_line (beampositie -> status/T1 kloppen tijdens de
 * CPU-burst van die lijn), daarna render_line (pixels + collision + audio).
 * Mid-line registerwrites landen zo vóór het renderen van diezelfde lijn.
 * Registerlayout (0x00-0xFF, richtlijn — bouwer verifieert tegen de
 * 8244-registerkaart): sprite-control, char-control, quad-chars,
 * sprite-shapes, control/status/collision/kleur, audio 0xA7-0xAA, grid.
 */
typedef struct vdc8244 {
    /* --- state (state.c serialiseert t/m audio_lfsr) --- */
    uint8_t  regs[0x100];       /* ruwe registerfile; afgeleide state hieronder */
    uint8_t  status;            /* statusreg: VBL/HBL/positie-latch bits     */
    uint8_t  collision;         /* gelatchte collision-bits (V3)             */
    bool     irq;               /* INT-uitgang; clear door statusread        */
    bool     bright;            /* P17 laag = fel palet (V11)                */
    int      line;              /* huidige scanline 0..vtotal-1              */
    g7k_region region;
    uint32_t audio_shift;       /* 24-bit shiftregister (A1)                 */
    uint16_t audio_lfsr;        /* ruisgenerator (A2)                        */
    /* --- transient (niet serialiseren) --- */
    int      line_phase;        /* CPU-cycli sinds lijnstart; door sys.c
                                   gezet vóór registertoegang (HBL-bit
                                   intra-lijn, [B] 4.7 bit 0). Transient:
                                   op framegrens altijd 0, dus veilig
                                   buiten de savestate. */
    /* --- audio-ringbuffer (niet serialiseren) --- */
    int16_t  samples[G7K_AUDIO_RING_SIZE];
    int      sample_count;
    /* --- render-doel (niet serialiseren; wijst in g7k_sys) --- */
    uint32_t *fb;
    int      fb_w, fb_h;
} vdc8244;

void vdc8244_init(vdc8244 *vdc, g7k_region region, uint32_t *fb,
                  int fb_w, int fb_h);
/* cold=true: registers/collision/audio gewist. cold=false: NIETS wissen —
 * de reset-knop hangt alleen aan de 8048-RESET-pin, de VDC heeft geen
 * reset-lijn (S1; bron [1] in kloktopologie-blok). */
void vdc8244_reset(vdc8244 *vdc, bool cold);
void vdc8244_set_region(vdc8244 *vdc, g7k_region region);

/* Registertoegang via MOVX (sys decodeert P1-selects).
 * write: V2 — writes op object-registers BLOKKEREN zolang foreground
 * enabled is (echt-ijzer-gedrag; geen opt-out).
 * read: V12 — statusread cleart VBL-bit + IRQ; collisionread latcht. */
uint8_t vdc8244_read(vdc8244 *vdc, uint8_t reg);
void    vdc8244_write(vdc8244 *vdc, uint8_t reg, uint8_t data);
/* Zonder side-effects, alleen voor g7k_vdc_peek/tests. */
uint8_t vdc8244_peek(const vdc8244 *vdc, uint8_t reg);

/* Scanline-lockstep-paar; zie blokcommentaar. render_line rendert de bij
 * begin_line gezette lijn (indien actief), werkt collision bij en pusht
 * G7K_AUDIO_SAMPLES_PER_LINE samples in de ringbuffer. */
void vdc8244_begin_line(vdc8244 *vdc, int line);
void vdc8244_render_line(vdc8244 *vdc);

/* Beam-fase binnen de huidige lijn (CPU-machinecycli sinds lijnstart);
 * sys.c roept dit vóór elke VDC-registertoegang aan zodat het
 * HBL-statusbit ($A1 bit 0) intra-lijn klopt. begin_line reset naar 0.
 * (Toevoeging 2026-07-26 n.a.v. review-finding; additief, wijzigt geen
 * bestaande signaturen.) */
void vdc8244_set_line_phase(vdc8244 *vdc, int cycles_into_line);

bool vdc8244_vblank(const vdc8244 *vdc);           /* lijn >= 242 (S2/C7)  */
/* HBLANK-status op 'cycles_into_line' CPU-machinecycli na lijnstart;
 * VDC rekent om naar pixelklokken (G7K_HBLANK_START_PX). Voor T1 (C7). */
bool vdc8244_hblank_at(const vdc8244 *vdc, int cycles_into_line);
bool vdc8244_irq_asserted(const vdc8244 *vdc);     /* naar 8048 INT (C5)   */
void vdc8244_set_bright(vdc8244 *vdc, bool bright);/* P17 laag = fel (V11) */

/* Leegt de ringbuffer naar out (max max_samples); return geschreven aantal. */
int vdc8244_audio_read(vdc8244 *vdc, int16_t *out, int max_samples);

/* ============================================================================
 * CART — cartridge + banking                   implementatie: src/cart.c
 * ============================================================================
 * QUIRKS-dekking: M1..M4, M6, M7 (+C8 samen met CPU-reset).
 * Geen mapper: P1-bits 0-1 zijn ROM-adreslijnen; bank live bij ELKE
 * P1-write (M3). Mappingformule (M4):
 *   chip = ((addr>=0xC00 ? addr-0x800 : addr-0x400) + bank*0x800)
 *          & (bit_floor(size)-1)
 * 0xC00-0xFFF spiegelt 0x800-0xBFF (A10 niet aangesloten, M2);
 * out-of-range/geen cart -> 0xFF open bus (M6).
 */
typedef struct g7k_cart {
    uint8_t rom[16384];         /* max 16K; geen malloc                   */
    uint32_t size;              /* 0 = geen cart geladen                  */
    uint8_t bank;               /* P1 & 3; na reset 3 (P1=0xFF, C8)       */
} g7k_cart;

/* size in {2048,4096,8192,12288,16384}; kopieert data. 0=ok, -1=fout. */
int  g7k_cart_load(g7k_cart *cart, const uint8_t *data, size_t size);
void g7k_cart_reset(g7k_cart *cart);               /* bank=3               */
void g7k_cart_set_p1(g7k_cart *cart, uint8_t p1);  /* bank=p1&3, live (M3) */
/* addr = CPU-programma-adres 0x400-0xFFF; puur, geen side-effects. */
uint8_t g7k_cart_read(const g7k_cart *cart, uint16_t addr);

/* ============================================================================
 * STATE — savestates                           implementatie: src/state.c
 * ============================================================================
 * QUIRKS-dekking: S3. Versioned blob: header met magic, state-versie,
 * BIOS-CRC32 (state.c berekent CRC over sys->bios), machinetype en regio;
 * load weigert bij mismatch van magic/versie/BIOS-CRC/machinetype (return
 * -1, systeemstate onaangetast). Serialiseert: cpu8048 (excl. bus),
 * vdc8244 (excl. ringbuffer/fb-pointers), g7k_cart (bank+size, GEEN
 * ROM-inhoud), extram, p1/p2-schaduw, scheduler-accumulatoren, input-latches.
 */
struct g7k_sys;                                    /* defined below */
size_t g7k_state_blob_size(const struct g7k_sys *sys);
int    g7k_state_blob_save(const struct g7k_sys *sys, uint8_t *buf, size_t size);
int    g7k_state_blob_load(struct g7k_sys *sys, const uint8_t *buf, size_t size);

/* ============================================================================
 * SYS — systeemstruct + scheduler              implementatie: src/sys.c (KLAAR)
 * ============================================================================
 * Bus-routing (gedragsfeiten, bron [1]):
 *   programmafetch : <0x400 BIOS (in de 8048), >=0x400 cart (M1/M2/M4)
 *   MOVX-read      : VDC als (P1 & 0x48)==0 (P13 laag = VDC-CS, P16 laag);
 *                    ext-RAM als P14 laag EN A7 laag -> 128B (M5);
 *                    beide af -> 0xFF; overlap AND-t (open-collector-bus)
 *   MOVX-write     : VDC als P13 laag; ext-RAM als P16 laag EN P14 laag
 *                    EN A7 laag — de externe write-strobe is alleen
 *                    actief bij P16 laag (bron [1] io_write; writes met
 *                    P16 hoog verdwijnen in het niets). De VDC-write
 *                    hangt NIET achter de P16-gate (conform bron [1]).
 *   P17            : laag = fel palet (V11)
 *   P2[0:2]        : rij-select toetsenbordmatrix/joysticks (74156, actief
 *                    als P12 laag); key terug via 74148 op P2[7:4]
 *                    (P24=GS laag bij toets, P25-27 = ~kolom); joystick
 *                    sel 0/1 actief-laag op BUS (S4-test verifieert
 *                    bitvolgorde + spelertoewijzing)
 *   T1             : VBLANK OF HBLANK (C7); externe IRQ: VDC-INT (C5)
 */
struct g7k_sys {
    g7k_region region;

    cpu8048  cpu;
    vdc8244  vdc;
    g7k_cart cart;

    uint8_t  bios[1024];
    bool     bios_loaded;
    uint8_t  extram[128];

    /* schaduw van de CPU-poortlatches voor busdecodering */
    uint8_t  p1, p2;

    /* input: pending = door frontend gezet; live = gelatcht bij frame-start
     * (deterministische record/replay, QUIRKS-ontwerpprincipe 4) */
    uint8_t  joy_pending[2], joy_live[2];
    uint8_t  keys_pending[8], keys_live[8];  /* bitmap per rij, bit=kolom */

    /* scheduler: breuk-accumulator + instructie-overshoot-schuld */
    int      cyc_frac;          /* teller-restant, 0..den-1              */
    int      cyc_debt;          /* cycli die de vorige lijn te veel liep */
    uint64_t line_start_cycles; /* cpu.cycles bij start huidige lijn     */

    uint64_t frames;

    uint32_t fb[G7K_FB_W * G7K_FB_H];
};

#endif /* G7K_CORE_INTERNAL_H */
