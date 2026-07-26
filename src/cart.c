/*
 * cart.c — cartridge-mapping + P1-live-banking VideopacHorse_Core (bouwer C)
 *
 * QUIRKS-dekking: M1..M4, M6, M7 (C8 samen met cpu8048_reset via
 * g7k_cart_reset: P1=0xFF na reset -> bank 3).
 *
 * Hardware-model (gedragsfeiten, geen code overgenomen; zie docs/QUIRKS.md
 * en docs/O2EM_DEEPDIVE.md §3):
 *   - De cart hangt op CPU-programma-adressen 0x400-0xBFF (M1); de 1KB BIOS
 *     op 0x000-0x3FF zit IN de 8048 en is niet ons domein.
 *   - A10 van de cart-poort is niet aangesloten: 0xC00-0xFFF spiegelt
 *     0x800-0xBFF (M2).
 *   - Er is GEEN mapper-register: P1-bits 0-1 zijn rechtstreeks als hoge
 *     ROM-adreslijnen bedraad. bank = P1 & 3, live bij ELKE P1-write (M3).
 *   - Mappingformule (M4):
 *       chip_offset = ((addr>=0xC00 ? addr-0x800 : addr-0x400) + bank*0x800)
 *                     & (bit_floor(size) - 1)
 *   - Buiten bereik / geen cart geladen -> 0xFF, open bus (M6).
 *
 * Keuze niet-macht-van-2 (12K): het masker is de grootte AFGEROND OMLAAG
 * op een macht van twee (12288 -> masker 0x1FFF). Dit volgt het
 * gedocumenteerde MAME-gedragsfeit dat de generieke O2-cart-mapping een
 * simpel AND-masker over de ROM-grootte is; kleinere-dan-masker offsets
 * blijven daardoor per constructie binnen de eerste bit_floor(size) bytes
 * en er is nooit een read voorbij het einde van de data. Bytes boven
 * bit_floor(size) zijn via deze generieke mapping onbereikbaar (12K/16K-
 * boards met extra adreslijnen zijn een latere, aparte mappingvariant).
 */
#include <string.h>

#include "core_internal.h"

/* Grootste macht van twee <= x (x > 0). C11 heeft geen stdbit/bit_floor. */
static uint32_t cart_bit_floor(uint32_t x)
{
    uint32_t r = 1u;
    while ((r << 1) != 0u && (r << 1) <= x)
        r <<= 1;
    return r;
}

int g7k_cart_load(g7k_cart *cart, const uint8_t *data, size_t size)
{
    if (!cart || !data)
        return -1;
    if (size != 2048 && size != 4096 && size != 8192 &&
        size != 12288 && size != 16384)
        return -1;

    /* niet-gevulde staart van het vaste 16K-array = open bus (M6) */
    memset(cart->rom, 0xFF, sizeof(cart->rom));
    memcpy(cart->rom, data, size);
    cart->size = (uint32_t)size;
    /* bank NIET aanraken: banking is uitsluitend P1/reset-domein (M3/C8) */
    return 0;
}

void g7k_cart_reset(g7k_cart *cart)
{
    if (!cart)
        return;
    /* MCS-48: P1 = 0xFF na reset -> bank = 3 (C8); 8K-carts booten dus
     * uit hun hoogste 2K-blok (M7-vector: vp_01pl fetcht file[0x1800]). */
    cart->bank = 3;
}

void g7k_cart_set_p1(g7k_cart *cart, uint8_t p1)
{
    if (!cart)
        return;
    cart->bank = (uint8_t)(p1 & 0x03u);          /* live, ELKE write (M3) */
}

uint8_t g7k_cart_read(const g7k_cart *cart, uint16_t addr)
{
    if (!cart || cart->size == 0)
        return 0xFF;                              /* geen cart: open bus (M6) */

    addr &= 0x0FFF;
    if (addr < 0x400)
        return 0xFF;                              /* BIOS-gebied, niet de cart (M1) */

    /* M2: 0xC00-0xFFF -> zelfde chipadres als 0x800-0xBFF (A10 zweeft) */
    uint16_t base = (uint16_t)((addr >= 0xC00) ? (addr - 0x800)
                                                : (addr - 0x400));
    /* M4: bankoffset + macht-van-2-masker (zie kopcommentaar voor 12K) */
    uint32_t off = ((uint32_t)base + (uint32_t)cart->bank * 0x800u)
                   & (cart_bit_floor(cart->size) - 1u);
    if (off >= cart->size)
        return 0xFF;                              /* defensief; open bus (M6) */
    return cart->rom[off];
}
