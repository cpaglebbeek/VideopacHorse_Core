/*
 * g7k_netplay_check — bewijst de drie aannames waar netplay (VideopacHorse_Web
 * /videopac/2/) volledig op steunt. Draait twee machines naast elkaar via
 * PRECIES de API die de web-frontend ook gebruikt (g7k_joystick_set,
 * g7k_key_set, g7k_run_frame, g7k_state_save/_load) — een groene uitslag hier
 * betekent dus iets voor de echte route, niet voor een testopstelling ernaast.
 *
 *   Fase 1  determinisme  : zelfde BIOS + cart + regio + invoer per frame
 *                           ⇒ bit-identieke machinestaat, frame na frame.
 *   Fase 2  detectie      : loopt er één frame verschil in, dan MOET de
 *                           state-hash dat zien. Een detector die nooit
 *                           aanslaat bewijst niets over fase 1.
 *   Fase 3  herstel       : een savestate van A in B laden zet B exact terug op
 *                           A; daarna lopen ze weer gelijk. Dat is de resync.
 *
 * gebruik: g7k_netplay_check --bios <o2rom.bin> [--cart <game.bin>]
 *                            [--frames N] [--seed S] [--region pal|ntsc]
 * exitcode 0 = alle drie de fasen zoals verwacht.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "g7000.h"

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Hash van de VOLLEDIGE machinestaat — niet van het beeld. Twee machines kunnen
 * hetzelfde tonen en toch verschillende RAM hebben; dat verschil komt frames
 * later alsnog naar buiten, en dan is het al te laat. */
static uint64_t state_hash(const g7k_sys *sys, uint8_t **buf, size_t *cap)
{
    size_t n = g7k_state_size(sys);
    if (n == 0) return 0;
    if (*cap < n) {
        free(*buf);
        *buf = malloc(n);
        *cap = n;
    }
    if (!*buf || g7k_state_save(sys, *buf, n) != 0) return 0;
    return fnv64(*buf, n);
}

static uint64_t fb_hash(const g7k_sys *sys)
{
    const uint32_t *fb = g7k_framebuffer(sys);
    size_t n = (size_t)g7k_fb_width(sys) * (size_t)g7k_fb_height(sys) * 4u;
    return fnv64((const uint8_t *)fb, n);
}

/* Reproduceerbare invoerreeks: dezelfde seed geeft dezelfde knoppen op dezelfde
 * frames, zodat beide machines gegarandeerd hetzelfde te verwerken krijgen. */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 16;
}

struct step_input {
    uint8_t p1, p2;
    uint8_t key;      /* G7K_KEY_NONE = geen toetsactie dit frame */
    uint8_t down;
};

static struct step_input input_for(long frame, uint32_t seed)
{
    uint32_t s = seed ^ (uint32_t)(frame * 2654435761u);
    struct step_input in;
    in.p1 = (uint8_t)(lcg(&s) & 0x1f);
    in.p2 = (uint8_t)(lcg(&s) & 0x1f);
    in.key = G7K_KEY_NONE;
    in.down = 0;
    /* Ook toetsen meenemen: op de console kies je het spel met het membraan-
     * toetsenbord, dus die weg moet net zo deterministisch zijn als de joystick. */
    if (frame % 17 == 0) { in.key = (uint8_t)(lcg(&s) % 48u); in.down = 1; }
    else if (frame % 17 == 3) { in.key = (uint8_t)(lcg(&s) % 48u); in.down = 0; }
    return in;
}

static void apply(g7k_sys *sys, struct step_input in)
{
    g7k_joystick_set(sys, 0, in.p1);
    g7k_joystick_set(sys, 1, in.p2);
    if (in.key != G7K_KEY_NONE) g7k_key_set(sys, in.key, in.down != 0);
}

static void usage(void)
{
    fprintf(stderr,
        "gebruik: g7k_netplay_check --bios <bin> [--cart <bin>]\n"
        "                          [--frames N] [--seed S] [--region pal|ntsc]\n");
}

