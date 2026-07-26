/*
 * cpu8048.c — Intel 8048 (MCS-48) CPU-kern, cycle-getrouw, 100% from scratch.
 *
 * Bron: uitsluitend Intel MCS-48 User's Manual / 8048-datasheetsemantiek
 * (gedragsfeiten). Geen code/tabellen uit O2EM, MAME of andere emulators.
 *
 * QUIRKS-dekking (docs/QUIRKS.md):
 *   C1  conditionele branches vervangen alleen PC[7:0]; de pagina is die van
 *       de EERSTVOLGENDE instructie (PC is al voorbij de 2-byte branch).
 *   C2  PC is een 11-bit teller: increment wrapt 0x7FF -> 0x000. A11 (bit 11)
 *       verandert NOOIT door increment, alleen door JMP/CALL (DBF) en
 *       RET/RETR (van de stack).
 *   C3  DA A conform datasheet: +6 op lage nibble bij >9 of AC, daarna +0x60
 *       op hoge nibble bij >9 of CY/overflow; CY wordt gezet bij overflow.
 *   C4  JNI: springt als de INT-pin laag is (irq_line asserted), ongeacht EN I.
 *   C5  Externe interrupt -> vector 0x003, timer -> 0x007; extern heeft
 *       prioriteit; push van PC+PSW[7:4]; tussen vector en RETR zijn verdere
 *       interrupts geblokkeerd en wordt DBF NIET naar A11 doorgelaten
 *       (a11_irq_hold); RETR herstelt PSW[7:4] en heft de blokkade op.
 *   C6  1- of 2-cycle-timing per instructie exact volgens de datasheet;
 *       interrupt-acknowledge kost 2 cycli.
 *   C7  T1 wordt per instructie gelezen via de bus-callback (JT1/JNT1) en in
 *       counter-mode per machinecyclus gesampled (1->0-flank telt).
 *   C8  RESET: P1=P2=BUS=0xFF; de latches worden via write_p1/write_p2 naar
 *       buiten gepompt zodat cart-bank 3 en VDC-selects consistent zijn.
 *   C9  DBF (SEL MB0/MB1) wordt pas bij JMP/CALL in A11 geladen; de rest van
 *       de PC-logica (increment, cond. branch) behoudt het lopende bit 11.
 *
 * Timer-detail (deterministische keuze binnen datasheet-marge): STRT T reset
 * de /32-prescaler; de prescaler tikt vanaf de cyclus van STRT T zelf, dus de
 * eerste timer-increment valt exact 32 machinecycli later. In counter-mode
 * telt de 1->0-flank op T1, gesampled elke machinecyclus. Overflow zet TF én
 * latcht een timer-interrupt-request; DIS TCNTI wist die request (datasheet).
 */
#include "core_internal.h"

/* PSW-layout: CY AC F0 BS 1 SP2 SP1 SP0 */
#define PSW_CY  0x80u
#define PSW_AC  0x40u
#define PSW_F0  0x20u
#define PSW_BS  0x10u
#define PSW_ONE 0x08u
#define PSW_SP  0x07u

/* ------------------------------------------------------------------ */
/* registers / geheugen-helpers                                        */
/* ------------------------------------------------------------------ */

static uint8_t reg_base(const cpu8048 *cpu)
{
    return (cpu->psw & PSW_BS) ? 24 : 0;   /* RB1 = iram 24..31 */
}

static uint8_t reg_get(const cpu8048 *cpu, uint8_t r)
{
    return cpu->iram[reg_base(cpu) + r];
}

static void reg_set(cpu8048 *cpu, uint8_t r, uint8_t v)
{
    cpu->iram[reg_base(cpu) + r] = v;
}

/* @R0/@R1: indirect adres in de 64B interne RAM (8048) */
static uint8_t ind_addr(const cpu8048 *cpu, uint8_t r)
{
    return (uint8_t)(reg_get(cpu, r) & 0x3F);
}

/* programma-fetch met 11-bit increment; bit 11 blijft staan (C2/C9) */
static uint8_t fetch(cpu8048 *cpu)
{
    uint8_t v = cpu->bus.read_rom(cpu->bus.ctx, (uint16_t)(cpu->pc & 0x0FFF));
    cpu->pc = (uint16_t)((cpu->pc & 0x0800) | ((cpu->pc + 1u) & 0x07FF));
    return v;
}

