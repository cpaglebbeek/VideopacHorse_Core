/*
 * test_sys.c — systeemniveau-tests (integrator): M5, S1, S2.
 *
 * Via de publieke API (include/g7000.h) met synthetische 8048-programma's
 * als 1KB test-BIOS (BIOS-loos zelfvoorzienend, geen echte ROMs).
 * Registratie: register_sys_tests() wordt aangeroepen vanuit test_main.c.
 */
#include <string.h>

#include "g7000.h"

void g7k_register_test(const char *name, int (*fn)(void));

/* Bouwt een 1KB BIOS-image met het programma vanaf 0x000 en een
 * eindeloze lus erachter zodat de CPU nooit buiten het programma leest. */
static void make_bios(uint8_t *bios, const uint8_t *prog, size_t n)
{
    memset(bios, 0x00, 1024); /* 0x00 = NOP */
    memcpy(bios, prog, n);
    /* JMP naar zichzelf op het einde van het programma (2-byte JMP page0) */
    bios[n]     = 0x04;
    bios[n + 1] = (uint8_t)n;
}

/* M5: 128B externe RAM, NIET 256 — A7 hoog selecteert géén ext-RAM,
 * dus een write op 0x92 mag extram[0x12] niet raken (geen 128B-alias)
 * en niet als extra RAM-bank landen. Plus P16-gate: de externe
 * write-strobe is alleen actief bij P16 laag (bron [1] io_write in
 * core_internal.h) — een MOVX-write met P16 hoog landt nergens. */
static int test_M5_extram_128_not_256(void)
{
    static const uint8_t prog[] = {
        0x23, 0xAF,       /* MOV A,#0xAF : P16+P14 laag (write-strobe + */
        0x39,             /* OUTL P1,A     RAM-select), VDC af, P17 hoog*/
        0xB8, 0x12,       /* MOV R0,#0x12                               */
        0x23, 0x5A,       /* MOV A,#0x5A                                */
        0x90,             /* MOVX @R0,A  -> extram[0x12] = 0x5A         */
        0xB8, 0x92,       /* MOV R0,#0x92  (A7 hoog!)                   */
        0x23, 0xA5,       /* MOV A,#0xA5                                */
        0x90,             /* MOVX @R0,A  -> mag NERGENS landen (M5)     */
        0x23, 0xEF,       /* MOV A,#0xEF : P14 laag maar P16 HOOG       */
        0x39,             /* OUTL P1,A                                  */
        0xB8, 0x12,       /* MOV R0,#0x12                               */
        0x23, 0x77,       /* MOV A,#0x77                                */
        0x90,             /* MOVX @R0,A  -> spook-write, moet genegeerd */
    };
    uint8_t bios[1024];
    make_bios(bios, prog, sizeof(prog));

    g7k_sys *sys = g7k_create();
    if (!sys)
        return 1;
    int rc = 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        goto done;
    g7k_reset(sys, true);
    g7k_run_frame(sys);

    if (g7k_extram_peek(sys, 0x12) != 0x5A)  /* write met A7 laag werkt  */
        goto done;
    /* zou 0xA5 zijn als A7 genegeerd werd (256B-gedrag) — moet 0x5A blijven */
    if (g7k_extram_peek(sys, 0x92) != 0x5A)  /* peek maskeert zelf &0x7F */
        goto done;
    /* zou 0x77 zijn zonder P16-gate — echt ijzer negeert deze write */
    if (g7k_extram_peek(sys, 0x12) != 0x5A)
        goto done;
    rc = 0;
done:
    g7k_destroy(sys);
    return rc;
}

/* S1: warme reset (reset-toets) laat extram ÉN VDC-state staan; koude
 * reset (power-cycle) wist beide. De VDC hangt niet aan de reset-knop
 * (geen reset-lijn), dus warm mag geen enkel VDC-register wissen. */