int main(int argc, char **argv)
{
    const char *bios_file = getenv("G7K_BIOS");
    const char *cart_file = getenv("G7K_CART");
    long frames = 1800;                 /* ~36 s PAL */
    uint32_t seed = 20260727u;
    g7k_region region = G7K_REGION_PAL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios_file = argv[++i];
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc) cart_file = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
            region = strcmp(argv[++i], "ntsc") == 0 ? G7K_REGION_NTSC : G7K_REGION_PAL;
        } else { usage(); return 2; }
    }
    if (!bios_file) { usage(); return 2; }

    size_t bios_len = 0, cart_len = 0;
    uint8_t *bios = read_file(bios_file, &bios_len);
    if (!bios) { fprintf(stderr, "kan BIOS niet lezen: %s\n", bios_file); return 2; }
    uint8_t *cart = NULL;
    if (cart_file) {
        cart = read_file(cart_file, &cart_len);
        if (!cart) { fprintf(stderr, "kan cart niet lezen: %s\n", cart_file); return 2; }
    }

    g7k_sys *A = g7k_create(), *B = g7k_create();
    if (!A || !B) { fprintf(stderr, "g7k_create faalde\n"); return 2; }
    for (int i = 0; i < 2; i++) {
        g7k_sys *s = i ? B : A;
        g7k_set_region(s, region);
        if (g7k_load_bios(s, bios, bios_len) != 0) { fprintf(stderr, "BIOS afgewezen\n"); return 2; }
        if (cart && g7k_load_cart(s, cart, cart_len) != 0) { fprintf(stderr, "cart afgewezen\n"); return 2; }
        g7k_reset(s, true);
    }

    uint8_t *bufA = NULL, *bufB = NULL;
    size_t capA = 0, capB = 0;
    long phase2_at = frames / 3, phase3_at = (frames * 2) / 3;
    int diverged_seen = 0, mismatch_in_phase1 = 0, mismatch_after_resync = 0;
    long first_diff_frame = -1, resync_frame = -1;

    printf("netplay-check: frames=%ld seed=%u regio=%s cart=%s\n",
           frames, seed, region == G7K_REGION_NTSC ? "ntsc" : "pal",
           cart_file ? cart_file : "(geen)");

    for (long f = 0; f < frames; f++) {
        struct step_input in = input_for(f, seed);
        apply(A, in);
        apply(B, in);
        g7k_run_frame(A);
        g7k_run_frame(B);

        if (f == phase2_at) {
            /* Fase 2: B loopt bewust één frame voor. Dat is een hardere
             * verstoring dan een afwijkende knopdruk, die in een menu of demo
             * genegeerd kan worden en dan ten onrechte "geen verschil" oplevert. */
            g7k_run_frame(B);
            printf("  fase 2: op frame %ld een extra frame in B gestopt\n", f);
        }

        if (f == phase3_at) {
            size_t n = g7k_state_size(A);
            uint8_t *snap = malloc(n);
            if (!snap || g7k_state_save(A, snap, n) != 0) {
                fprintf(stderr, "savestate maken faalde\n");
                return 2;
            }
            if (g7k_state_load(B, snap, n) != 0) {
                fprintf(stderr, "savestate laden faalde\n");
                return 2;
            }
            free(snap);
            resync_frame = f;
            printf("  fase 3: op frame %ld savestate van A in B geladen\n", f);
        }

        if ((f % 60) == 0 || f == frames - 1) {
            uint64_t ha = state_hash(A, &bufA, &capA);
            uint64_t hb = state_hash(B, &bufB, &capB);
            uint64_t fa = fb_hash(A), fb = fb_hash(B);
            int same = (ha == hb) && (fa == fb);
            if (f < phase2_at) {
                if (!same) { mismatch_in_phase1 = 1; if (first_diff_frame < 0) first_diff_frame = f; }
            } else if (f > phase2_at && f < phase3_at) {
                if (!same) diverged_seen = 1;
            } else if (f > phase3_at) {
                if (!same) { mismatch_after_resync = 1; if (first_diff_frame < 0) first_diff_frame = f; }
            }
        }
    }

    /* Slotvergelijking op het laatste frame: het strengst mogelijke oordeel. */
    uint64_t ha = state_hash(A, &bufA, &capA);
    uint64_t hb = state_hash(B, &bufB, &capB);

    printf("\nfase 1 (determinisme t/m frame %ld): %s\n",
           phase2_at, mismatch_in_phase1 ? "GEFAALD — machines liepen uiteen" : "ok");
    printf("fase 2 (detectie na verstoring):      %s\n",
           diverged_seen ? "ok — verschil gezien" : "GEFAALD — verschil niet opgemerkt");
    printf("fase 3 (herstel via savestate):       %s\n",
           mismatch_after_resync ? "GEFAALD — nog steeds verschil" : "ok");
    printf("state-hash A=%016llx B=%016llx (savestate %zu bytes)\n",
           (unsigned long long)ha, (unsigned long long)hb, g7k_state_size(A));
    if (first_diff_frame >= 0) printf("eerste onverwachte afwijking op frame %ld\n", first_diff_frame);
    if (resync_frame >= 0) printf("resync uitgevoerd op frame %ld\n", resync_frame);

    free(bufA); free(bufB); free(bios); free(cart);
    g7k_destroy(A); g7k_destroy(B);

    int ok = !mismatch_in_phase1 && diverged_seen && !mismatch_after_resync && (ha == hb);
    printf("\n%s\n", ok ? "NETPLAY-CHECK GROEN" : "NETPLAY-CHECK ROOD");
    return ok ? 0 : 1;
}
