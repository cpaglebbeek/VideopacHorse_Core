/*
 * g7k_run.c — headless mini-verificatietool voor VideopacHorse_Core.
 *
 * Draait N frames met echte BIOS (+ optionele cart) en print per frame een
 * FNV-1a-64-hash van de RGBA-framebuffer plus een collision-samenvatting.
 * Bedoeld voor snelle regressie-vergelijking tegen een referentie-runner
 * (RetroArch/libretro-o2em) zonder scherm of geluid.
 *
 * Gebruik:
 *   g7k_run --bios <o2rom.bin> [--cart <game.bin>] [--frames N]
 *           [--region pal|ntsc] [--hash]
 *
 * ROMs staan NIET in de repo (CLAUDE.md regel 3); paden komen van de
 * gebruiker via argumenten of de env-variabelen G7K_BIOS / G7K_CART.
 * Dit is tooling, geen core: I/O (stdio) is hier toegestaan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g7000.h"

#define FNV64_OFFSET 0xcbf29ce484222325ULL
#define FNV64_PRIME  0x00000100000001b3ULL

static uint64_t fnv1a64(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint64_t h = FNV64_OFFSET;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* Leest een heel bestand; buffer via malloc (tool, geen core). */
static uint8_t *read_file(const char *file, size_t *out_size)
{
    FILE *f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "g7k_run: kan '%s' niet openen\n", file);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0 || len > 1 << 20) {
        fprintf(stderr, "g7k_run: '%s' heeft onbruikbare lengte %ld\n",
                file, len);
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "g7k_run: leesfout in '%s'\n", file);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)len;
    return buf;
}

static void usage(void)
{
    fprintf(stderr,
        "gebruik: g7k_run --bios <bin> [--cart <bin>] [--frames N]\n"
        "                 [--region pal|ntsc] [--hash]\n"
        "  --bios    1KB BIOS-dump (of env G7K_BIOS)\n"
        "  --cart    cart-dump 2/4/8/12/16K (of env G7K_CART)\n"
        "  --frames  aantal frames (default 60)\n"
        "  --region  pal (default) of ntsc\n"
        "  --hash    print FNV-1a-64 framebuffer-hash per frame\n");
}

int main(int argc, char **argv)
{
    const char *bios_file = getenv("G7K_BIOS");
    const char *cart_file = getenv("G7K_CART");
    long frames = 60;
    int  want_hash = 0;
    g7k_region region = G7K_REGION_PAL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) {
            bios_file = argv[++i];
        } else if (!strcmp(argv[i], "--cart") && i + 1 < argc) {
            cart_file = argv[++i];
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = strtol(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
            const char *r = argv[++i];
            if (!strcmp(r, "pal"))       region = G7K_REGION_PAL;
            else if (!strcmp(r, "ntsc")) region = G7K_REGION_NTSC;
            else { usage(); return 1; }
        } else if (!strcmp(argv[i], "--hash")) {
            want_hash = 1;
        } else {
            usage();
            return 1;
        }
    }
    if (!bios_file || frames <= 0) {
        usage();
        return 1;
    }

    size_t bios_size = 0;
    uint8_t *bios = read_file(bios_file, &bios_size);
    if (!bios)
        return 1;

    g7k_sys *sys = g7k_create();
    if (!sys) {
        free(bios);
        return 1;
    }
    int rc = 1;
    uint8_t *cart = NULL;

    if (g7k_load_bios(sys, bios, bios_size) != 0) {
        fprintf(stderr, "g7k_run: BIOS geweigerd (verwacht exact 1024 "
                        "bytes, kreeg %zu)\n", bios_size);
        goto done;
    }
    if (cart_file) {
        size_t cart_size = 0;
        cart = read_file(cart_file, &cart_size);
        if (!cart)
            goto done;
        if (g7k_load_cart(sys, cart, cart_size) != 0) {
            fprintf(stderr, "g7k_run: cart geweigerd (grootte %zu; "
                            "geldig: 2/4/8/12/16K)\n", cart_size);
            goto done;
        }
    }
    g7k_set_region(sys, region);
    g7k_reset(sys, true);

    const uint32_t *fb = g7k_framebuffer(sys);
    const size_t fb_bytes = (size_t)g7k_fb_width(sys) *
                            (size_t)g7k_fb_height(sys) * sizeof(uint32_t);

    uint64_t last_hash = 0;
    uint8_t  coll_union = 0;
    long     coll_frames = 0;

    for (long fnum = 0; fnum < frames; fnum++) {
        g7k_run_frame(sys);
        last_hash = fnv1a64(fb, fb_bytes);
        /* side-effect-vrije peek op de collision-accumulator ($A2) */
        uint8_t coll = g7k_vdc_peek(sys, 0xA2);
        if (coll) {
            coll_union |= coll;
            coll_frames++;
        }
        if (want_hash)
            printf("frame %5ld  fb-fnv1a64 %016llx  coll %02X\n",
                   fnum, (unsigned long long)last_hash, coll);
    }

    printf("g7k_run: %s, %ld frames, %s%s\n",
           region == G7K_REGION_PAL ? "PAL" : "NTSC", frames,
           cart_file ? "cart=" : "geen cart", cart_file ? cart_file : "");
    printf("eindhash  fb-fnv1a64 %016llx\n", (unsigned long long)last_hash);
    printf("collision: %ld/%ld frames met hits, bit-unie %02X\n",
           coll_frames, frames, coll_union);
    printf("cycli: %llu, audio %d Hz\n",
           (unsigned long long)g7k_cycle_count(sys),
           g7k_audio_sample_rate(sys));
    rc = 0;

done:
    free(cart);
    free(bios);
    g7k_destroy(sys);
    return rc;
}