static int test_S1_warm_keeps_extram_cold_wipes(void)
{
    static const uint8_t prog[] = {
        0x23, 0xAF,       /* MOV A,#0xAF : P16+P14 laag, VDC af         */
        0x39,             /* OUTL P1,A   */
        0xB8, 0x40,       /* MOV R0,#0x40 */
        0x23, 0xC3,       /* MOV A,#0xC3 */
        0x90,             /* MOVX @R0,A  -> extram[0x40] = 0xC3         */
        0x23, 0xB7,       /* MOV A,#0xB7 : P13 laag (VDC), P14 hoog     */
        0x39,             /* OUTL P1,A   */
        0xB8, 0x01,       /* MOV R0,#0x01 */
        0x23, 0x5C,       /* MOV A,#0x5C */
        0x90,             /* MOVX @R0,A  -> VDC sprite-0-X = 0x5C       */
    };
    uint8_t bios[1024];
    make_bios(bios, prog, sizeof(prog));

    g7k_sys *sys = g7k_create();
    if (!sys)
        return 1;
    int rc = 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        goto done;
    g7k_reset(sys, true);
    g7k_run_frame(sys);
    if (g7k_extram_peek(sys, 0x40) != 0xC3)
        goto done;
    if (g7k_vdc_peek(sys, 0x01) != 0x5C)      /* VDC-write is geland    */
        goto done;

    g7k_reset(sys, false);                    /* warm: RAM blijft (S1)  */
    if (g7k_extram_peek(sys, 0x40) != 0xC3)
        goto done;
    if (g7k_vdc_peek(sys, 0x01) != 0x5C)      /* warm laat VDC staan    */
        goto done;
    if (g7k_cycle_count(sys) == 0)            /* warm wist cycli NIET   */
        goto done;

    g7k_reset(sys, true);                     /* koud: alles gewist     */
    if (g7k_extram_peek(sys, 0x40) != 0x00)
        goto done;
    if (g7k_vdc_peek(sys, 0x01) != 0x00)      /* koud wist ook de VDC   */
        goto done;
    if (g7k_cycle_count(sys) != 0)
        goto done;
    rc = 0;
done:
    g7k_destroy(sys);
    return rc;
}

/* S2: regio is een first-class runtime-parameter — framebudget en
 * audio-samplerate volgen de regio zonder re-create.
 * PAL: 313 lijnen x 76/3 cycli = 23788/3 -> 3 frames = exact 23788 cycli.
 * NTSC: 263 x 91/4 = 23933/4 -> 4 frames = exact 23933 cycli.
 * Overshoot-schuld van een 2-cycle-instructie op de framegrens geeft
 * maximaal +1 cyclus verschil (schuld wordt het volgende frame verrekend). */
static int test_S2_region_runtime_switch(void)
{
    uint8_t bios[1024];
    static const uint8_t prog[] = { 0x00 };   /* NOP; make_bios plakt lus */
    make_bios(bios, prog, sizeof(prog));

    g7k_sys *sys = g7k_create();
    if (!sys)
        return 1;
    int rc = 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        goto done;

    if (g7k_get_region(sys) != G7K_REGION_PAL)  /* G7000 = PAL default  */
        goto done;
    if (g7k_audio_sample_rate(sys) != 31113)
        goto done;

    g7k_reset(sys, true);
    for (int i = 0; i < 3; i++)
        g7k_run_frame(sys);
    uint64_t pal3 = g7k_cycle_count(sys);
    if (pal3 < 23788 || pal3 > 23789)
        goto done;

    g7k_set_region(sys, G7K_REGION_NTSC);       /* runtime switch (S2)  */
    if (g7k_get_region(sys) != G7K_REGION_NTSC)
        goto done;
    if (g7k_audio_sample_rate(sys) != 31469)
        goto done;
    for (int i = 0; i < 4; i++)
        g7k_run_frame(sys);
    uint64_t delta = g7k_cycle_count(sys) - pal3;
    if (delta < 23933 - 1 || delta > 23933 + 1)
        goto done;
    rc = 0;
done:
    g7k_destroy(sys);
    return rc;
}

/* C7-bedrading: T1 = VBLANK OF HBLANK, gemeten door de HELE keten
 * (JT1-opcode -> bus-callback -> sys_read_t1 -> vdc8244_vblank/
 * vdc8244_hblank_at) via de publieke API. Een synthetisch BIOS-programma
 * sampelt T1 op vijf cyclus-exacte momenten in een NTSC-frame en schrijft
 * 0/1 naar interne RAM 0x20..0x24:
 *
 *   S1 @cyc 2    lijn 0,   in-lijn 2    -> 0 (actieve scan, geen HBL)
 *   S2 @cyc 20   lijn 0,   in-lijn 20   -> 1 (HBL: 20*20px=400 >= 366)
 *   S3 @cyc 30   lijn 1,   in-lijn ~8   -> 0
 *   S4 @cyc 5490 lijn 241, in-lijn ~8   -> 0 (vlak VOOR VBLANK)
 *   S5 @cyc 5508 lijn 242, in-lijn ~3   -> 1 (VBLANK-start; in-lijn < 19
 *                                             dus alleen VBL verklaart 1)
 *
 * Elk sample-blok is constant 8 cycli ongeacht de T1-waarde, zodat de
 * cyclusposities deterministisch blijven. NTSC-lijnbudget = 91/4 cycli;
 * lijnstart L = floor(91L/4) +/- 1 cyclus overshoot-schuld — alle
 * samplepunten liggen ruim binnen hun geldigheidsvenster. Een omgekeerde
 * T1-polariteit, een verkeerde VBLANK-startlijn of een kapotte
 * px-per-cyclus-omrekening laat minstens een sample omklappen. */
