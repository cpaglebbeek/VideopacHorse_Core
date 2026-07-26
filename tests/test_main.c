/*
 * test_main.c — minimale testharness VideopacHorse_Core
 *
 * Conventie: elke quirk uit docs/QUIRKS.md krijgt een test met het QUIRKS-nummer
 * in de naam (bv. test_C2_pc_wrap_07ff). Registratie via REGISTER_TEST in de
 * per-onderwerp testbestanden; de harness draait alles en rapporteert per test.
 * BIOS-loos: tests injecteren eigen 8048-programma's als byte-arrays.
 * Tests die echte ROMs nodig hebben lezen G7K_BIOS/G7K_ROMDIR en SKIPPEN stil
 * als die env-variabelen ontbreken.
 */
#include <stdio.h>
#include <string.h>

typedef int (*g7k_test_fn)(void); /* 0=pass, 1=fail, 2=skip */

typedef struct {
    const char *name;
    g7k_test_fn fn;
} g7k_test;

#define G7K_MAX_TESTS 512
static g7k_test g_tests[G7K_MAX_TESTS];
static int g_test_count = 0;

void g7k_register_test(const char *name, g7k_test_fn fn)
{
    if (g_test_count < G7K_MAX_TESTS) {
        g_tests[g_test_count].name = name;
        g_tests[g_test_count].fn = fn;
        g_test_count++;
    }
}

/* Elke test-unit levert een registratiefunctie; hier aanroepen. */
void register_smoke_tests(void);
void register_cpu_tests(void);
void register_vdc_tests(void);
void register_cart_tests(void);
void register_state_tests(void);
void register_sys_tests(void);

int main(void)
{
    int pass = 0, fail = 0, skip = 0;

    register_smoke_tests();
    register_cpu_tests();
    register_vdc_tests();
    register_cart_tests();
    register_state_tests();
    register_sys_tests();

    for (int i = 0; i < g_test_count; i++) {
        int r = g_tests[i].fn();
        const char *tag = r == 0 ? "PASS" : (r == 2 ? "SKIP" : "FAIL");
        if (r == 0) pass++; else if (r == 2) skip++; else fail++;
        printf("[%s] %s\n", tag, g_tests[i].name);
    }
    printf("\n%d tests: %d pass, %d fail, %d skip\n", g_test_count, pass, fail, skip);
    return fail ? 1 : 0;
}
