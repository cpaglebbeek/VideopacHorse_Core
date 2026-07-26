/*
 * test_cpu.c — CPU-tests Intel 8048 (QUIRKS C1..C9 + brede opcode-dekking).
 *
 * BIOS-loos en ROM-loos: alle programma's zijn eigen synthetische
 * 8048-byte-arrays. Twee routes:
 *   1) directe cpu8048-aansturing via core_internal.h + een eigen
 *      test-bus (ARCHITECTURE.md: tests mogen interne headers gebruiken) —
 *      nodig voor cycle-exacte en interrupt-timing-checks;
 *   2) de publieke API (g7k_create + g7k_load_bios + g7k_run_frame +
 *      g7k_*_peek) als integratie-check.
 *
 * Registratie: register_cpu_tests(); de integrator roept die aan vanuit
 * test_main.c.
 */
#include <string.h>

#include "../include/g7000.h"
#include "core_internal.h"

void g7k_register_test(const char *name, int (*fn)(void));

#define CHECK(cond) do { if (!(cond)) return 1; } while (0)

/* PSW-bits (spiegel van de datasheet-layout; onafhankelijk gedefinieerd) */
#define T_CY 0x80u
#define T_AC 0x40u
#define T_F0 0x20u
#define T_BS 0x10u

/* ------------------------------------------------------------------ */
/* test-bus: 4KB ROM + 256B ext-RAM + poorten/T0/T1 in een struct      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t rom[4096];
    uint8_t xram[256];
    uint8_t p1, p2, busv;      /* laatst naar buiten geschreven waarden  */
    uint8_t p1_ext, p2_ext;    /* wat de buitenwereld laag trekt (AND)   */
    uint8_t bus_in;            /* wat INS A,BUS leest                    */
    int     p1_writes;
    bool    t0, t1;
} tenv;

static uint8_t tb_read_rom(void *ctx, uint16_t addr)
{
    return ((tenv *)ctx)->rom[addr & 0x0FFF];
}
static uint8_t tb_read_ext(void *ctx, uint8_t addr)
{
    return ((tenv *)ctx)->xram[addr];
}
static void tb_write_ext(void *ctx, uint8_t addr, uint8_t data)
{
    ((tenv *)ctx)->xram[addr] = data;
}
static void tb_write_p1(void *ctx, uint8_t val)
{
    tenv *e = ctx;
    e->p1 = val;
    e->p1_writes++;
}
static uint8_t tb_read_p1(void *ctx, uint8_t latch)
{
    return (uint8_t)(latch & ((tenv *)ctx)->p1_ext);
}
static void tb_write_p2(void *ctx, uint8_t val)
{
    ((tenv *)ctx)->p2 = val;
}
static uint8_t tb_read_p2(void *ctx, uint8_t latch)
{
    return (uint8_t)(latch & ((tenv *)ctx)->p2_ext);
}
static uint8_t tb_read_bus(void *ctx)
{
    return ((tenv *)ctx)->bus_in;
}
static void tb_write_bus(void *ctx, uint8_t val)
{
    ((tenv *)ctx)->busv = val;
}
static bool tb_read_t0(void *ctx) { return ((tenv *)ctx)->t0; }
static bool tb_read_t1(void *ctx) { return ((tenv *)ctx)->t1; }

/* verse CPU + omgeving; reset uitgevoerd (P1/P2-latch 0xFF, C8) */
static void tb_init(tenv *e, cpu8048 *cpu)
{
    cpu8048_bus bus;

    memset(e, 0, sizeof(*e));
    e->p1_ext = 0xFF;
    e->p2_ext = 0xFF;
    e->bus_in = 0xFF;

    memset(&bus, 0, sizeof(bus));
    bus.ctx       = e;
    bus.read_rom  = tb_read_rom;
    bus.read_ext  = tb_read_ext;
    bus.write_ext = tb_write_ext;
    bus.write_p1  = tb_write_p1;
    bus.read_p1   = tb_read_p1;
    bus.write_p2  = tb_write_p2;
    bus.read_p2   = tb_read_p2;
    bus.read_bus  = tb_read_bus;
    bus.write_bus = tb_write_bus;
    bus.read_t0   = tb_read_t0;
    bus.read_t1   = tb_read_t1;

    cpu8048_init(cpu, &bus);
    cpu8048_reset(cpu);
}

/* n instructies stappen; som van verbruikte cycli terug */
static int tb_steps(cpu8048 *cpu, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += cpu8048_step(cpu);
    return total;
}

/* ------------------------------------------------------------------ */
/* C1 — conditionele branch blijft binnen de 256-byte pagina van de    */
/*      VOLGENDE instructie                                            */
/* ------------------------------------------------------------------ */

