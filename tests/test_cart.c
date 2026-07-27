/*
 * test_cart.c — QUIRKS M1-M7 tests voor src/cart.c (bouwer C)
 *
 * Synthetische ROMs (byte-arrays, geen echte dumps) + eigen CRC32-helper.
 * De optionele echte-ROM-test (M7-vectoren op vp_01pl.bin) leest G7K_ROMDIR
 * en SKIPT stil als die env-variabele of het bestand ontbreekt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"

void g7k_register_test(const char *name, int (*fn)(void));

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) — eigen implementatie */
static uint32_t tcrc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* deterministisch synthetisch ROM-patroon: elke byte hangt af van zijn
 * absolute file-offset, dus elk 2K-blok is gegarandeerd verschillend */
static void fill_pattern(uint8_t *rom, size_t size)
{
    for (size_t i = 0; i < size; i++)
        rom[i] = (uint8_t)((i * 31u) ^ (i >> 6) ^ 0xA5u);
}

/* onafhankelijke referentie-implementatie van de M4-formule, rechtstreeks
 * uit QUIRKS.md overgetikt (bewust NIET g7k_cart_read hergebruiken) */
static uint32_t ref_bit_floor(uint32_t x)
{
    uint32_t r = 1u;
    while ((r << 1) != 0u && (r << 1) <= x)
        r <<= 1;
    return r;
}

static uint8_t ref_read(const uint8_t *file, uint32_t size,
                        unsigned bank, uint16_t addr)
{
    uint32_t base = (addr >= 0xC00) ? (uint32_t)(addr - 0x800)
                                    : (uint32_t)(addr - 0x400);
    uint32_t off = (base + bank * 0x800u) & (ref_bit_floor(size) - 1u);
    return (off < size) ? file[off] : 0xFF;
}

static void load_cart(g7k_cart *cart, const uint8_t *rom, size_t size)
{
    memset(cart, 0, sizeof(*cart));
    (void)g7k_cart_load(cart, rom, size);
    g7k_cart_reset(cart);
}

/* ------------------------------------------------------------------ */
/* CRC-zelftest (bewaakt de helper waar alle M7-checks op leunen)      */
/* ------------------------------------------------------------------ */

