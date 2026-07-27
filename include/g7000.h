/*
 * g7000.h — publieke API van VideopacHorse_Core
 *
 * Portable C11 emulator-core voor de Philips Videopac G7000 / Magnavox Odyssey².
 * Deze header is de ENIGE interface die frontends (Web/WASM, Android/JNI,
 * Steam Deck/SDL2) mogen gebruiken. Geen platform-code in de core; geen I/O,
 * geen malloc buiten g7k_create/g7k_destroy, geen globals.
 *
 * Licentie: AGPL-3.0 — from-scratch implementatie; geen code overgenomen uit
 * O2EM (Clarified Artistic License) of andere emulators. Zie docs/O2EM_DEEPDIVE.md
 * voor de gedrags-/feitenbronnen (datasheets, MAME-gedragsfeiten, No-Intro hashes).
 */
#ifndef G7000_H
#define G7000_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G7K_VERSION_STRING "0.5.1-Kerstens"

typedef struct g7k_sys g7k_sys;

typedef enum { G7K_REGION_PAL = 0, G7K_REGION_NTSC = 1 } g7k_region;

/* Joystick-bitmask (actief = ingedrukt) */
enum {
    G7K_JOY_UP    = 1u << 0,
    G7K_JOY_DOWN  = 1u << 1,
    G7K_JOY_LEFT  = 1u << 2,
    G7K_JOY_RIGHT = 1u << 3,
    G7K_JOY_FIRE  = 1u << 4
};

/* Console-toetsenbord: matrixcode = rij*8 + kolom (rij 0..7, kolom 0..7).
 * De mapping van tekens naar matrixcodes levert de core via g7k_key_from_char. */
#define G7K_KEY_NONE 0xFF

/* -------- levenscyclus -------- */
g7k_sys *g7k_create(void);
void     g7k_destroy(g7k_sys *sys);

/* BIOS: exact 1024 bytes (interne 8048-ROM). Return 0 = ok. */
int g7k_load_bios(g7k_sys *sys, const uint8_t *data, size_t size);

/* Cartridge: raw dump, 2048/4096/8192/12288/16384 bytes. Return 0 = ok.
 * Data wordt gekopieerd; caller mag de buffer vrijgeven. */
int g7k_load_cart(g7k_sys *sys, const uint8_t *data, size_t size);

/* cold=true: power-cycle (RAM/VDC-state gewist); cold=false: reset-toets. */
void g7k_reset(g7k_sys *sys, bool cold);

void       g7k_set_region(g7k_sys *sys, g7k_region region);
g7k_region g7k_get_region(const g7k_sys *sys);

/* -------- uitvoering -------- */
/* Emuleer precies één videoframe (incl. VBLANK). */
void g7k_run_frame(g7k_sys *sys);

/* -------- video -------- */
/* RGBA8888-framebuffer, geldig tot volgende g7k_run_frame/g7k_destroy. */
const uint32_t *g7k_framebuffer(const g7k_sys *sys);
int g7k_fb_width(const g7k_sys *sys);
int g7k_fb_height(const g7k_sys *sys);

/* -------- audio -------- */
/* Mono int16-samples van het laatste frame. Return: aantal geschreven samples. */
int g7k_audio_read(g7k_sys *sys, int16_t *out, int max_samples);
int g7k_audio_sample_rate(const g7k_sys *sys);

/* -------- input -------- */
void g7k_joystick_set(g7k_sys *sys, int player /*0|1*/, uint8_t mask);
void g7k_key_set(g7k_sys *sys, uint8_t matrix_code, bool down);
/* ASCII ('0'-'9','A'-'Z','+','-','*','/','=', ' ', '\n' enter, '\b' clear)
 * naar matrixcode; G7K_KEY_NONE als de console die toets niet heeft. */
uint8_t g7k_key_from_char(char c);

/* -------- savestate -------- */
size_t g7k_state_size(const g7k_sys *sys);
int    g7k_state_save(const g7k_sys *sys, uint8_t *buf, size_t size);
int    g7k_state_load(g7k_sys *sys, const uint8_t *buf, size_t size);

/* -------- introspectie / test-hooks -------- */
uint64_t g7k_cycle_count(const g7k_sys *sys);     /* CPU-cycles sinds cold reset */
uint8_t  g7k_bus_peek(const g7k_sys *sys, uint16_t addr);   /* CPU-adresruimte  */
uint8_t  g7k_intram_peek(const g7k_sys *sys, uint8_t addr); /* 64B interne RAM  */
uint8_t  g7k_extram_peek(const g7k_sys *sys, uint8_t addr); /* 128B externe RAM */
uint8_t  g7k_vdc_peek(const g7k_sys *sys, uint8_t reg);     /* VDC-register     */
const char *g7k_version(void);

#ifdef __cplusplus
}
#endif
#endif /* G7000_H */