static int test_C1_cond_branch_page(void)
{
    tenv e; cpu8048 c;

    /* binnen de pagina: JC op 0x150 -> doel 0x120 */
    tb_init(&e, &c);
    e.rom[0x150] = 0xF6;               /* JC 0x20 */
    e.rom[0x151] = 0x20;
    c.pc = 0x150;
    c.psw |= T_CY;
    cpu8048_step(&c);
    CHECK(c.pc == 0x120);

    /* over de paginagrens: branch-instructie op 0x2FE/0x2FF -> volgende
     * instructie ligt op 0x300 -> doelpagina is 0x300, NIET 0x200 */
    tb_init(&e, &c);
    e.rom[0x2FE] = 0xF6;               /* JC 0x10 */
    e.rom[0x2FF] = 0x10;
    c.pc = 0x2FE;
    c.psw |= T_CY;
    cpu8048_step(&c);
    CHECK(c.pc == 0x310);

    /* niet genomen: PC gewoon voorbij de 2-byte instructie */
    tb_init(&e, &c);
    e.rom[0x150] = 0xF6;
    e.rom[0x151] = 0x20;
    c.pc = 0x150;                      /* CY = 0 */
    cpu8048_step(&c);
    CHECK(c.pc == 0x152);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C2 — 11-bit PC wrapt op 0x7FF naar 0x000                            */
/* ------------------------------------------------------------------ */

static int test_C2_pc_wrap_07ff(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0x7FF] = 0x00;               /* NOP */
    c.pc = 0x7FF;
    cpu8048_step(&c);
    CHECK(c.pc == 0x000);

    /* 2-byte instructie op 0x7FF: operand komt van 0x000 */
    tb_init(&e, &c);
    e.rom[0x7FF] = 0x23;               /* MOV A,# */
    e.rom[0x000] = 0x5A;
    c.pc = 0x7FF;
    cpu8048_step(&c);
    CHECK(c.a == 0x5A);
    CHECK(c.pc == 0x001);

    /* in de bovenste bank blijft bit 11 staan: 0xFFF -> 0x800 */
    tb_init(&e, &c);
    e.rom[0xFFF] = 0x00;               /* NOP */
    c.pc = 0xFFF;
    cpu8048_step(&c);
    CHECK(c.pc == 0x800);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C3 — DA A (BCD)                                                     */
/* ------------------------------------------------------------------ */

static int run_daa(uint8_t a, uint8_t psw_in, uint8_t *a_out, uint8_t *psw_out)
{
    tenv e; cpu8048 c;
    tb_init(&e, &c);
    e.rom[0] = 0x57;                   /* DA A */
    c.a = a;
    c.psw = (uint8_t)(c.psw | psw_in);
    cpu8048_step(&c);
    *a_out = c.a;
    *psw_out = c.psw;
    return 0;
}

static int test_C3_daa_bcd(void)
{
    uint8_t a, psw;
    tenv e; cpu8048 c;

    /* 0x09 + 0x08 = 0x11 binair (AC gezet) -> DA -> 0x17 */
    tb_init(&e, &c);
    e.rom[0] = 0x03; e.rom[1] = 0x08;  /* ADD A,#0x08 */
    e.rom[2] = 0x57;                   /* DA A */
    c.a = 0x09;
    tb_steps(&c, 2);
    CHECK(c.a == 0x17);
    CHECK(!(c.psw & T_CY));

    /* 0x99 + 0x01 = 0x9A -> DA -> 0x00 + CY */
    tb_init(&e, &c);
    e.rom[0] = 0x03; e.rom[1] = 0x01;
    e.rom[2] = 0x57;
    c.a = 0x99;
    tb_steps(&c, 2);
    CHECK(c.a == 0x00);
    CHECK(c.psw & T_CY);

    /* alleen lage nibble > 9: 0x3C -> 0x42 */
    run_daa(0x3C, 0, &a, &psw);
    CHECK(a == 0x42);
    CHECK(!(psw & T_CY));

    /* AC gezet, nibbles zelf < 10: 0x11 + AC -> 0x17 */
    run_daa(0x11, T_AC, &a, &psw);
    CHECK(a == 0x17);

    /* CY vooraf gezet: alleen hoge correctie: 0x12 + CY -> 0x72, CY blijft */
    run_daa(0x12, T_CY, &a, &psw);
    CHECK(a == 0x72);
    CHECK(psw & T_CY);

    /* al geldig BCD: onaangeroerd */
    run_daa(0x45, 0, &a, &psw);
    CHECK(a == 0x45);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C4 — JNI: springt op INT-lijn laag, ook met interrupts uitgeschakeld */
/* ------------------------------------------------------------------ */

static int test_C4_jni(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x86;                   /* JNI 0x40 */
    e.rom[1] = 0x40;
    cpu8048_step(&c);                  /* lijn niet asserted: geen sprong */
    CHECK(c.pc == 0x002);

    tb_init(&e, &c);
    e.rom[0] = 0x86;
    e.rom[1] = 0x40;
    cpu8048_set_irq(&c, true);         /* lijn laag; EN I NIET uitgevoerd */
    cpu8048_step(&c);
    CHECK(c.pc == 0x040);              /* sprong, geen vectoring */
    return 0;
}

/* ------------------------------------------------------------------ */
/* C5 — externe interrupt: vector 0x003, state, prioriteit, RETR       */
/* ------------------------------------------------------------------ */

static int test_C5_ext_irq_vector_state(void)
{
    tenv e; cpu8048 c;
    int cyc;

    tb_init(&e, &c);
    e.rom[0x000] = 0x05;               /* EN I */
    e.rom[0x001] = 0x00;               /* NOP */
    e.rom[0x002] = 0x00;               /* NOP */
    e.rom[0x003] = 0x17;               /* ISR: INC A */
    e.rom[0x004] = 0x93;               /* RETR */

    cpu8048_step(&c);                  /* EN I; pc=1 */
    c.psw |= T_CY;                     /* moet door RETR terugkomen */
    cpu8048_set_irq(&c, true);

    cyc = cpu8048_step(&c);            /* dispatch VOOR de fetch */
    CHECK(cyc == 2);                   /* acknowledge = 2 cycli (C6) */
    CHECK(c.pc == 0x003);
    CHECK(c.in_irq);
    CHECK((c.psw & 0x07) == 1);        /* SP = 1 */
    CHECK(c.iram[8] == 0x01);          /* return-adres laag */
    CHECK((c.iram[9] & 0x0F) == 0x00); /* return-adres hoog */
    CHECK((c.iram[9] & 0xF0) == (uint8_t)(T_CY | 0x00)); /* PSW[7:4] mee */

    cpu8048_step(&c);                  /* INC A in ISR */
    c.psw &= (uint8_t)~T_CY;           /* ISR verpest CY... */
    cpu8048_step(&c);                  /* RETR */
    CHECK(c.pc == 0x001);
    CHECK(c.psw & T_CY);               /* ...RETR herstelt PSW[7:4] */
    CHECK(!c.in_irq);
    CHECK((c.psw & 0x07) == 0);

    /* lijn nog steeds asserted: meteen opnieuw vectoren */
    cyc = cpu8048_step(&c);
    CHECK(cyc == 2);
    CHECK(c.pc == 0x003);

    /* binnen ISR geen tweede dispatch */
    cpu8048_step(&c);                  /* INC A */
    CHECK(c.pc == 0x004);
    cpu8048_set_irq(&c, false);
    cpu8048_step(&c);                  /* RETR */
    CHECK(c.pc == 0x001);
    return 0;
}

static int test_C5_priority_ext_over_timer(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x00;
    c.irq_enabled = true;
    c.tcnti_enabled = true;
    c.timer_irq_pending = true;
    cpu8048_set_irq(&c, true);
    cpu8048_step(&c);
    CHECK(c.pc == 0x003);              /* extern wint van timer */
    CHECK(c.timer_irq_pending);        /* timer-request blijft staan */
    return 0;
}

static int test_C5_irq_suppresses_dbf(void)
{
    tenv e; cpu8048 c;

    /* hoofdprogramma (op 0x010, weg van de vectorruimte) zet MB1;
     * in de ISR moet JMP toch in bank 0 landen */
    tb_init(&e, &c);
    e.rom[0x010] = 0x05;               /* EN I */
    e.rom[0x011] = 0xF5;               /* SEL MB1 */
    e.rom[0x012] = 0x00;               /* NOP (wordt onderbroken) */
    e.rom[0x013] = 0x04;               /* na RETR: JMP 0x010 -> 0x810! */
    e.rom[0x014] = 0x10;
    e.rom[0x003] = 0x04;               /* ISR: JMP 0x080 (pagina 0) */
    e.rom[0x004] = 0x80;
    e.rom[0x080] = 0x93;               /* RETR */
    e.rom[0x810] = 0x00;               /* NOP in de bovenste bank */

    c.pc = 0x010;
    tb_steps(&c, 2);                   /* EN I + SEL MB1; pc=0x012 */
    CHECK(c.dbf);
    cpu8048_set_irq(&c, true);
    cpu8048_step(&c);                  /* vector 0x003 */
    CHECK(c.pc == 0x003);
    cpu8048_step(&c);                  /* JMP in ISR: DBF onderdrukt */
    CHECK(c.pc == 0x080);              /* bank 0, ondanks DBF=1 */
    cpu8048_set_irq(&c, false);
    cpu8048_step(&c);                  /* RETR -> pc=0x012 */
    CHECK(c.pc == 0x012);
    cpu8048_step(&c);                  /* NOP -> pc=0x013 */
    CHECK(c.pc == 0x013);
    cpu8048_step(&c);                  /* JMP: DBF geldt weer */
    CHECK(c.pc == 0x810);
    return 0;
}

static int test_C5_irq_return_addr_before_instr(void)
{
    tenv e; cpu8048 c;

    /* dispatch gebeurt voor de fetch: return-adres = niet-uitgevoerde
     * instructie, ook midden in een reeks 2-cycle-instructies */
    tb_init(&e, &c);
    e.rom[0x000] = 0x05;               /* EN I */
    e.rom[0x001] = 0x23; e.rom[0x002] = 0x11;   /* MOV A,#0x11 */
    e.rom[0x003] = 0x93;               /* ISR: RETR */
    /* let op: 0x003 is ook de vector; hoofdcode ligt eromheen */
    e.rom[0x004] = 0x23; e.rom[0x005] = 0x22;   /* MOV A,#0x22 */

    cpu8048_step(&c);                  /* EN I */
    cpu8048_step(&c);                  /* MOV A,#0x11; pc=3 */
    CHECK(c.a == 0x11);
    cpu8048_set_irq(&c, true);
    cpu8048_step(&c);                  /* vector; return-adres = 3 */
    CHECK(c.pc == 0x003);
    CHECK(c.iram[8] == 0x03);
    cpu8048_set_irq(&c, false);
    cpu8048_step(&c);                  /* RETR (0x003) -> terug naar 3 */
    CHECK(c.pc == 0x003);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C6 — cycle-getrouwe timing                                          */
/* ------------------------------------------------------------------ */

static int test_C6_cycle_timing(void)
{
    tenv e; cpu8048 c;
    int i = 0;

    tb_init(&e, &c);
    /* programma met bekende cycli:
       NOP(1) MOV A,#(2) INC R0(1) OUTL P1,A(2) IN A,P1(2) INS A,BUS(2)
       MOVX @R0,A(2) MOVX A,@R0(2) MOVP A,@A(2) CLR A(1) SWAP A(1)
       CALL(2) [op 0x40: RET(2)] JMP(2) */
    e.rom[i++] = 0x00;                 /* NOP            1 */
    e.rom[i++] = 0x23; e.rom[i++] = 0x34; /* MOV A,#     2 */
    e.rom[i++] = 0x18;                 /* INC R0         1 */
    e.rom[i++] = 0x39;                 /* OUTL P1,A      2 */
    e.rom[i++] = 0x09;                 /* IN A,P1        2 */
    e.rom[i++] = 0x08;                 /* INS A,BUS      2 */
    e.rom[i++] = 0x90;                 /* MOVX @R0,A     2 */
    e.rom[i++] = 0x80;                 /* MOVX A,@R0     2 */
    e.rom[i++] = 0xA3;                 /* MOVP A,@A      2 */
    e.rom[i++] = 0x27;                 /* CLR A          1 */
    e.rom[i++] = 0x47;                 /* SWAP A         1 */
    e.rom[i++] = 0x14; e.rom[i++] = 0x40; /* CALL 0x040  2 */
    e.rom[0x40] = 0x83;                /* RET            2 */
    e.rom[i++] = 0x04; e.rom[i++] = 0x00; /* JMP 0x000   2 */

    CHECK(cpu8048_step(&c) == 1);      /* NOP */
    CHECK(cpu8048_step(&c) == 2);      /* MOV A,# */
    CHECK(cpu8048_step(&c) == 1);      /* INC R0 */
    CHECK(cpu8048_step(&c) == 2);      /* OUTL P1,A */
    CHECK(cpu8048_step(&c) == 2);      /* IN A,P1 */
    CHECK(cpu8048_step(&c) == 2);      /* INS A,BUS */
    CHECK(cpu8048_step(&c) == 2);      /* MOVX @R0,A */
    CHECK(cpu8048_step(&c) == 2);      /* MOVX A,@R0 */
    CHECK(cpu8048_step(&c) == 2);      /* MOVP A,@A */
    CHECK(cpu8048_step(&c) == 1);      /* CLR A */
    CHECK(cpu8048_step(&c) == 1);      /* SWAP A */
    CHECK(cpu8048_step(&c) == 2);      /* CALL */
    CHECK(cpu8048_step(&c) == 2);      /* RET */
    CHECK(cpu8048_step(&c) == 2);      /* JMP */
    CHECK(c.cycles == 24);             /* som klopt met de teller */
    CHECK(c.pc == 0x000);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C7 — T1-conditie (JT1/JNT1) op CPU-niveau. De echte bedrading
 * (T1 = VBLANK OF HBLANK via sys_read_t1 + vdc8244_vblank/hblank_at)
 * wordt end-to-end getest in tests/test_sys.c
 * (test_C7_t1_wiring_vbl_hbl) en unit-getest in tests/test_vdc.c
 * (test_C7_vblank_hblank_units).                                      */
/* ------------------------------------------------------------------ */

static int test_C7_t1_conditions(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x56; e.rom[1] = 0x30;  /* JT1 0x30 */
    e.t1 = true;
    cpu8048_step(&c);
    CHECK(c.pc == 0x030);

    tb_init(&e, &c);
    e.rom[0] = 0x56; e.rom[1] = 0x30;
    e.t1 = false;
    cpu8048_step(&c);
    CHECK(c.pc == 0x002);

    tb_init(&e, &c);
    e.rom[0] = 0x46; e.rom[1] = 0x30;  /* JNT1 0x30 */
    e.t1 = false;
    cpu8048_step(&c);
    CHECK(c.pc == 0x030);

    /* JT0/JNT0 idem op T0 */
    tb_init(&e, &c);
    e.rom[0] = 0x36; e.rom[1] = 0x30;  /* JT0 */
    e.t0 = true;
    cpu8048_step(&c);
    CHECK(c.pc == 0x030);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C8 — reset: P1=P2=0xFF, doorgepompt via de callbacks                */
/* ------------------------------------------------------------------ */

static int test_C8_reset_p1_ff(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);                   /* tb_init doet cpu8048_reset */
    CHECK(c.p1_latch == 0xFF);
    CHECK(c.p2_latch == 0xFF);
    CHECK(c.bus_latch == 0xFF);
    CHECK(e.p1 == 0xFF);               /* write_p1(0xFF) is echt gebeld */
    CHECK(e.p2 == 0xFF);
    CHECK(e.p1_writes >= 1);
    CHECK(c.pc == 0x000);
    CHECK((c.psw & 0x07) == 0);        /* SP leeg */
    CHECK(!(c.psw & T_BS));            /* RB0 */
    CHECK(!c.dbf);                     /* MB0 */
    CHECK(!c.irq_enabled);
    CHECK(!c.tcnti_enabled);
    CHECK(!c.timer_running);
    return 0;
}

/* ------------------------------------------------------------------ */
/* C9 — MB0/MB1 (DBF) + JMP/CALL over de 2K-grens                      */
/* ------------------------------------------------------------------ */

static int test_C9_mb_bank_jmp_call(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0x000] = 0xF5;               /* SEL MB1 */
    e.rom[0x001] = 0x04; e.rom[0x002] = 0x10; /* JMP 0x010 -> 0x810 */
    e.rom[0x810] = 0xE5;               /* SEL MB0 */
    e.rom[0x811] = 0x14; e.rom[0x812] = 0x20; /* CALL 0x020 -> 0x020 */
    e.rom[0x020] = 0x83;               /* RET -> 0x813 (bit 11 v/d stack) */
    e.rom[0x813] = 0x96; e.rom[0x814] = 0x30; /* JNZ 0x30 -> 0x830 */
    e.rom[0x830] = 0x04; e.rom[0x831] = 0x00; /* JMP 0x000 -> 0x000 (MB0) */

    cpu8048_step(&c);                  /* SEL MB1 */
    CHECK(c.dbf);
    cpu8048_step(&c);                  /* JMP: A11 uit DBF */
    CHECK(c.pc == 0x810);
    cpu8048_step(&c);                  /* SEL MB0 */
    CHECK(!c.dbf);
    cpu8048_step(&c);                  /* CALL naar bank 0 */
    CHECK(c.pc == 0x020);
    CHECK(c.iram[8] == 0x13);          /* return laag = 0x13 */
    CHECK((c.iram[9] & 0x0F) == 0x08); /* return hoog bevat bit 11! */
    cpu8048_step(&c);                  /* RET: volledige 12 bits terug */
    CHECK(c.pc == 0x813);
    c.a = 1;
    cpu8048_step(&c);                  /* JNZ binnen de bovenste bank */
    CHECK(c.pc == 0x830);              /* pagina behoudt bit 11 (C1+C9) */
    cpu8048_step(&c);                  /* JMP met DBF=0 -> bank 0 */
    CHECK(c.pc == 0x000);
    return 0;
}

/* ------------------------------------------------------------------ */
/* timer/counter                                                       */
/* ------------------------------------------------------------------ */

static int test_timer_prescaler_div32(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x55;                   /* STRT T */
    memset(e.rom + 1, 0x00, 64);       /* NOPs */
    cpu8048_step(&c);                  /* STRT T: prescaler-tik 1 */
    /* implementatiekeuze (gedocumenteerd in cpu8048.c): eerste increment
     * exact 32 cycli na STRT T, geteld vanaf de STRT T-cyclus zelf */
    tb_steps(&c, 30);                  /* 31 cycli totaal */
    CHECK(c.timer == 0);
    cpu8048_step(&c);                  /* 32e cyclus */
    CHECK(c.timer == 1);
    tb_steps(&c, 31);
    CHECK(c.timer == 1);
    cpu8048_step(&c);                  /* 64e cyclus */
    CHECK(c.timer == 2);
    CHECK(!c.tf);
    return 0;
}

static int test_timer_overflow_tf_jtf(void)
{
    tenv e; cpu8048 c;
    int i;

    tb_init(&e, &c);
    e.rom[0] = 0x23; e.rom[1] = 0xFF;  /* MOV A,#0xFF */
    e.rom[2] = 0x62;                   /* MOV T,A */
    e.rom[3] = 0x55;                   /* STRT T */
    for (i = 4; i < 0x40; i++)
        e.rom[i] = 0x00;               /* NOPs */
    e.rom[0x40] = 0x16; e.rom[0x41] = 0x60; /* JTF 0x60 */
    e.rom[0x60] = 0x16; e.rom[0x61] = 0x70; /* JTF 0x70 (mag NIET springen) */

    tb_steps(&c, 3);                   /* MOV/MOV T/STRT T */
    CHECK(c.timer == 0xFF);
    tb_steps(&c, 31);                  /* 31 NOPs = cyclus 32 na STRT T */
    CHECK(c.timer == 0x00);            /* overflow */
    CHECK(c.tf);
    CHECK(c.timer_irq_pending);        /* request gelatcht */

    c.pc = 0x40;
    cpu8048_step(&c);                  /* JTF: springt en wist TF */
    CHECK(c.pc == 0x060);
    CHECK(!c.tf);
    cpu8048_step(&c);                  /* JTF: TF is weg -> geen sprong */
    CHECK(c.pc == 0x062);
    return 0;
}

static int test_timer_irq_vector7(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0x007] = 0x93;               /* timer-ISR: RETR */
    memset(e.rom + 0x10, 0x00, 0x40);  /* NOPs */
    c.pc = 0x10;
    c.tcnti_enabled = true;
    c.timer = 0xFF;
    c.timer_running = true;
    c.prescaler = 31;                  /* volgende cyclus -> overflow */
    cpu8048_step(&c);                  /* NOP: timer klapt over */
    CHECK(c.timer_irq_pending);
    CHECK(cpu8048_step(&c) == 2);      /* dispatch naar 0x007 */
    CHECK(c.pc == 0x007);
    CHECK(!c.timer_irq_pending);       /* request geconsumeerd */
    CHECK(c.in_irq);
    cpu8048_step(&c);                  /* RETR */
    CHECK(!c.in_irq);
    CHECK(c.pc == 0x011);
    return 0;
}

static int test_timer_dis_tcnti_clears_pending(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x35;                   /* DIS TCNTI */
    e.rom[1] = 0x25;                   /* EN TCNTI */
    e.rom[2] = 0x00;                   /* NOP */
    c.timer_irq_pending = true;
    cpu8048_step(&c);                  /* DIS TCNTI wist de request */
    CHECK(!c.timer_irq_pending);
    cpu8048_step(&c);                  /* EN TCNTI */
    cpu8048_step(&c);                  /* NOP: geen vector meer */
    CHECK(c.pc == 0x003);
    return 0;
}

static int test_counter_t1_falling_edge(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x45;                   /* STRT CNT */
    memset(e.rom + 1, 0x00, 16);       /* NOPs */
    e.t1 = true;
    cpu8048_step(&c);                  /* STRT CNT: samplet T1=1 */
    CHECK(c.counter_mode);
    e.t1 = false;
    cpu8048_step(&c);                  /* NOP: flank 1->0 telt */
    CHECK(c.timer == 1);
    cpu8048_step(&c);                  /* T1 blijft laag: niets */
    CHECK(c.timer == 1);
    e.t1 = true;
    cpu8048_step(&c);                  /* stijgende flank: niets */
    CHECK(c.timer == 1);
    e.t1 = false;
    cpu8048_step(&c);                  /* tweede 1->0-flank */
    CHECK(c.timer == 2);
    /* STOP TCNT bevriest de teller */
    e.rom[c.pc] = 0x65;                /* STOP TCNT */
    cpu8048_step(&c);
    e.t1 = true;
    cpu8048_step(&c);
    e.t1 = false;
    cpu8048_step(&c);
    CHECK(c.timer == 2);
    return 0;
}

/* ------------------------------------------------------------------ */
/* registers, banken, flags, stack                                     */
/* ------------------------------------------------------------------ */

static int test_regbanks_rb0_rb1(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xB8; e.rom[1] = 0x12;  /* MOV R0,#0x12 (RB0) */
    e.rom[2] = 0xD5;                   /* SEL RB1 */
    e.rom[3] = 0xB8; e.rom[4] = 0x34;  /* MOV R0,#0x34 (RB1) */
    e.rom[5] = 0xC5;                   /* SEL RB0 */
    e.rom[6] = 0xF8;                   /* MOV A,R0 */
    tb_steps(&c, 5);
    CHECK(c.a == 0x12);                /* RB0-R0 */
    CHECK(c.iram[0] == 0x12);          /* RB0 = iram 0..7 */
    CHECK(c.iram[24] == 0x34);         /* RB1 = iram 24..31 */
    return 0;
}

static int test_flags_add_addc_ac_cy(void)
{
    tenv e; cpu8048 c;

    /* 0x0F + 0x01: AC wel, CY niet */
    tb_init(&e, &c);
    e.rom[0] = 0x03; e.rom[1] = 0x01;
    c.a = 0x0F;
    cpu8048_step(&c);
    CHECK(c.a == 0x10);
    CHECK(c.psw & T_AC);
    CHECK(!(c.psw & T_CY));

    /* 0xFF + 0x01: CY en AC */
    tb_init(&e, &c);
    e.rom[0] = 0x03; e.rom[1] = 0x01;
    c.a = 0xFF;
    cpu8048_step(&c);
    CHECK(c.a == 0x00);
    CHECK(c.psw & T_CY);
    CHECK(c.psw & T_AC);

    /* ADDC telt de carry mee; ADD niet */
    tb_init(&e, &c);
    e.rom[0] = 0x13; e.rom[1] = 0x00;  /* ADDC A,#0 */
    c.a = 0x10;
    c.psw |= T_CY;
    cpu8048_step(&c);
    CHECK(c.a == 0x11);
    CHECK(!(c.psw & T_CY));            /* opnieuw berekend */

    tb_init(&e, &c);
    e.rom[0] = 0x03; e.rom[1] = 0x00;  /* ADD A,#0 negeert CY */
    c.a = 0x10;
    c.psw |= T_CY;
    cpu8048_step(&c);
    CHECK(c.a == 0x10);

    /* CLR C / CPL C */
    tb_init(&e, &c);
    e.rom[0] = 0xA7;                   /* CPL C */
    e.rom[1] = 0x97;                   /* CLR C */
    cpu8048_step(&c);
    CHECK(c.psw & T_CY);
    cpu8048_step(&c);
    CHECK(!(c.psw & T_CY));
    return 0;
}

static int test_rotates_rrc_rlc(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x67;                   /* RRC A */
    e.rom[1] = 0xF7;                   /* RLC A */
    e.rom[2] = 0x77;                   /* RR A */
    e.rom[3] = 0xE7;                   /* RL A */
    c.a = 0x01;
    cpu8048_step(&c);                  /* RRC: CY<-1, A=0x00 */
    CHECK(c.a == 0x00);
    CHECK(c.psw & T_CY);
    cpu8048_step(&c);                  /* RLC: A=0x01, CY<-0 */
    CHECK(c.a == 0x01);
    CHECK(!(c.psw & T_CY));
    c.a = 0x81;
    cpu8048_step(&c);                  /* RR: 0x81 -> 0xC0 */
    CHECK(c.a == 0xC0);
    cpu8048_step(&c);                  /* RL: 0xC0 -> 0x81 */
    CHECK(c.a == 0x81);
    return 0;
}

static int test_stack_call_ret_nested(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0x000] = 0x14; e.rom[0x001] = 0x50; /* CALL 0x050 */
    e.rom[0x050] = 0x34; e.rom[0x051] = 0x80; /* CALL 0x180 */
    e.rom[0x180] = 0x83;                      /* RET */
    e.rom[0x052] = 0x83;                      /* RET */

    cpu8048_step(&c);
    CHECK(c.pc == 0x050);
    CHECK((c.psw & 0x07) == 1);
    CHECK(c.iram[8] == 0x02);
    cpu8048_step(&c);
    CHECK(c.pc == 0x180);
    CHECK((c.psw & 0x07) == 2);
    CHECK(c.iram[10] == 0x52);         /* niveau 2 op iram 10/11 */
    CHECK((c.iram[11] & 0x0F) == 0x00);
    cpu8048_step(&c);                  /* RET */
    CHECK(c.pc == 0x052);
    CHECK((c.psw & 0x07) == 1);
    cpu8048_step(&c);                  /* RET */
    CHECK(c.pc == 0x002);
    CHECK((c.psw & 0x07) == 0);
    return 0;
}

static int test_psw_mov_and_sp(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xD7;                   /* MOV PSW,A */
    e.rom[1] = 0xC7;                   /* MOV A,PSW */
    e.rom[2] = 0x14; e.rom[3] = 0x40;  /* CALL 0x040 */
    c.a = 0x03;                        /* SP=3 */
    cpu8048_step(&c);
    CHECK((c.psw & 0x07) == 3);
    c.a = 0;
    cpu8048_step(&c);
    CHECK(c.a == (uint8_t)(c.psw | 0x08)); /* bit 3 leest 1 */
    cpu8048_step(&c);                  /* CALL pusht op niveau 3 */
    CHECK(c.iram[8 + 2 * 3] == 0x04);
    CHECK((c.psw & 0x07) == 4);
    return 0;
}

/* ------------------------------------------------------------------ */
/* poorten, MOVX, MOVP/MOVP3/JMPP, XCHD, DJNZ                          */
/* ------------------------------------------------------------------ */

static int test_ports_latch_anl_orl(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0x23; e.rom[1] = 0x5A;  /* MOV A,#0x5A */
    e.rom[2] = 0x39;                   /* OUTL P1,A */
    e.rom[3] = 0x99; e.rom[4] = 0xF0;  /* ANL P1,#0xF0 */
    e.rom[5] = 0x89; e.rom[6] = 0x0F;  /* ORL P1,#0x0F */
    e.rom[7] = 0x09;                   /* IN A,P1 */
    tb_steps(&c, 2);
    CHECK(e.p1 == 0x5A);
    CHECK(c.p1_latch == 0x5A);
    cpu8048_step(&c);
    CHECK(e.p1 == 0x50);               /* ANL werkt op de latch */
    cpu8048_step(&c);
    CHECK(e.p1 == 0x5F);
    /* quasi-bidirectioneel: buitenwereld trekt bits laag */
    e.p1_ext = 0x0F;
    cpu8048_step(&c);
    CHECK(c.a == 0x0F);                /* latch 0x5F & extern 0x0F */

    /* P2 + BUS */
    tb_init(&e, &c);
    e.rom[0] = 0x23; e.rom[1] = 0xA5;  /* MOV A,#0xA5 */
    e.rom[2] = 0x3A;                   /* OUTL P2,A */
    e.rom[3] = 0x02;                   /* OUTL BUS,A */
    e.rom[4] = 0x98; e.rom[5] = 0x0F;  /* ANL BUS,#0x0F */
    e.rom[6] = 0x08;                   /* INS A,BUS */
    tb_steps(&c, 3);
    CHECK(e.p2 == 0xA5);
    CHECK(e.busv == 0xA5);
    cpu8048_step(&c);
    CHECK(e.busv == 0x05);
    e.bus_in = 0x3C;
    cpu8048_step(&c);
    CHECK(c.a == 0x3C);
    return 0;
}

static int test_movx_ext_ram(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xB8; e.rom[1] = 0x34;  /* MOV R0,#0x34 */
    e.rom[2] = 0x23; e.rom[3] = 0x77;  /* MOV A,#0x77 */
    e.rom[4] = 0x90;                   /* MOVX @R0,A */
    e.rom[5] = 0x27;                   /* CLR A */
    e.rom[6] = 0x80;                   /* MOVX A,@R0 */
    e.rom[7] = 0xB9; e.rom[8] = 0xC0;  /* MOV R1,#0xC0 (volle 8 bits!) */
    e.rom[9] = 0x91;                   /* MOVX @R1,A */
    tb_steps(&c, 3);
    CHECK(e.xram[0x34] == 0x77);
    tb_steps(&c, 2);
    CHECK(c.a == 0x77);
    tb_steps(&c, 2);
    CHECK(e.xram[0xC0] == 0x77);       /* MOVX-adres niet gemaskeerd */
    return 0;
}

static int test_movp_movp3_jmpp(void)
{
    tenv e; cpu8048 c;

    /* MOVP: pagina van de volgende instructie */
    tb_init(&e, &c);
    e.rom[0x24F] = 0xA3;               /* MOVP A,@A op 0x24F; next = 0x250 */
    e.rom[0x210] = 0xAB;
    c.pc = 0x24F;
    c.a = 0x10;
    cpu8048_step(&c);
    CHECK(c.a == 0xAB);                /* gelezen van 0x210 (pagina 2) */
    CHECK(c.pc == 0x250);

    /* MOVP3: altijd pagina 3 */
    tb_init(&e, &c);
    e.rom[0x050] = 0xE3;               /* MOVP3 A,@A */
    e.rom[0x342] = 0x99;
    c.pc = 0x050;
    c.a = 0x42;
    cpu8048_step(&c);
    CHECK(c.a == 0x99);

    /* JMPP: indirect binnen de eigen pagina */
    tb_init(&e, &c);
    e.rom[0x100] = 0xB3;               /* JMPP @A */
    e.rom[0x120] = 0x55;
    c.pc = 0x100;
    c.a = 0x20;
    cpu8048_step(&c);
    CHECK(c.pc == 0x155);
    return 0;
}

static int test_xchd_swap_logic(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xB8; e.rom[1] = 0x20;  /* MOV R0,#0x20 */
    e.rom[2] = 0x30;                   /* XCHD A,@R0 */
    e.rom[3] = 0x20;                   /* XCH A,@R0 */
    e.rom[4] = 0x37;                   /* CPL A */
    e.rom[5] = 0x43; e.rom[6] = 0x0F;  /* ORL A,#0x0F */
    e.rom[7] = 0x53; e.rom[8] = 0xF3;  /* ANL A,#0xF3 */
    e.rom[9] = 0xD3; e.rom[10] = 0xFF; /* XRL A,#0xFF */
    c.iram[0x20] = 0x9C;
    c.a = 0x5A;
    tb_steps(&c, 2);                   /* MOV R0 + XCHD */
    CHECK(c.a == 0x5C);                /* lage nibbles gewisseld */
    CHECK(c.iram[0x20] == 0x9A);
    cpu8048_step(&c);                  /* XCH */
    CHECK(c.a == 0x9A);
    CHECK(c.iram[0x20] == 0x5C);
    cpu8048_step(&c);                  /* CPL: 0x9A -> 0x65 */
    CHECK(c.a == 0x65);
    cpu8048_step(&c);                  /* ORL #0x0F -> 0x6F */
    CHECK(c.a == 0x6F);
    cpu8048_step(&c);                  /* ANL #0xF3 -> 0x63 */
    CHECK(c.a == 0x63);
    cpu8048_step(&c);                  /* XRL #0xFF -> 0x9C */
    CHECK(c.a == 0x9C);
    return 0;
}

static int test_djnz_loop_count(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xBA; e.rom[1] = 0x03;  /* MOV R2,#3 */
    e.rom[2] = 0x1B;                   /* loop: INC R3 */
    e.rom[3] = 0xEA; e.rom[4] = 0x02;  /* DJNZ R2,loop */
    e.rom[5] = 0x00;                   /* NOP */
    cpu8048_step(&c);                  /* MOV R2,#3 */
    /* 3 iteraties: INC R3 + DJNZ; laatste DJNZ valt door */
    tb_steps(&c, 6);
    CHECK(c.pc == 0x005);
    CHECK(c.iram[3] == 3);             /* R3 (RB0) drie keer verhoogd */
    CHECK(c.iram[2] == 0);             /* R2 op nul */
    CHECK(c.cycles == 2 + 3 * (1 + 2));
    return 0;
}

static int test_jz_jnz_jf0_jf1_jb(void)
{
    tenv e; cpu8048 c;

    tb_init(&e, &c);
    e.rom[0] = 0xC6; e.rom[1] = 0x10;  /* JZ 0x10 */
    c.a = 0;
    cpu8048_step(&c);
    CHECK(c.pc == 0x010);

    tb_init(&e, &c);
    e.rom[0] = 0x96; e.rom[1] = 0x10;  /* JNZ */
    c.a = 7;
    cpu8048_step(&c);
    CHECK(c.pc == 0x010);

    tb_init(&e, &c);
    e.rom[0] = 0x95;                   /* CPL F0 */
    e.rom[1] = 0xB6; e.rom[2] = 0x10;  /* JF0 */
    tb_steps(&c, 2);
    CHECK(c.pc == 0x010);

    tb_init(&e, &c);
    e.rom[0] = 0xB5;                   /* CPL F1 */
    e.rom[1] = 0x76; e.rom[2] = 0x10;  /* JF1 */
    tb_steps(&c, 2);
    CHECK(c.pc == 0x010);

    /* JB0 en JB7 */
    tb_init(&e, &c);
    e.rom[0] = 0x12; e.rom[1] = 0x10;  /* JB0 */
    c.a = 0x01;
    cpu8048_step(&c);
    CHECK(c.pc == 0x010);

    tb_init(&e, &c);
    e.rom[0] = 0xF2; e.rom[1] = 0x10;  /* JB7 */
    c.a = 0x80;
    cpu8048_step(&c);
    CHECK(c.pc == 0x010);

    tb_init(&e, &c);
    e.rom[0] = 0xF2; e.rom[1] = 0x10;  /* JB7, bit niet gezet */
    c.a = 0x7F;
    cpu8048_step(&c);
    CHECK(c.pc == 0x002);
    return 0;
}

/* ------------------------------------------------------------------ */
/* interrupt tijdens timer-overflow + EN I-volgorde                    */
/* ------------------------------------------------------------------ */

static int test_irq_after_en_i_next_boundary(void)
{
    tenv e; cpu8048 c;

    /* lijn al asserted vóór EN I: dispatch pas op de eerstvolgende
     * instructiegrens ná EN I */
    tb_init(&e, &c);
    e.rom[0] = 0x00;                   /* NOP */
    e.rom[1] = 0x05;                   /* EN I */
    e.rom[2] = 0x00;                   /* NOP (wordt onderbroken) */
    e.rom[3] = 0x93;                   /* vector 3: RETR */
    cpu8048_set_irq(&c, true);
    cpu8048_step(&c);                  /* NOP: interrupts nog uit */
    CHECK(c.pc == 0x001);
    cpu8048_step(&c);                  /* EN I voltooit eerst */
    CHECK(c.pc == 0x002);
    cpu8048_step(&c);                  /* nu pas de vector */
    CHECK(c.pc == 0x003);
    CHECK(c.iram[8] == 0x02);
    return 0;
}

/* ------------------------------------------------------------------ */
/* integratie via de publieke API (test-BIOS, geen echte ROM)          */
/* ------------------------------------------------------------------ */

static int test_api_bios_program_extram(void)
{
    g7k_sys *sys;
    uint8_t bios[1024];
    uint64_t cyc;
    int rc = 0;

    memset(bios, 0, sizeof(bios));
    /* synthetisch test-BIOS:
     *   MOV A,#0xAF ; OUTL P1,A   -> P16+P14 laag: write-strobe + externe
     *                                RAM aan (P13 hoog: VDC af)
     *   MOV R0,#0x12 ; MOV A,#0x5A ; MOVX @R0,A
     *   lus: JMP lus */
    bios[0x00] = 0x23; bios[0x01] = 0xAF;
    bios[0x02] = 0x39;
    bios[0x03] = 0xB8; bios[0x04] = 0x12;
    bios[0x05] = 0x23; bios[0x06] = 0x5A;
    bios[0x07] = 0x90;
    bios[0x08] = 0x04; bios[0x09] = 0x08;   /* JMP 0x008 */

    sys = g7k_create();
    if (!sys)
        return 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        rc = 1;
    if (!rc) {
        g7k_reset(sys, true);
        g7k_run_frame(sys);
        if (g7k_extram_peek(sys, 0x12) != 0x5A)
            rc = 1;
        /* PAL-framebudget: 313 lijnen x 76/3 = 7929 cycli (+ max 1
         * overshoot van een 2-cycle-instructie over de framegrens) */
        cyc = g7k_cycle_count(sys);
        if (cyc < 7929 || cyc > 7930)
            rc = 1;
    }
    g7k_destroy(sys);
    return rc;
}

static int test_api_warm_reset_keeps_iram(void)
{
    g7k_sys *sys;
    uint8_t bios[1024];
    int rc = 0;

    memset(bios, 0, sizeof(bios));
    /* MOV R0,#0x20 ; MOV @R0,#0x42 ; lus: JMP lus */
    bios[0x00] = 0xB8; bios[0x01] = 0x20;
    bios[0x02] = 0xB0; bios[0x03] = 0x42;
    bios[0x04] = 0x04; bios[0x05] = 0x04;

    sys = g7k_create();
    if (!sys)
        return 1;
    if (g7k_load_bios(sys, bios, sizeof(bios)) != 0)
        rc = 1;
    if (!rc) {
        g7k_reset(sys, true);
        g7k_run_frame(sys);
        if (g7k_intram_peek(sys, 0x20) != 0x42)
            rc = 1;
        g7k_reset(sys, false);         /* warm: iram blijft staan (S1) */
        if (g7k_intram_peek(sys, 0x20) != 0x42)
            rc = 1;
        g7k_reset(sys, true);          /* koud: iram gewist */
        if (g7k_intram_peek(sys, 0x20) != 0x00)
            rc = 1;
    }
    g7k_destroy(sys);
    return rc;
}

/* ------------------------------------------------------------------ */
/* registratie                                                         */
/* ------------------------------------------------------------------ */

void register_cpu_tests(void);

void register_cpu_tests(void)
{
    g7k_register_test("cpu_C1_cond_branch_page",        test_C1_cond_branch_page);
    g7k_register_test("cpu_C2_pc_wrap_07ff",            test_C2_pc_wrap_07ff);
    g7k_register_test("cpu_C3_daa_bcd",                 test_C3_daa_bcd);
    g7k_register_test("cpu_C4_jni",                     test_C4_jni);
    g7k_register_test("cpu_C5_ext_irq_vector_state",    test_C5_ext_irq_vector_state);
    g7k_register_test("cpu_C5_priority_ext_over_timer", test_C5_priority_ext_over_timer);
    g7k_register_test("cpu_C5_irq_suppresses_dbf",      test_C5_irq_suppresses_dbf);
    g7k_register_test("cpu_C5_irq_return_addr",         test_C5_irq_return_addr_before_instr);
    g7k_register_test("cpu_C6_cycle_timing",            test_C6_cycle_timing);
    g7k_register_test("cpu_C7_t1_conditions",           test_C7_t1_conditions);
    g7k_register_test("cpu_C8_reset_p1_ff",             test_C8_reset_p1_ff);
    g7k_register_test("cpu_C9_mb_bank_jmp_call",        test_C9_mb_bank_jmp_call);
    g7k_register_test("cpu_timer_prescaler_div32",      test_timer_prescaler_div32);
    g7k_register_test("cpu_timer_overflow_tf_jtf",      test_timer_overflow_tf_jtf);
    g7k_register_test("cpu_timer_irq_vector7",          test_timer_irq_vector7);
    g7k_register_test("cpu_timer_dis_tcnti_pending",    test_timer_dis_tcnti_clears_pending);
    g7k_register_test("cpu_counter_t1_falling_edge",    test_counter_t1_falling_edge);
    g7k_register_test("cpu_regbanks_rb0_rb1",           test_regbanks_rb0_rb1);
    g7k_register_test("cpu_flags_add_addc_ac_cy",       test_flags_add_addc_ac_cy);
    g7k_register_test("cpu_rotates_rrc_rlc",            test_rotates_rrc_rlc);
    g7k_register_test("cpu_stack_call_ret_nested",      test_stack_call_ret_nested);
    g7k_register_test("cpu_psw_mov_and_sp",             test_psw_mov_and_sp);
    g7k_register_test("cpu_ports_latch_anl_orl",        test_ports_latch_anl_orl);
    g7k_register_test("cpu_movx_ext_ram",               test_movx_ext_ram);
    g7k_register_test("cpu_movp_movp3_jmpp",            test_movp_movp3_jmpp);
    g7k_register_test("cpu_xchd_swap_logic",            test_xchd_swap_logic);
    g7k_register_test("cpu_djnz_loop_count",            test_djnz_loop_count);
    g7k_register_test("cpu_jz_jnz_jf0_jf1_jb",          test_jz_jnz_jf0_jf1_jb);
    g7k_register_test("cpu_irq_after_en_i_boundary",    test_irq_after_en_i_next_boundary);
    g7k_register_test("cpu_api_bios_program_extram",    test_api_bios_program_extram);
    g7k_register_test("cpu_api_warm_reset_keeps_iram",  test_api_warm_reset_keeps_iram);
}