static int test_crc32_selftest(void)
{
    const uint8_t v[] = { '1','2','3','4','5','6','7','8','9' };
    return tcrc32(v, sizeof(v)) == 0xCBF43926u ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* M1 — cart-venster begint op 0x400; daaronder is het BIOS-domein     */
/* ------------------------------------------------------------------ */

static int test_M1_window_start_0x400(void)
{
    uint8_t rom[4096];
    g7k_cart cart;
    fill_pattern(rom, sizeof(rom));
    load_cart(&cart, rom, sizeof(rom));
    g7k_cart_set_p1(&cart, 0xFC);               /* bank 0 */

    for (uint16_t a = 0; a < 0x400; a++)
        if (g7k_cart_read(&cart, a) != 0xFF)
            return 1;                           /* cart mag hier niet antwoorden */
    if (g7k_cart_read(&cart, 0x400) != rom[0])
        return 1;
    if (g7k_cart_read(&cart, 0xBFF) != rom[0x7FF])
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* M2 — 0xC00-0xFFF spiegelt 0x800-0xBFF (A10 niet aangesloten)        */
/* ------------------------------------------------------------------ */

static int test_M2_mirror_0xC00(void)
{
    static const size_t sizes[] = { 2048, 4096, 8192, 12288, 16384 };
    uint8_t rom[16384];
    fill_pattern(rom, sizeof(rom));

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        g7k_cart cart;
        load_cart(&cart, rom, sizes[s]);
        for (unsigned bank = 0; bank < 4; bank++) {
            g7k_cart_set_p1(&cart, (uint8_t)(0xF0u | bank));
            for (uint16_t i = 0; i < 0x400; i += 7) {
                if (g7k_cart_read(&cart, (uint16_t)(0xC00 + i)) !=
                    g7k_cart_read(&cart, (uint16_t)(0x800 + i)))
                    return 1;
            }
            /* randen expliciet */
            if (g7k_cart_read(&cart, 0xC00) != g7k_cart_read(&cart, 0x800))
                return 1;
            if (g7k_cart_read(&cart, 0xFFF) != g7k_cart_read(&cart, 0xBFF))
                return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* M3 — bank = P1 & 3, live bij ELKE P1-write (geen mapper-register)   */
/* ------------------------------------------------------------------ */

static int test_M3_bank_live_p1(void)
{
    uint8_t rom[8192];
    g7k_cart cart;
    fill_pattern(rom, sizeof(rom));
    load_cart(&cart, rom, sizeof(rom));

    /* hoge P1-bits (VDC-selects, P17-helderheid) mogen de bank nooit raken */
    static const uint8_t p1s[] = { 0x00, 0xFD, 0x4A, 0xFF, 0x80, 0x03, 0xF6 };
    for (size_t i = 0; i < sizeof(p1s); i++) {
        g7k_cart_set_p1(&cart, p1s[i]);
        unsigned bank = p1s[i] & 3u;
        if (cart.bank != bank)
            return 1;
        /* effect direct zichtbaar op de bus */
        if (g7k_cart_read(&cart, 0x400) != rom[bank * 0x800u])
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* M4 — mappingformule voor alle groottes/banken/adressen              */
/* ------------------------------------------------------------------ */

static int test_M4_mapping_formula(void)
{
    static const size_t sizes[] = { 2048, 4096, 8192, 12288, 16384 };
    uint8_t rom[16384];
    fill_pattern(rom, sizeof(rom));

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        g7k_cart cart;
        load_cart(&cart, rom, sizes[s]);
        for (unsigned bank = 0; bank < 4; bank++) {
            g7k_cart_set_p1(&cart, (uint8_t)bank);
            for (uint32_t a = 0x400; a <= 0xFFF; a++) {
                uint8_t got = g7k_cart_read(&cart, (uint16_t)a);
                uint8_t exp = ref_read(rom, (uint32_t)sizes[s],
                                       bank, (uint16_t)a);
                if (got != exp)
                    return 1;
            }
        }
    }
    return 0;
}

/* 12K is geen macht van twee: masker = bit_floor(12288)-1 = 0x1FFF —
 * alle reads blijven per constructie binnen de eerste 8K van de file */
static int test_M4_12k_floor_mask(void)
{
    uint8_t rom[12288];
    g7k_cart cart;
    fill_pattern(rom, sizeof(rom));
    load_cart(&cart, rom, sizeof(rom));

    /* bank 3 + hoogste adres = maximale offset: (0x7FF+0x1800)&0x1FFF */
    g7k_cart_set_p1(&cart, 0x03);
    if (g7k_cart_read(&cart, 0xBFF) != rom[0x1FFF])
        return 1;
    /* bank 0, laagste adres */
    g7k_cart_set_p1(&cart, 0x00);
    if (g7k_cart_read(&cart, 0x400) != rom[0])
        return 1;
    /* geen enkele (bank,addr)-combinatie mag boven 0x1FFF uitkomen */
    for (unsigned bank = 0; bank < 4; bank++) {
        for (uint32_t a = 0x400; a <= 0xFFF; a += 3) {
            uint32_t base = (a >= 0xC00) ? a - 0x800 : a - 0x400;
            uint32_t off = (base + bank * 0x800u) & 0x1FFFu;
            if (off > 0x1FFFu)
                return 1;
            g7k_cart_set_p1(&cart, (uint8_t)bank);
            if (g7k_cart_read(&cart, (uint16_t)a) != rom[off])
                return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* M6 — open bus: geen cart of buiten bereik -> 0xFF                   */
/* ------------------------------------------------------------------ */

static int test_M6_open_bus(void)
{
    g7k_cart cart;
    memset(&cart, 0, sizeof(cart));             /* size = 0: geen cart */
    g7k_cart_reset(&cart);

    for (uint32_t a = 0x400; a <= 0xFFF; a += 5)
        if (g7k_cart_read(&cart, (uint16_t)a) != 0xFF)
            return 1;

    /* ongeldige groottes worden geweigerd en laten size op 0 */
    uint8_t buf[4096];
    fill_pattern(buf, sizeof(buf));
    if (g7k_cart_load(&cart, buf, 0) != -1)      return 1;
    if (g7k_cart_load(&cart, buf, 1024) != -1)   return 1;
    if (g7k_cart_load(&cart, buf, 3000) != -1)   return 1;
    if (g7k_cart_load(&cart, NULL, 4096) != -1)  return 1;
    if (cart.size != 0)                          return 1;
    if (g7k_cart_read(&cart, 0x400) != 0xFF)     return 1;

    /* geldige load werkt daarna gewoon */
    if (g7k_cart_load(&cart, buf, 4096) != 0)    return 1;
    g7k_cart_set_p1(&cart, 0x00);
    if (g7k_cart_read(&cart, 0x400) != buf[0])   return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* M7 — exacte testvectoren (synthetisch nagebouwd)                    */
/* ------------------------------------------------------------------ */

/* 8K-cart: na reset (P1=0xFF -> bank 3, C8) is de eerste cart-fetch op
 * 0x400 file[0x1800] — precies het vp_01pl-vectorgedrag (waarde 0x44) */
static int test_M7_first_fetch_8k(void)
{
    uint8_t rom[8192];
    g7k_cart cart;
    fill_pattern(rom, sizeof(rom));
    rom[0x1800] = 0x44;                          /* de M7-vectorwaarde */
    load_cart(&cart, rom, sizeof(rom));          /* reset -> bank 3 */

    if (cart.bank != 3)
        return 1;
    if (g7k_cart_read(&cart, 0x400) != 0x44)
        return 1;
    return 0;
}

/* 2K-cart: bank 3 wrapt door het masker terug naar file[0] (vp_14/vp_24) */
static int test_M7_first_fetch_2k(void)
{
    uint8_t rom[2048];
    g7k_cart cart;
    fill_pattern(rom, sizeof(rom));
    rom[0] = 0x44;                               /* de M7-vectorwaarde */
    load_cart(&cart, rom, sizeof(rom));          /* reset -> bank 3 */

    if (g7k_cart_read(&cart, 0x400) != 0x44)
        return 1;
    /* alle vier de banken zien hetzelfde 2K-blok */
    for (unsigned bank = 0; bank < 4; bank++) {
        g7k_cart_set_p1(&cart, (uint8_t)bank);
        if (g7k_cart_read(&cart, 0x400) != 0x44)
            return 1;
    }
    return 0;
}

/* 4x2K-banking als CRC-regressie: per bank moet de gestreamde mapper-
 * uitvoer byte-exact het bijbehorende 2K-fileblok zijn (en alle vier de
 * blok-CRC's verschillend — zelfde meetopzet als de vp_01pl-bank-CRC's) */
static int test_M7_bank_crcs_synthetic(void)
{
    uint8_t rom[8192];
    uint8_t stream[0x800];
    uint32_t crc_file[4], crc_stream[4];
    g7k_cart cart;

    fill_pattern(rom, sizeof(rom));
    load_cart(&cart, rom, sizeof(rom));

    for (unsigned bank = 0; bank < 4; bank++) {
        crc_file[bank] = tcrc32(&rom[bank * 0x800u], 0x800);
        g7k_cart_set_p1(&cart, (uint8_t)(0xFCu | bank));
        for (uint32_t i = 0; i < 0x800u; i++)
            stream[i] = g7k_cart_read(&cart, (uint16_t)(0x400u + i));
        crc_stream[bank] = tcrc32(stream, sizeof(stream));
        if (crc_stream[bank] != crc_file[bank])
            return 1;
    }
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = i + 1; j < 4; j++)
            if (crc_file[i] == crc_file[j])
                return 1;                        /* patroon te zwak */
    return 0;
}

/* Optioneel: de ECHTE M7-vectoren op vp_01pl.bin (alleen met G7K_ROMDIR).
 * File-CRC EE3EE642; bank-CRC's 004DEC9C/576D1FE3/0EBC9913/13EB8DD9;
 * eerste fetch na reset @0x400 = 0x44. Ontbreekt bestand/env -> SKIP. */
static int test_M7_real_rom_vp01pl(void)
{
    const char *romdir = getenv("G7K_ROMDIR");
    if (!romdir || !romdir[0])
        return 2;

    char fname[1024];
    snprintf(fname, sizeof(fname), "%s/vp_01pl.bin", romdir);
    FILE *f = fopen(fname, "rb");
    if (!f)
        return 2;

    uint8_t rom[8192];
    size_t n = fread(rom, 1, sizeof(rom), f);
    int extra = fgetc(f);                        /* moet EOF zijn */
    fclose(f);

    /* Onderscheid VERKEERD BESTAND van KAPOTTE EMULATOR. Deze test verwacht een
     * specifieke dump (Videopac nr 1 "plus", 8 KB, CRC EE3EE642). Ligt er een
     * ander bestand met dezelfde naam — bijvoorbeeld de 2 KB-variant van
     * hetzelfde spel — dan is dat een fout van de tester, geen bug in de core.
     * Hiervóór gaf dat gewoon FAIL en zocht je in de verkeerde hoek. Nu wordt het
     * een SKIP mét uitleg op stderr. */
    if (n != sizeof(rom) || extra != EOF) {
        fprintf(stderr,
            "  [M7] overgeslagen: %s is %zu bytes, verwacht %zu "
            "(is dit de 'plus'-dump, 8 KB?)\n", fname, n, sizeof(rom));
        return 2;
    }
    uint32_t got = tcrc32(rom, sizeof(rom));
    if (got != 0xEE3EE642u) {
        fprintf(stderr,
            "  [M7] overgeslagen: %s heeft CRC %08X, verwacht EE3EE642 "
            "(ander bestand, niet per se een emulatiefout)\n", fname, got);
        return 2;
    }

    static const uint32_t bank_crcs[4] = {
        0x004DEC9Cu, 0x576D1FE3u, 0x0EBC9913u, 0x13EB8DD9u
    };
    for (unsigned bank = 0; bank < 4; bank++)
        if (tcrc32(&rom[bank * 0x800u], 0x800) != bank_crcs[bank])
            return 1;

    g7k_cart cart;
    load_cart(&cart, rom, sizeof(rom));          /* reset -> bank 3 */
    if (g7k_cart_read(&cart, 0x400) != 0x44)
        return 1;

    uint8_t stream[0x800];
    for (unsigned bank = 0; bank < 4; bank++) {
        g7k_cart_set_p1(&cart, (uint8_t)bank);
        for (uint32_t i = 0; i < 0x800u; i++)
            stream[i] = g7k_cart_read(&cart, (uint16_t)(0x400u + i));
        if (tcrc32(stream, sizeof(stream)) != bank_crcs[bank])
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

void register_cart_tests(void)
{
    g7k_register_test("cart_crc32_selftest",          test_crc32_selftest);
    g7k_register_test("test_M1_window_start_0x400",   test_M1_window_start_0x400);
    g7k_register_test("test_M2_mirror_0xC00",         test_M2_mirror_0xC00);
    g7k_register_test("test_M3_bank_live_p1",         test_M3_bank_live_p1);
    g7k_register_test("test_M4_mapping_formula",      test_M4_mapping_formula);
    g7k_register_test("test_M4_12k_floor_mask",       test_M4_12k_floor_mask);
    g7k_register_test("test_M6_open_bus",             test_M6_open_bus);
    g7k_register_test("test_M7_first_fetch_8k",       test_M7_first_fetch_8k);
    g7k_register_test("test_M7_first_fetch_2k",       test_M7_first_fetch_2k);
    g7k_register_test("test_M7_bank_crcs_synthetic",  test_M7_bank_crcs_synthetic);
    g7k_register_test("test_M7_real_rom_vp01pl",      test_M7_real_rom_vp01pl);
}
