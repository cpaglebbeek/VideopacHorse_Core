/*
 * sys.c — skeleton-implementatie (v0.0.1-Baer).
 * Levenscyclus + API-oppervlak zodat ports en testharness kunnen linken.
 * De echte emulatie (cpu8048.c, vdc8244.c, cart.c, state.c) vervangt deze
 * stubs in de bouwfase; zie ARCHITECTURE.md en docs/QUIRKS.md.
 */
#include <stdlib.h>
#include <string.h>
#include "../include/g7000.h"

#define STUB_FB_W 320
#define STUB_FB_H 240
#define STUB_AUDIO_RATE 44100

struct g7k_sys {
    g7k_region region;
    uint32_t fb[STUB_FB_W * STUB_FB_H];
    uint8_t bios[1024];
    bool bios_loaded;
    uint8_t *cart;
    size_t cart_size;
    uint64_t cycles;
    uint8_t joy[2];
};

g7k_sys *g7k_create(void)
{
    g7k_sys *sys = calloc(1, sizeof(*sys));
    if (sys) sys->region = G7K_REGION_PAL;
    return sys;
}

void g7k_destroy(g7k_sys *sys)
{
    if (!sys) return;
    free(sys->cart);
    free(sys);
}

int g7k_load_bios(g7k_sys *sys, const uint8_t *data, size_t size)
{
    if (!sys || !data || size != sizeof(sys->bios)) return -1;
    memcpy(sys->bios, data, size);
    sys->bios_loaded = true;
    return 0;
}

int g7k_load_cart(g7k_sys *sys, const uint8_t *data, size_t size)
{
    if (!sys || !data || size < 2048 || size > 16384) return -1;
    uint8_t *copy = malloc(size);
    if (!copy) return -1;
    memcpy(copy, data, size);
    free(sys->cart);
    sys->cart = copy;
    sys->cart_size = size;
    return 0;
}

void g7k_reset(g7k_sys *sys, bool cold)
{
    if (!sys) return;
    if (cold) {
        sys->cycles = 0;
        memset(sys->fb, 0, sizeof(sys->fb));
    }
}

void g7k_set_region(g7k_sys *sys, g7k_region region) { if (sys) sys->region = region; }
g7k_region g7k_get_region(const g7k_sys *sys) { return sys ? sys->region : G7K_REGION_PAL; }

void g7k_run_frame(g7k_sys *sys) { if (sys) sys->cycles += 1; }

const uint32_t *g7k_framebuffer(const g7k_sys *sys) { return sys ? sys->fb : NULL; }
int g7k_fb_width(const g7k_sys *sys) { (void)sys; return STUB_FB_W; }
int g7k_fb_height(const g7k_sys *sys) { (void)sys; return STUB_FB_H; }

int g7k_audio_read(g7k_sys *sys, int16_t *out, int max_samples)
{
    (void)sys; (void)out; (void)max_samples;
    return 0;
}
int g7k_audio_sample_rate(const g7k_sys *sys) { (void)sys; return STUB_AUDIO_RATE; }

void g7k_joystick_set(g7k_sys *sys, int player, uint8_t mask)
{
    if (sys && (player == 0 || player == 1)) sys->joy[player] = mask;
}
void g7k_key_set(g7k_sys *sys, uint8_t matrix_code, bool down)
{
    (void)sys; (void)matrix_code; (void)down;
}
uint8_t g7k_key_from_char(char c) { (void)c; return G7K_KEY_NONE; }

size_t g7k_state_size(const g7k_sys *sys) { (void)sys; return 0; }
int g7k_state_save(const g7k_sys *sys, uint8_t *buf, size_t size)
{
    (void)sys; (void)buf; (void)size;
    return -1;
}
int g7k_state_load(g7k_sys *sys, const uint8_t *buf, size_t size)
{
    (void)sys; (void)buf; (void)size;
    return -1;
}

uint64_t g7k_cycle_count(const g7k_sys *sys) { return sys ? sys->cycles : 0; }
uint8_t g7k_bus_peek(const g7k_sys *sys, uint16_t addr)
{
    if (!sys) return 0xFF;
    if (addr < 0x400) return sys->bios_loaded ? sys->bios[addr] : 0xFF;
    return 0xFF;
}
uint8_t g7k_intram_peek(const g7k_sys *sys, uint8_t addr) { (void)sys; (void)addr; return 0xFF; }
uint8_t g7k_extram_peek(const g7k_sys *sys, uint8_t addr) { (void)sys; (void)addr; return 0xFF; }
uint8_t g7k_vdc_peek(const g7k_sys *sys, uint8_t reg) { (void)sys; (void)reg; return 0xFF; }

const char *g7k_version(void) { return G7K_VERSION_STRING; }
