/*
 * test_state.c — QUIRKS S3 tests voor src/state.c (bouwer C)
 *
 * Strategie: volledige zichtbare state muteren -> save -> anders muteren ->
 * load -> (a) save opnieuw en byte-exact vergelijken met de eerste blob,
 * (b) spot-checks via de publieke API en de interne structs.
 * Corrupte headers (magic/versie/BIOS-CRC/machinetype/regio/afgekapt)
 * moeten -1 geven ZONDER de state aan te raken (bewijs: blob vóór en na
 * de mislukte load identiek).
 *
 * Header-offsets (contract met src/state.c):
 *   0..3 magic 'G7KS' | 4..7 versie | 8..11 BIOS-CRC32 |
 *   12..15 machinetype | 16..19 regio  (alles little-endian)
 */
#include <string.h>

#include "core_internal.h"

void g7k_register_test(const char *name, int (*fn)(void));

#define BLOB_MAX 4096

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void fill_bytes(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 13u);
}

/* volledig ingerichte machine: BIOS + 8K-cart geladen */
static g7k_sys *make_sys(void)
{
    uint8_t bios[1024];
    uint8_t rom[8192];
    g7k_sys *sys = g7k_create();
    if (!sys)
        return NULL;
    fill_bytes(bios, sizeof(bios), 0x11);
    fill_bytes(rom, sizeof(rom), 0x77);
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0 ||
        g7k_load_cart(sys, rom, sizeof(rom)) != 0) {
        g7k_destroy(sys);
        return NULL;
    }
    return sys;
}

/* muteert ALLE geserialiseerde velden afhankelijk van seed, zodat
 * roundtrip-gelijkheid niet per ongeluk op default-nullen kan slagen */
static void scramble(g7k_sys *sys, uint8_t seed)
{
    cpu8048 *c = &sys->cpu;
    vdc8244 *v = &sys->vdc;

    c->a   = (uint8_t)(seed ^ 0x5A);
    c->pc  = (uint16_t)(0x0400u + seed);
    c->psw = (uint8_t)(seed | 0x08);
    fill_bytes(c->iram, sizeof(c->iram), (uint8_t)(seed + 1));
    c->p1_latch  = (uint8_t)(seed | 0x01);
    c->p2_latch  = (uint8_t)(seed ^ 0xF0);
    c->bus_latch = (uint8_t)(seed + 3);
    c->f1  = (seed & 1) != 0;
    c->dbf = (seed & 2) != 0;
    c->a11_irq_hold = (seed & 4) != 0;
    c->timer     = (uint8_t)(seed + 5);
    c->prescaler = (uint8_t)(seed & 0x1F);
    c->timer_running     = (seed & 1) == 0;
    c->counter_mode      = (seed & 8) != 0;
    c->last_t1           = (seed & 16) != 0;
    c->tf                = (seed & 2) == 0;
    c->timer_irq_pending = (seed & 32) != 0;
    c->irq_enabled       = (seed & 4) == 0;
    c->tcnti_enabled     = (seed & 64) != 0;
    c->irq_line          = (seed & 128) != 0;
    c->in_irq            = (seed & 8) == 0;
    c->cycles = 0x0102030405060708ULL + seed;

    fill_bytes(v->regs, sizeof(v->regs), (uint8_t)(seed + 2));
    v->status      = (uint8_t)(seed ^ 0x88);
    v->collision   = (uint8_t)(seed + 9);
    v->irq         = (seed & 1) != 0;
    v->bright      = (seed & 2) == 0;
    v->line        = 100 + seed;
    v->audio_shift = 0x00ABCDEFu + seed;
    v->audio_lfsr  = (uint16_t)(0x1234u + seed);

    g7k_cart_set_p1(&sys->cart, (uint8_t)(seed & 3));

    fill_bytes(sys->extram, sizeof(sys->extram), (uint8_t)(seed + 4));
    sys->p1 = c->p1_latch;
    sys->p2 = c->p2_latch;
    sys->joy_pending[0] = (uint8_t)(seed & 0x1F);
    sys->joy_pending[1] = (uint8_t)((seed >> 1) & 0x1F);
    sys->joy_live[0]    = (uint8_t)((seed >> 2) & 0x1F);
    sys->joy_live[1]    = (uint8_t)((seed >> 3) & 0x1F);
    fill_bytes(sys->keys_pending, sizeof(sys->keys_pending), (uint8_t)(seed + 6));
    fill_bytes(sys->keys_live, sizeof(sys->keys_live), (uint8_t)(seed + 7));
    sys->cyc_frac = seed % 3;
    sys->cyc_debt = (seed >> 1) & 1;
    sys->line_start_cycles = c->cycles - 7;
    sys->frames = 1000u + seed;
}