/* ------------------------------------------------------------------ */
/* stack (iram 8..23, 8 niveaus x 2 bytes)                             */
/* ------------------------------------------------------------------ */

static void stack_push(cpu8048 *cpu)
{
    uint8_t sp = cpu->psw & PSW_SP;
    cpu->iram[8 + 2 * sp]     = (uint8_t)(cpu->pc & 0xFF);
    cpu->iram[8 + 2 * sp + 1] = (uint8_t)(((cpu->pc >> 8) & 0x0F) |
                                          (cpu->psw & 0xF0));
    cpu->psw = (uint8_t)((cpu->psw & 0xF8) | ((sp + 1u) & PSW_SP));
}

static void stack_pop(cpu8048 *cpu, bool restore_psw)
{
    uint8_t sp = (uint8_t)(((cpu->psw & PSW_SP) - 1u) & PSW_SP);
    uint8_t lo = cpu->iram[8 + 2 * sp];
    uint8_t hi = cpu->iram[8 + 2 * sp + 1];
    cpu->pc = (uint16_t)(((uint16_t)(hi & 0x0F) << 8) | lo);
    if (restore_psw)
        cpu->psw = (uint8_t)((hi & 0xF0) | PSW_ONE | sp);
    else
        cpu->psw = (uint8_t)((cpu->psw & 0xF8) | sp);
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

static void alu_add(cpu8048 *cpu, uint8_t v, bool use_carry)
{
    unsigned c = (use_carry && (cpu->psw & PSW_CY)) ? 1u : 0u;
    unsigned r = (unsigned)cpu->a + v + c;
    unsigned h = (unsigned)(cpu->a & 0x0F) + (unsigned)(v & 0x0F) + c;
    cpu->psw &= (uint8_t)~(PSW_CY | PSW_AC);
    if (r > 0xFF)
        cpu->psw |= PSW_CY;
    if (h > 0x0F)
        cpu->psw |= PSW_AC;
    cpu->a = (uint8_t)r;
}

static void alu_daa(cpu8048 *cpu)                              /* C3 */
{
    unsigned t = cpu->a;
    if ((t & 0x0F) > 9 || (cpu->psw & PSW_AC))
        t += 0x06;
    if (((t >> 4) & 0x0F) > 9 || (cpu->psw & PSW_CY) || t > 0xFF)
        t += 0x60;
    if (t > 0xFF)
        cpu->psw |= PSW_CY;
    cpu->a = (uint8_t)t;
}

/* ------------------------------------------------------------------ */
/* timer/counter                                                       */
/* ------------------------------------------------------------------ */

static void timer_increment(cpu8048 *cpu)
{
    cpu->timer = (uint8_t)(cpu->timer + 1u);
    if (cpu->timer == 0) {
        cpu->tf = true;
        cpu->timer_irq_pending = true;
    }
}

/* n machinecycli laten verstrijken: cyclusteller + timer/counter-klok.
 * In counter-mode wordt T1 elke cyclus gesampled (flank 1->0 telt, C7). */
static void tick(cpu8048 *cpu, int n)
{
    for (int i = 0; i < n; i++) {
        cpu->cycles++;
        if (!cpu->timer_running)
            continue;
        if (cpu->counter_mode) {
            bool t1 = cpu->bus.read_t1 ? cpu->bus.read_t1(cpu->bus.ctx)
                                       : false;
            if (cpu->last_t1 && !t1)
                timer_increment(cpu);
            cpu->last_t1 = t1;
        } else {
            cpu->prescaler = (uint8_t)((cpu->prescaler + 1u) & 0x1F);
            if (cpu->prescaler == 0)
                timer_increment(cpu);
        }
    }
}

/* ------------------------------------------------------------------ */
/* interrupts (C5)                                                     */
/* ------------------------------------------------------------------ */

static bool try_interrupt(cpu8048 *cpu)
{
    uint16_t vec;

    if (cpu->in_irq)
        return false;                  /* geblokkeerd tot RETR */
    if (cpu->irq_line && cpu->irq_enabled) {
        vec = 0x003;                   /* extern heeft prioriteit */
    } else if (cpu->timer_irq_pending && cpu->tcnti_enabled) {
        vec = 0x007;
        cpu->timer_irq_pending = false;
    } else {
        return false;
    }
    stack_push(cpu);                   /* als CALL: PC + PSW[7:4] */
    cpu->pc = vec;
    cpu->in_irq = true;
    cpu->a11_irq_hold = true;          /* DBF -> A11 onderdrukt (C5/C9) */
    tick(cpu, 2);                      /* acknowledge kost 2 cycli (C6) */
    return true;
}

/* ------------------------------------------------------------------ */
/* sprong-helpers                                                      */
/* ------------------------------------------------------------------ */

/* conditionele branch: 2 bytes, doel = pagina van de VOLGENDE instructie
 * (PC staat na de operand-fetch al op die instructie) + operand (C1).
 * (pc & 0x0F00) behoudt ook A11, dus branches in de bovenste 2K blijven
 * in de bovenste 2K (C9). */
static int jcond(cpu8048 *cpu, bool cond)
{
    uint8_t off = fetch(cpu);
    if (cond)
        cpu->pc = (uint16_t)((cpu->pc & 0x0F00) | off);
    return 2;
}

static uint16_t a11_effective(const cpu8048 *cpu)
{
    return (cpu->dbf && !cpu->a11_irq_hold) ? 0x0800u : 0x0000u;
}

/* ------------------------------------------------------------------ */
/* poort-helpers                                                       */
/* ------------------------------------------------------------------ */

static void p1_out(cpu8048 *cpu, uint8_t v)
{
    cpu->p1_latch = v;
    if (cpu->bus.write_p1)
        cpu->bus.write_p1(cpu->bus.ctx, v);
}

static void p2_out(cpu8048 *cpu, uint8_t v)
{
    cpu->p2_latch = v;
    if (cpu->bus.write_p2)
        cpu->bus.write_p2(cpu->bus.ctx, v);
}

static void bus_out(cpu8048 *cpu, uint8_t v)
{
    cpu->bus_latch = v;
    if (cpu->bus.write_bus)
        cpu->bus.write_bus(cpu->bus.ctx, v);
}

/* ------------------------------------------------------------------ */
/* instructie-executie; return = machinecycli (1 of 2, C6)             */
/* ------------------------------------------------------------------ */

static int exec(cpu8048 *cpu, uint8_t op)
{
    const uint8_t r = (uint8_t)(op & 0x07);

    /* --- register-families (laagste 3 bits = Rr) --- */
    switch (op & 0xF8) {
    case 0x18:                                    /* INC Rr        1c */
        reg_set(cpu, r, (uint8_t)(reg_get(cpu, r) + 1u));
        return 1;
    case 0x28: {                                  /* XCH A,Rr      1c */
        uint8_t t = cpu->a;
        cpu->a = reg_get(cpu, r);
        reg_set(cpu, r, t);
        return 1;
    }
    case 0x48:                                    /* ORL A,Rr      1c */
        cpu->a |= reg_get(cpu, r);
        return 1;
    case 0x58:                                    /* ANL A,Rr      1c */
        cpu->a &= reg_get(cpu, r);
        return 1;
    case 0x68:                                    /* ADD A,Rr      1c */
        alu_add(cpu, reg_get(cpu, r), false);
        return 1;
    case 0x78:                                    /* ADDC A,Rr     1c */
        alu_add(cpu, reg_get(cpu, r), true);
        return 1;
    case 0xA8:                                    /* MOV Rr,A      1c */
        reg_set(cpu, r, cpu->a);
        return 1;
    case 0xB8:                                    /* MOV Rr,#imm   2c */
        reg_set(cpu, r, fetch(cpu));
        return 2;
    case 0xC8:                                    /* DEC Rr        1c */
        reg_set(cpu, r, (uint8_t)(reg_get(cpu, r) - 1u));
        return 1;
    case 0xD8:                                    /* XRL A,Rr      1c */
        cpu->a ^= reg_get(cpu, r);
        return 1;
    case 0xE8: {                                  /* DJNZ Rr,addr  2c */
        uint8_t v = (uint8_t)(reg_get(cpu, r) - 1u);
        reg_set(cpu, r, v);
        return jcond(cpu, v != 0);
    }
    case 0xF8:                                    /* MOV A,Rr      1c */
        cpu->a = reg_get(cpu, r);
        return 1;
    default:
        break;
    }

    /* --- JMP / CALL / JBb: hoogste 3 bits = pagina resp. bitnummer --- */
    switch (op & 0x1F) {
    case 0x04: {                                  /* JMP addr      2c */
        uint8_t lo = fetch(cpu);
        cpu->pc = (uint16_t)(a11_effective(cpu) |
                             ((uint16_t)(op >> 5) << 8) | lo);  /* C9 */
        return 2;
    }
    case 0x14: {                                  /* CALL addr     2c */
        uint8_t lo = fetch(cpu);
        stack_push(cpu);
        cpu->pc = (uint16_t)(a11_effective(cpu) |
                             ((uint16_t)(op >> 5) << 8) | lo);  /* C9 */
        return 2;
    }
    case 0x12:                                    /* JBb addr      2c */
        return jcond(cpu, (cpu->a & (uint8_t)(1u << (op >> 5))) != 0);
    default:
        break;
    }

    /* --- overige opcodes --- */
    switch (op) {
    case 0x00:                                    /* NOP           1c */
        return 1;

    /* accumulator */
    case 0x07: cpu->a = (uint8_t)(cpu->a - 1u); return 1;   /* DEC A  */
    case 0x17: cpu->a = (uint8_t)(cpu->a + 1u); return 1;   /* INC A  */
    case 0x27: cpu->a = 0;                      return 1;   /* CLR A  */
    case 0x37: cpu->a = (uint8_t)~cpu->a;       return 1;   /* CPL A  */
    case 0x47:                                              /* SWAP A */
        cpu->a = (uint8_t)((cpu->a >> 4) | (cpu->a << 4));
        return 1;
    case 0x57: alu_daa(cpu);                    return 1;   /* DA A (C3) */
    case 0x67: {                                            /* RRC A  */
        uint8_t oldcy = (cpu->psw & PSW_CY) ? 0x80u : 0u;
        uint8_t newcy = (uint8_t)(cpu->a & 0x01);
        cpu->a = (uint8_t)((cpu->a >> 1) | oldcy);
        cpu->psw = (uint8_t)((cpu->psw & ~PSW_CY) | (newcy ? PSW_CY : 0));
        return 1;
    }
    case 0x77:                                              /* RR A   */
        cpu->a = (uint8_t)((cpu->a >> 1) | (cpu->a << 7));
        return 1;
    case 0xE7:                                              /* RL A   */
        cpu->a = (uint8_t)((cpu->a << 1) | (cpu->a >> 7));
        return 1;
    case 0xF7: {                                            /* RLC A  */
        uint8_t oldcy = (cpu->psw & PSW_CY) ? 0x01u : 0u;
        uint8_t newcy = (uint8_t)(cpu->a & 0x80);
        cpu->a = (uint8_t)((cpu->a << 1) | oldcy);
        cpu->psw = (uint8_t)((cpu->psw & ~PSW_CY) | (newcy ? PSW_CY : 0));
        return 1;
    }

    /* immediate ALU (2 bytes, 2 cycli) */
    case 0x03: alu_add(cpu, fetch(cpu), false); return 2;   /* ADD A,#  */
    case 0x13: alu_add(cpu, fetch(cpu), true);  return 2;   /* ADDC A,# */
    case 0x23: cpu->a  = fetch(cpu);            return 2;   /* MOV A,#  */
    case 0x43: cpu->a |= fetch(cpu);            return 2;   /* ORL A,#  */
    case 0x53: cpu->a &= fetch(cpu);            return 2;   /* ANL A,#  */
    case 0xD3: cpu->a ^= fetch(cpu);            return 2;   /* XRL A,#  */

    /* indirect via @R0/@R1 */
    case 0x10: case 0x11: {                                 /* INC @Ri  */
        uint8_t ad = ind_addr(cpu, (uint8_t)(op & 1));
        cpu->iram[ad] = (uint8_t)(cpu->iram[ad] + 1u);
        return 1;
    }
    case 0x20: case 0x21: {                                 /* XCH A,@Ri */
        uint8_t ad = ind_addr(cpu, (uint8_t)(op & 1));
        uint8_t t = cpu->a;
        cpu->a = cpu->iram[ad];
        cpu->iram[ad] = t;
        return 1;
    }
    case 0x30: case 0x31: {                                 /* XCHD A,@Ri */
        uint8_t ad = ind_addr(cpu, (uint8_t)(op & 1));
        uint8_t t = (uint8_t)(cpu->a & 0x0F);
        cpu->a = (uint8_t)((cpu->a & 0xF0) | (cpu->iram[ad] & 0x0F));
        cpu->iram[ad] = (uint8_t)((cpu->iram[ad] & 0xF0) | t);
        return 1;
    }
    case 0x40: case 0x41:                                   /* ORL A,@Ri */
        cpu->a |= cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))];
        return 1;
    case 0x50: case 0x51:                                   /* ANL A,@Ri */
        cpu->a &= cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))];
        return 1;
    case 0x60: case 0x61:                                   /* ADD A,@Ri */
        alu_add(cpu, cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))], false);
        return 1;
    case 0x70: case 0x71:                                   /* ADDC A,@Ri */
        alu_add(cpu, cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))], true);
        return 1;
    case 0xA0: case 0xA1:                                   /* MOV @Ri,A */
        cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))] = cpu->a;
        return 1;
    case 0xB0: case 0xB1:                                   /* MOV @Ri,# */
        cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))] = fetch(cpu);
        return 2;
    case 0xD0: case 0xD1:                                   /* XRL A,@Ri */
        cpu->a ^= cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))];
        return 1;
    case 0xF0: case 0xF1:                                   /* MOV A,@Ri */
        cpu->a = cpu->iram[ind_addr(cpu, (uint8_t)(op & 1))];
        return 1;

    /* MOVX — externe dataruimte (2 cycli) */
    case 0x80: case 0x81:                                   /* MOVX A,@Ri */
        cpu->a = cpu->bus.read_ext
                     ? cpu->bus.read_ext(cpu->bus.ctx,
                                         reg_get(cpu, (uint8_t)(op & 1)))
                     : 0xFF;
        return 2;
    case 0x90: case 0x91:                                   /* MOVX @Ri,A */
        if (cpu->bus.write_ext)
            cpu->bus.write_ext(cpu->bus.ctx,
                               reg_get(cpu, (uint8_t)(op & 1)), cpu->a);
        return 2;

    /* programmageheugen-leesinstructies (2 cycli) */
    case 0xA3:                                              /* MOVP A,@A  */
        /* pagina = die van de volgende instructie (PC al geincrementeerd) */
        cpu->a = cpu->bus.read_rom(cpu->bus.ctx,
                                   (uint16_t)((cpu->pc & 0x0F00) | cpu->a));
        return 2;
    case 0xE3:                                              /* MOVP3 A,@A */
        cpu->a = cpu->bus.read_rom(cpu->bus.ctx,
                                   (uint16_t)(0x300u | cpu->a));
        return 2;
    case 0xB3: {                                            /* JMPP @A    */
        uint16_t page = (uint16_t)(cpu->pc & 0x0F00);
        cpu->pc = (uint16_t)(page |
                             cpu->bus.read_rom(cpu->bus.ctx,
                                               (uint16_t)(page | cpu->a)));
        return 2;
    }

    /* subroutine-returns (2 cycli) */
    case 0x83:                                              /* RET   */
        stack_pop(cpu, false);
        return 2;
    case 0x93:                                              /* RETR (C5) */
        stack_pop(cpu, true);
        cpu->in_irq = false;
        cpu->a11_irq_hold = false;
        return 2;

    /* conditionele sprongen (2 bytes, 2 cycli; C1) */
    case 0x16: {                                            /* JTF   */
        bool c = cpu->tf;
        cpu->tf = false;                    /* JTF wist TF (datasheet) */
        return jcond(cpu, c);
    }
    case 0x26:                                              /* JNT0  */
        return jcond(cpu, !(cpu->bus.read_t0 &&
                            cpu->bus.read_t0(cpu->bus.ctx)));
    case 0x36:                                              /* JT0   */
        return jcond(cpu, cpu->bus.read_t0 &&
                          cpu->bus.read_t0(cpu->bus.ctx));
    case 0x46:                                              /* JNT1  */
        return jcond(cpu, !(cpu->bus.read_t1 &&
                            cpu->bus.read_t1(cpu->bus.ctx)));
    case 0x56:                                              /* JT1 (C7) */
        return jcond(cpu, cpu->bus.read_t1 &&
                          cpu->bus.read_t1(cpu->bus.ctx));
    case 0x76:                                              /* JF1   */
        return jcond(cpu, cpu->f1);
    case 0x86:                                              /* JNI (C4) */
        return jcond(cpu, cpu->irq_line);   /* INT laag = asserted */
    case 0x96:                                              /* JNZ   */
        return jcond(cpu, cpu->a != 0);
    case 0xB6:                                              /* JF0   */
        return jcond(cpu, (cpu->psw & PSW_F0) != 0);
    case 0xC6:                                              /* JZ    */
        return jcond(cpu, cpu->a == 0);
    case 0xE6:                                              /* JNC   */
        return jcond(cpu, !(cpu->psw & PSW_CY));
    case 0xF6:                                              /* JC    */
        return jcond(cpu, (cpu->psw & PSW_CY) != 0);

    /* vlaggen */
    case 0x85: cpu->psw &= (uint8_t)~PSW_F0; return 1;      /* CLR F0 */
    case 0x95: cpu->psw ^= PSW_F0;           return 1;      /* CPL F0 */
    case 0xA5: cpu->f1 = false;              return 1;      /* CLR F1 */
    case 0xB5: cpu->f1 = !cpu->f1;           return 1;      /* CPL F1 */
    case 0x97: cpu->psw &= (uint8_t)~PSW_CY; return 1;      /* CLR C  */
    case 0xA7: cpu->psw ^= PSW_CY;           return 1;      /* CPL C  */

    /* PSW */
    case 0xC7:                                              /* MOV A,PSW */
        cpu->a = (uint8_t)(cpu->psw | PSW_ONE);  /* bit 3 leest altijd 1 */
        return 1;
    case 0xD7:                                              /* MOV PSW,A */
        cpu->psw = (uint8_t)(cpu->a | PSW_ONE);
        return 1;

    /* interrupt-enables */
    case 0x05: cpu->irq_enabled = true;   return 1;         /* EN I      */
    case 0x15: cpu->irq_enabled = false;  return 1;         /* DIS I     */
    case 0x25: cpu->tcnti_enabled = true; return 1;         /* EN TCNTI  */
    case 0x35:                                              /* DIS TCNTI */
        cpu->tcnti_enabled = false;
        cpu->timer_irq_pending = false;   /* wist gelatchte request */
        return 1;

    /* timer/counter */
    case 0x42: cpu->a = cpu->timer;       return 1;         /* MOV A,T   */
    case 0x62: cpu->timer = cpu->a;       return 1;         /* MOV T,A   */
    case 0x45:                                              /* STRT CNT  */
        cpu->counter_mode = true;
        cpu->timer_running = true;
        cpu->last_t1 = cpu->bus.read_t1 ? cpu->bus.read_t1(cpu->bus.ctx)
                                        : false;
        return 1;
    case 0x55:                                              /* STRT T    */
        cpu->counter_mode = false;
        cpu->timer_running = true;
        cpu->prescaler = 0;               /* deterministisch: reset /32 */
        return 1;
    case 0x65: cpu->timer_running = false; return 1;        /* STOP TCNT */
    case 0x75: return 1;    /* ENT0 CLK: klok-uit op T0, hier geen effect */

    /* registerbank / geheugenbank */
    case 0xC5: cpu->psw &= (uint8_t)~PSW_BS; return 1;      /* SEL RB0 */
    case 0xD5: cpu->psw |= PSW_BS;           return 1;      /* SEL RB1 */
    case 0xE5: cpu->dbf = false;             return 1;      /* SEL MB0 (C9) */
    case 0xF5: cpu->dbf = true;              return 1;      /* SEL MB1 (C9) */

    /* poorten (alle 2 cycli) */
    case 0x02: bus_out(cpu, cpu->a);                       return 2; /* OUTL BUS,A */
    case 0x08:                                                       /* INS A,BUS  */
        cpu->a = cpu->bus.read_bus ? cpu->bus.read_bus(cpu->bus.ctx)
                                   : cpu->bus_latch;
        return 2;
    case 0x09:                                                       /* IN A,P1 */
        cpu->a = cpu->bus.read_p1
                     ? cpu->bus.read_p1(cpu->bus.ctx, cpu->p1_latch)
                     : cpu->p1_latch;
        return 2;
    case 0x0A:                                                       /* IN A,P2 */
        cpu->a = cpu->bus.read_p2
                     ? cpu->bus.read_p2(cpu->bus.ctx, cpu->p2_latch)
                     : cpu->p2_latch;
        return 2;
    case 0x39: p1_out(cpu, cpu->a);                        return 2; /* OUTL P1,A */
    case 0x3A: p2_out(cpu, cpu->a);                        return 2; /* OUTL P2,A */
    case 0x88: bus_out(cpu, (uint8_t)(cpu->bus_latch | fetch(cpu)));
        return 2;                                                    /* ORL BUS,# */
    case 0x89: p1_out(cpu, (uint8_t)(cpu->p1_latch | fetch(cpu)));
        return 2;                                                    /* ORL P1,#  */
    case 0x8A: p2_out(cpu, (uint8_t)(cpu->p2_latch | fetch(cpu)));
        return 2;                                                    /* ORL P2,#  */
    case 0x98: bus_out(cpu, (uint8_t)(cpu->bus_latch & fetch(cpu)));
        return 2;                                                    /* ANL BUS,# */
    case 0x99: p1_out(cpu, (uint8_t)(cpu->p1_latch & fetch(cpu)));
        return 2;                                                    /* ANL P1,#  */
    case 0x9A: p2_out(cpu, (uint8_t)(cpu->p2_latch & fetch(cpu)));
        return 2;                                                    /* ANL P2,#  */

    /* 8243-expanderpoorten P4-P7: niet aanwezig op de G7000/O2.
     * MOVD A,Pp leest een zwevende 4-bit bus -> 0x0F in de lage nibble,
     * hoge nibble 0 (datasheet: bovenste nibble wordt gewist). Writes
     * verdwijnen in het niets. Alle 2 cycli. */
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:             /* MOVD A,Pp */
        cpu->a = 0x0F;
        return 2;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:             /* MOVD Pp,A */
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:             /* ORLD Pp,A */
    case 0x9C: case 0x9D: case 0x9E: case 0x9F:             /* ANLD Pp,A */
        return 2;

    default:
        /* niet-gedefinieerde opcodes gedragen zich als NOP (1 cyclus) */
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* publieke (interne) API — zie core_internal.h                        */
/* ------------------------------------------------------------------ */

