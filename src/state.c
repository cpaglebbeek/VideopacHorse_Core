/*
 * state.c — versioned savestates VideopacHorse_Core (bouwer C)
 *
 * QUIRKS-dekking: S3.
 *
 * Blob-layout (alle multi-byte velden little-endian, expliciet per byte —
 * geen struct-memcpy, dus padding-/endianness-onafhankelijk):
 *
 *   header (20 bytes)
 *     [0..3]   magic 'G','7','K','S'
 *     [4..7]   formaatversie (G7K_STATE_VERSION)
 *     [8..11]  CRC32 over sys->bios (1024 bytes; O2EM-les: savestates
 *              zijn BIOS-gebonden — deep-dive §7.5)
 *     [12..15] machinetype (1 = G7000/Odyssey2)
 *     [16..19] regio (g7k_region)
 *   payload
 *     cpu8048   (alle architectuurstate, excl. bus-wiring)
 *     vdc8244   (regs t/m audio_lfsr; ringbuffer/fb-pointers niet)
 *     cart      (bank + size; GEEN ROM-inhoud)
 *     sys       (extram, p1/p2-schaduw, input-latches,
 *                scheduler-accumulatoren, framenummer)
 *
 * Robuust laden (S3): eerst header valideren (magic/versie/BIOS-CRC/
 * machinetype/regio) en payload volledig in LOKALE kopieën deserialiseren;
 * pas als ALLES klopt wordt in één commit-stap naar *sys geschreven.
 * Elke fout -> -1 en de systeemstate blijft byte-voor-byte onaangetast.
 * Extra guard: cart-size in de blob moet overeenkomen met de geladen cart
 * (een state hervatten op een andere cart is betekenisloos).
 */
#include <string.h>

#include "core_internal.h"

#define G7K_STATE_VERSION      1u
#define G7K_STATE_MACHINE_G7000 1u

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) — bitwise, geen      */
/* tabel: tabellen zouden statische state zijn en 1KB input is klein.  */
/* ------------------------------------------------------------------ */

static uint32_t state_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* writer/reader met bounds-checking; writer met buf==NULL telt alleen */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;               /* NULL = counting-mode */
    size_t   cap;
    size_t   pos;
    bool     fail;
} state_w;

typedef struct {
    const uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     fail;
} state_r;

static void put_u8(state_w *w, uint8_t v)
{
    if (w->buf) {
        if (w->pos < w->cap)
            w->buf[w->pos] = v;
        else
            w->fail = true;
    }
    w->pos++;
}

static void put_u16(state_w *w, uint16_t v)
{
    put_u8(w, (uint8_t)(v & 0xFF));
    put_u8(w, (uint8_t)(v >> 8));
}

static void put_u32(state_w *w, uint32_t v)
{
    put_u16(w, (uint16_t)(v & 0xFFFF));
    put_u16(w, (uint16_t)(v >> 16));
}