/* -1 bij falen, anders blob-lengte; blob moet in BLOB_MAX passen */
static int save_now(g7k_sys *sys, uint8_t *blob)
{
    size_t n = g7k_state_size(sys);
    if (n == 0 || n > BLOB_MAX)
        return -1;
    if (g7k_state_save(sys, blob, n) != 0)
        return -1;
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* S3 — roundtrip: save -> muteer -> load -> volledige staat terug     */
/* ------------------------------------------------------------------ */

static int test_S3_roundtrip_full_state(void)
{
    uint8_t blob1[BLOB_MAX], blob2[BLOB_MAX];
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    scramble(sys, 0x21);
    int n = save_now(sys, blob1);
    if (n < 0)
        goto out;

    scramble(sys, 0xB6);                        /* alles anders */
    if (g7k_state_load(sys, blob1, (size_t)n) != 0)
        goto out;

    /* (a) byte-exacte her-serialisatie */
    if (save_now(sys, blob2) != n || memcmp(blob1, blob2, (size_t)n) != 0)
        goto out;

    /* (b) spot-checks: interne structs + publieke API */
    if (sys->cpu.a != (0x21 ^ 0x5A))                     goto out;
    if (sys->cpu.pc != 0x0400u + 0x21)                   goto out;
    if (sys->cpu.cycles != 0x0102030405060708ULL + 0x21) goto out;
    if (g7k_cycle_count(sys) != sys->cpu.cycles)         goto out;
    if (g7k_intram_peek(sys, 5) != (uint8_t)(0x22 + 5 * 13)) goto out;
    if (g7k_extram_peek(sys, 9) != (uint8_t)(0x25 + 9 * 13)) goto out;
    if (g7k_vdc_peek(sys, 0x40) != (uint8_t)(0x23 + 0x40 * 13)) goto out;
    if (sys->cart.bank != (0x21 & 3))                    goto out;
    if (sys->frames != 1000u + 0x21)                     goto out;
    if (sys->cyc_frac != 0x21 % 3)                       goto out;

    /* wiring mag een load nooit slopen */
    if (sys->vdc.fb != sys->fb)                          goto out;
    if (sys->cpu.bus.ctx != sys)                         goto out;

    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* regio zit in de header en moet mee terugkomen */
static int test_S3_region_restored(void)
{
    uint8_t blob[BLOB_MAX];
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    g7k_set_region(sys, G7K_REGION_PAL);
    scramble(sys, 0x42);
    int n = save_now(sys, blob);
    if (n < 0)
        goto out;

    g7k_set_region(sys, G7K_REGION_NTSC);
    if (g7k_state_load(sys, blob, (size_t)n) != 0)
        goto out;
    if (g7k_get_region(sys) != G7K_REGION_PAL)
        goto out;
    if (sys->vdc.region != G7K_REGION_PAL)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* ------------------------------------------------------------------ */
/* S3 — corrupte headers: -1 en state onaangetast                      */
/* ------------------------------------------------------------------ */

/* gedeelde opzet: geldige blob maken, corrupter(blob) toepassen, laden
 * moet -1 geven en de state moet byte-exact ongewijzigd blijven */
static int corrupt_case(void (*corrupter)(uint8_t *blob, int n))
{
    uint8_t blob[BLOB_MAX], before[BLOB_MAX], after[BLOB_MAX];
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    scramble(sys, 0x33);
    int n = save_now(sys, blob);
    if (n < 0)
        goto out;
    memcpy(before, blob, (size_t)n);

    corrupter(blob, n);
    if (g7k_state_load(sys, blob, (size_t)n) != -1)
        goto out;                                /* had geweigerd moeten worden */

    /* state onaangetast? her-serialisatie == pre-corruptie-blob */
    if (save_now(sys, after) != n || memcmp(before, after, (size_t)n) != 0)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

static void corrupt_magic(uint8_t *blob, int n)   { (void)n; blob[0] ^= 0xFF; }
static void corrupt_version(uint8_t *blob, int n) { (void)n; blob[4] ^= 0x01; }
static void corrupt_bioscrc(uint8_t *blob, int n) { (void)n; blob[8] ^= 0x01; }
static void corrupt_machine(uint8_t *blob, int n) { (void)n; blob[12] ^= 0xFF; }
static void corrupt_region(uint8_t *blob, int n)  { (void)n; blob[16] = 0x7F; }

static int test_S3_bad_magic(void)    { return corrupt_case(corrupt_magic); }
static int test_S3_bad_version(void)  { return corrupt_case(corrupt_version); }
static int test_S3_bad_bios_crc(void) { return corrupt_case(corrupt_bioscrc); }
static int test_S3_bad_machine(void)  { return corrupt_case(corrupt_machine); }
static int test_S3_bad_region(void)   { return corrupt_case(corrupt_region); }

/* andere BIOS in de machine dan waarmee de state gemaakt is -> -1 */
static int test_S3_other_bios_refused(void)
{
    uint8_t blob[BLOB_MAX];
    uint8_t bios2[1024];
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    scramble(sys, 0x55);
    int n = save_now(sys, blob);
    if (n < 0)
        goto out;

    fill_bytes(bios2, sizeof(bios2), 0x99);      /* andere BIOS-inhoud */
    if (g7k_load_bios(sys, bios2, sizeof(bios2)) != 0)
        goto out;
    if (g7k_state_load(sys, blob, (size_t)n) != -1)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* andere cart-grootte geladen dan in de blob -> -1; zelfde cart -> 0 */
static int test_S3_other_cart_refused(void)
{
    uint8_t blob[BLOB_MAX];
    uint8_t rom2k[2048], rom8k[8192];
    g7k_sys *sys = make_sys();                   /* 8K-cart */
    if (!sys)
        return 1;
    int rc = 1;

    scramble(sys, 0x66);
    int n = save_now(sys, blob);
    if (n < 0)
        goto out;

    fill_bytes(rom2k, sizeof(rom2k), 0x10);
    if (g7k_load_cart(sys, rom2k, sizeof(rom2k)) != 0)
        goto out;
    if (g7k_state_load(sys, blob, (size_t)n) != -1)
        goto out;

    fill_bytes(rom8k, sizeof(rom8k), 0x77);      /* zelfde als make_sys */
    if (g7k_load_cart(sys, rom8k, sizeof(rom8k)) != 0)
        goto out;
    if (g7k_state_load(sys, blob, (size_t)n) != 0)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* afgekapte buffers en te kleine save-buffers -> -1 */
static int test_S3_truncated_and_bounds(void)
{
    uint8_t blob[BLOB_MAX], before[BLOB_MAX], after[BLOB_MAX];
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    scramble(sys, 0x44);
    int n = save_now(sys, blob);
    if (n < 0 || n <= 20)
        goto out;
    memcpy(before, blob, (size_t)n);

    if (g7k_state_save(sys, blob, (size_t)(n - 1)) != -1)
        goto out;                                /* buffer te klein voor save */
    if (g7k_state_load(sys, blob, (size_t)(n - 1)) != -1)
        goto out;                                /* afgekapt laden */
    if (g7k_state_load(sys, blob, 4) != -1)
        goto out;                                /* korter dan de header */

    if (save_now(sys, after) != n || memcmp(before, after, (size_t)n) != 0)
        goto out;                                /* niets half geladen */

    /* volle blob laadt daarna nog gewoon */
    if (g7k_state_load(sys, blob, (size_t)n) != 0)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* blob-grootte is stabiel en > header */
static int test_S3_blob_size_stable(void)
{
    g7k_sys *sys = make_sys();
    if (!sys)
        return 1;
    int rc = 1;

    size_t n1 = g7k_state_size(sys);
    scramble(sys, 0x77);
    size_t n2 = g7k_state_size(sys);
    if (n1 == 0 || n1 != n2 || n1 <= 20 || n1 > BLOB_MAX)
        goto out;
    rc = 0;
out:
    g7k_destroy(sys);
    return rc;
}

/* ------------------------------------------------------------------ */

void register_state_tests(void)
{
    g7k_register_test("test_S3_roundtrip_full_state",  test_S3_roundtrip_full_state);
    g7k_register_test("test_S3_region_restored",       test_S3_region_restored);
    g7k_register_test("test_S3_bad_magic",             test_S3_bad_magic);
    g7k_register_test("test_S3_bad_version",           test_S3_bad_version);
    g7k_register_test("test_S3_bad_bios_crc",          test_S3_bad_bios_crc);
    g7k_register_test("test_S3_bad_machine",           test_S3_bad_machine);
    g7k_register_test("test_S3_bad_region",            test_S3_bad_region);
    g7k_register_test("test_S3_other_bios_refused",    test_S3_other_bios_refused);
    g7k_register_test("test_S3_other_cart_refused",    test_S3_other_cart_refused);
    g7k_register_test("test_S3_truncated_and_bounds",  test_S3_truncated_and_bounds);
    g7k_register_test("test_S3_blob_size_stable",      test_S3_blob_size_stable);
}