static size_t emit_t1_sample(uint8_t *p, size_t o)
{
    uint8_t hi   = (uint8_t)(o + 6);
    uint8_t join = (uint8_t)(o + 10);
    p[o++] = 0x56; p[o++] = hi;        /* JT1 hi        (2 cycli, leest T1) */
    p[o++] = 0x23; p[o++] = 0x00;      /* MOV A,#0      (2)                 */
    p[o++] = 0x04; p[o++] = join;      /* JMP join      (2)                 */
    p[o++] = 0x23; p[o++] = 0x01;      /* hi: MOV A,#1  (2)                 */
    p[o++] = 0x04; p[o++] = join;      /* JMP join      (2)                 */
    p[o++] = 0xA0;                     /* join: MOV @R0,A (1)               */
    p[o++] = 0x18;                     /* INC R0        (1)                 */
    return o;                          /* beide paden: 8 cycli totaal       */
}

static int test_C7_t1_wiring_vbl_hbl(void)
{
    uint8_t prog[128];
    uint8_t bios[1024];
    size_t o = 0, outer, inner, d2;
    int i;

    prog[o++] = 0xB8; prog[o++] = 0x20;     /* MOV R0,#0x20   cyc 0-1    */
    o = emit_t1_sample(prog, o);            /* S1: leest T1 @cyc 2       */
    for (i = 0; i < 10; i++)
        prog[o++] = 0x00;                   /* NOP x10        cyc 10-19  */
    o = emit_t1_sample(prog, o);            /* S2: leest T1 @cyc 20      */
    prog[o++] = 0x00; prog[o++] = 0x00;     /* NOP x2         cyc 28-29  */
    o = emit_t1_sample(prog, o);            /* S3: leest T1 @cyc 30      */
    /* delay 2 + 25*(2+2*107+2) = 5452 cycli -> cyc 38..5489 */
    prog[o++] = 0xBA; prog[o++] = 25;       /* MOV R2,#25                */
    outer = o;
    prog[o++] = 0xBB; prog[o++] = 107;      /* MOV R3,#107               */
    inner = o;
    prog[o++] = 0xEB; prog[o++] = (uint8_t)inner;  /* DJNZ R3,inner      */
    prog[o++] = 0xEA; prog[o++] = (uint8_t)outer;  /* DJNZ R2,outer      */
    o = emit_t1_sample(prog, o);            /* S4: leest T1 @cyc 5490    */
    /* delay 2 + 2*4 = 10 cycli -> cyc 5498..5507 */
    prog[o++] = 0xBB; prog[o++] = 4;        /* MOV R3,#4                 */
    d2 = o;
    prog[o++] = 0xEB; prog[o++] = (uint8_t)d2;     /* DJNZ R3,d2         */
    o = emit_t1_sample(prog, o);            /* S5: leest T1 @cyc 5508    */
    make_bios(bios, prog, o);

    g7k_sys *sys = g7k_create();
    if (!sys)
        return 1;
    int rc = 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        goto done;
    g7k_set_region(sys, G7K_REGION_NTSC);   /* 20 px per cyclus (C7)     */
    g7k_reset(sys, true);
    g7k_run_frame(sys);

    if (g7k_intram_peek(sys, 0x20) != 0)    /* S1: actief, geen HBL      */
        goto done;
    if (g7k_intram_peek(sys, 0x21) != 1)    /* S2: HBLANK                */
        goto done;
    if (g7k_intram_peek(sys, 0x22) != 0)    /* S3: actief, geen HBL      */
        goto done;
    if (g7k_intram_peek(sys, 0x23) != 0)    /* S4: lijn 241, nog geen VBL*/
        goto done;
    if (g7k_intram_peek(sys, 0x24) != 1)    /* S5: VBLANK vanaf lijn 242 */
        goto done;
    rc = 0;
done:
    g7k_destroy(sys);
    return rc;
}

void register_sys_tests(void);

void register_sys_tests(void)
{
    g7k_register_test("test_M5_extram_128_not_256",
                      test_M5_extram_128_not_256);
    g7k_register_test("test_S1_warm_keeps_extram_cold_wipes",
                      test_S1_warm_keeps_extram_cold_wipes);
    g7k_register_test("test_S2_region_runtime_switch",
                      test_S2_region_runtime_switch);
    g7k_register_test("test_C7_t1_wiring_vbl_hbl",
                      test_C7_t1_wiring_vbl_hbl);
}