void cpu8048_init(cpu8048 *cpu, const cpu8048_bus *bus)
{
    cpu8048 zero = {0};
    *cpu = zero;                       /* koude start: alles nul */
    cpu->psw = PSW_ONE;                /* bit 3 leest altijd 1 */
    cpu->bus = *bus;
}

void cpu8048_reset(cpu8048 *cpu)
{
    cpu->pc = 0;                       /* reset-vector 0x000 */
    cpu->psw = PSW_ONE;                /* CY/AC/F0/BS/SP = 0 */
    cpu->f1 = false;
    cpu->dbf = false;                  /* MB0 */
    cpu->a11_irq_hold = false;
    cpu->in_irq = false;
    cpu->irq_enabled = false;
    cpu->tcnti_enabled = false;
    cpu->timer_running = false;
    cpu->counter_mode = false;
    cpu->prescaler = 0;
    cpu->last_t1 = false;
    cpu->tf = false;
    cpu->timer_irq_pending = false;
    /* C8: poortlatches 0xFF; doorgepompt zodat cart-bank 3 (M3) en
     * VDC-selects consistent zijn. iram en cycles blijven staan (S1:
     * koud wissen is beleid van g7k_reset in sys.c). */
    cpu->p1_latch = 0xFF;
    cpu->p2_latch = 0xFF;
    cpu->bus_latch = 0xFF;
    if (cpu->bus.write_p1)
        cpu->bus.write_p1(cpu->bus.ctx, 0xFF);
    if (cpu->bus.write_p2)
        cpu->bus.write_p2(cpu->bus.ctx, 0xFF);
}

int cpu8048_step(cpu8048 *cpu)
{
    int cyc;

    if (try_interrupt(cpu))            /* dispatch VOOR de fetch (C5) */
        return 2;

    cyc = exec(cpu, fetch(cpu));
    tick(cpu, cyc);
    return cyc;
}

void cpu8048_set_irq(cpu8048 *cpu, bool asserted)
{
    cpu->irq_line = asserted;          /* level-triggered (C5) */
}