static void put_u64(state_w *w, uint64_t v)
{
    put_u32(w, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32(w, (uint32_t)(v >> 32));
}

static void put_bool(state_w *w, bool v)   { put_u8(w, v ? 1u : 0u); }

static void put_bytes(state_w *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        put_u8(w, p[i]);
}

static uint8_t get_u8(state_r *r)
{
    if (r->pos < r->cap)
        return r->buf[r->pos++];
    r->fail = true;
    return 0;
}

static uint16_t get_u16(state_r *r)
{
    uint16_t lo = get_u8(r);
    uint16_t hi = get_u8(r);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static uint32_t get_u32(state_r *r)
{
    uint32_t lo = get_u16(r);
    uint32_t hi = get_u16(r);
    return lo | (hi << 16);
}

static uint64_t get_u64(state_r *r)
{
    uint64_t lo = get_u32(r);
    uint64_t hi = get_u32(r);
    return lo | (hi << 32);
}

static bool get_bool(state_r *r)           { return get_u8(r) != 0; }

static void get_bytes(state_r *r, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = get_u8(r);
}

/* ------------------------------------------------------------------ */
/* per-subsysteem (de ser/deser-paren MOETEN veldsgewijs symmetrisch   */
/* blijven; de roundtrip-test in tests/test_state.c bewaakt dat)       */
/* ------------------------------------------------------------------ */

static void ser_cpu(state_w *w, const cpu8048 *c)
{
    put_u8(w, c->a);
    put_u16(w, c->pc);
    put_u8(w, c->psw);
    put_bytes(w, c->iram, sizeof(c->iram));
    put_u8(w, c->p1_latch);
    put_u8(w, c->p2_latch);
    put_u8(w, c->bus_latch);
    put_bool(w, c->f1);
    put_bool(w, c->dbf);
    put_bool(w, c->a11_irq_hold);
    put_u8(w, c->timer);
    put_u8(w, c->prescaler);
    put_bool(w, c->timer_running);
    put_bool(w, c->counter_mode);
    put_bool(w, c->last_t1);
    put_bool(w, c->tf);
    put_bool(w, c->timer_irq_pending);
    put_bool(w, c->irq_enabled);
    put_bool(w, c->tcnti_enabled);
    put_bool(w, c->irq_line);
    put_bool(w, c->in_irq);
    put_u64(w, c->cycles);
}

static void deser_cpu(state_r *r, cpu8048 *c)
{
    c->a         = get_u8(r);
    c->pc        = get_u16(r);
    c->psw       = get_u8(r);
    get_bytes(r, c->iram, sizeof(c->iram));
    c->p1_latch  = get_u8(r);
    c->p2_latch  = get_u8(r);
    c->bus_latch = get_u8(r);
    c->f1        = get_bool(r);
    c->dbf       = get_bool(r);
    c->a11_irq_hold      = get_bool(r);
    c->timer     = get_u8(r);
    c->prescaler = get_u8(r);
    c->timer_running     = get_bool(r);
    c->counter_mode      = get_bool(r);
    c->last_t1           = get_bool(r);
    c->tf                = get_bool(r);
    c->timer_irq_pending = get_bool(r);
    c->irq_enabled       = get_bool(r);
    c->tcnti_enabled     = get_bool(r);
    c->irq_line          = get_bool(r);
    c->in_irq            = get_bool(r);
    c->cycles    = get_u64(r);
}

static void ser_vdc(state_w *w, const vdc8244 *v)
{
    put_bytes(w, v->regs, sizeof(v->regs));
    put_u8(w, v->status);
    put_u8(w, v->collision);
    put_bool(w, v->irq);
    put_bool(w, v->bright);
    put_u32(w, (uint32_t)v->line);
    put_u32(w, (uint32_t)v->region);
    put_u32(w, v->audio_shift);
    put_u16(w, v->audio_lfsr);
}

static void deser_vdc(state_r *r, vdc8244 *v)
{
    get_bytes(r, v->regs, sizeof(v->regs));
    v->status      = get_u8(r);
    v->collision   = get_u8(r);
    v->irq         = get_bool(r);
    v->bright      = get_bool(r);
    v->line        = (int)(int32_t)get_u32(r);
    v->region      = (g7k_region)get_u32(r);
    v->audio_shift = get_u32(r);
    v->audio_lfsr  = get_u16(r);
}

/* sys-niveau payload minus cpu/vdc (cart inline: bank+size, geen ROM) */
static void ser_sys(state_w *w, const struct g7k_sys *sys)
{
    put_u8(w, sys->cart.bank);
    put_u32(w, sys->cart.size);
    put_bytes(w, sys->extram, sizeof(sys->extram));
    put_u8(w, sys->p1);
    put_u8(w, sys->p2);
    put_bytes(w, sys->joy_pending, sizeof(sys->joy_pending));
    put_bytes(w, sys->joy_live, sizeof(sys->joy_live));
    put_bytes(w, sys->keys_pending, sizeof(sys->keys_pending));
    put_bytes(w, sys->keys_live, sizeof(sys->keys_live));
    put_u32(w, (uint32_t)sys->cyc_frac);
    put_u32(w, (uint32_t)sys->cyc_debt);
    put_u64(w, sys->line_start_cycles);
    put_u64(w, sys->frames);
}

static void ser_all(state_w *w, const struct g7k_sys *sys)
{
    /* header */
    put_u8(w, 'G'); put_u8(w, '7'); put_u8(w, 'K'); put_u8(w, 'S');
    put_u32(w, G7K_STATE_VERSION);
    put_u32(w, state_crc32(sys->bios, sizeof(sys->bios)));
    put_u32(w, G7K_STATE_MACHINE_G7000);
    put_u32(w, (uint32_t)sys->region);
    /* payload */
    ser_cpu(w, &sys->cpu);
    ser_vdc(w, &sys->vdc);
    ser_sys(w, sys);
}

/* ------------------------------------------------------------------ */
/* publieke (interne) API                                              */
/* ------------------------------------------------------------------ */

size_t g7k_state_blob_size(const struct g7k_sys *sys)
{
    if (!sys)
        return 0;
    state_w w = { NULL, 0, 0, false };          /* counting-mode */
    ser_all(&w, sys);
    return w.pos;
}

int g7k_state_blob_save(const struct g7k_sys *sys, uint8_t *buf, size_t size)
{
    if (!sys || !buf)
        return -1;
    state_w w = { buf, size, 0, false };
    ser_all(&w, sys);
    return (w.fail || w.pos > size) ? -1 : 0;
}

int g7k_state_blob_load(struct g7k_sys *sys, const uint8_t *buf, size_t size)
{
    if (!sys || !buf)
        return -1;

    const size_t need = g7k_state_blob_size(sys);
    if (size < need)
        return -1;                              /* afgekapt -> weigeren */

    state_r r = { buf, need, 0, false };

    /* --- header valideren VOOR er iets wordt aangeraakt (S3) --- */
    if (get_u8(&r) != 'G' || get_u8(&r) != '7' ||
        get_u8(&r) != 'K' || get_u8(&r) != 'S')
        return -1;                              /* magic */
    if (get_u32(&r) != G7K_STATE_VERSION)
        return -1;                              /* formaatversie */
    if (get_u32(&r) != state_crc32(sys->bios, sizeof(sys->bios)))
        return -1;                              /* andere BIOS */
    if (get_u32(&r) != G7K_STATE_MACHINE_G7000)
        return -1;                              /* machinetype */
    uint32_t region_raw = get_u32(&r);
    if (region_raw != (uint32_t)G7K_REGION_PAL &&
        region_raw != (uint32_t)G7K_REGION_NTSC)
        return -1;
    if (r.fail)
        return -1;

    /* --- payload in LOKALE kopieën (wiring/pointers blijven staan) --- */
    cpu8048 cpu_tmp = sys->cpu;                 /* behoudt bus-callbacks */
    vdc8244 vdc_tmp = sys->vdc;                 /* behoudt fb/ringbuffer */
    deser_cpu(&r, &cpu_tmp);
    deser_vdc(&r, &vdc_tmp);

    uint8_t  cart_bank = get_u8(&r);
    uint32_t cart_size = get_u32(&r);
    uint8_t  extram_tmp[sizeof(sys->extram)];
    get_bytes(&r, extram_tmp, sizeof(extram_tmp));
    uint8_t  p1_tmp = get_u8(&r);
    uint8_t  p2_tmp = get_u8(&r);
    uint8_t  joyp_tmp[sizeof(sys->joy_pending)];
    uint8_t  joyl_tmp[sizeof(sys->joy_live)];
    uint8_t  keyp_tmp[sizeof(sys->keys_pending)];
    uint8_t  keyl_tmp[sizeof(sys->keys_live)];
    get_bytes(&r, joyp_tmp, sizeof(joyp_tmp));
    get_bytes(&r, joyl_tmp, sizeof(joyl_tmp));
    get_bytes(&r, keyp_tmp, sizeof(keyp_tmp));
    get_bytes(&r, keyl_tmp, sizeof(keyl_tmp));
    int      cyc_frac_tmp = (int)(int32_t)get_u32(&r);
    int      cyc_debt_tmp = (int)(int32_t)get_u32(&r);
    uint64_t lsc_tmp      = get_u64(&r);
    uint64_t frames_tmp   = get_u64(&r);

    if (r.fail)
        return -1;                              /* mag niet: need geverifieerd */
    if (cart_size != sys->cart.size)
        return -1;                              /* andere cart geladen */
    if (cart_bank > 3)
        return -1;                              /* bank = P1 & 3, nooit > 3 */

    /* --- commit: pas nu, en in één keer, de systeemstate muteren --- */
    sys->region = (g7k_region)region_raw;
    sys->cpu    = cpu_tmp;
    sys->vdc    = vdc_tmp;
    sys->cart.bank = cart_bank;                 /* ROM-inhoud blijft staan */
    memcpy(sys->extram, extram_tmp, sizeof(sys->extram));
    sys->p1 = p1_tmp;
    sys->p2 = p2_tmp;
    memcpy(sys->joy_pending, joyp_tmp, sizeof(sys->joy_pending));
    memcpy(sys->joy_live, joyl_tmp, sizeof(sys->joy_live));
    memcpy(sys->keys_pending, keyp_tmp, sizeof(sys->keys_pending));
    memcpy(sys->keys_live, keyl_tmp, sizeof(sys->keys_live));
    sys->cyc_frac = cyc_frac_tmp;
    sys->cyc_debt = cyc_debt_tmp;
    sys->line_start_cycles = lsc_tmp;
    sys->frames = frames_tmp;
    return 0;
}
