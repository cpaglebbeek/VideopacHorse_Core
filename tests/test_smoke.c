/* test_smoke.c — skeleton-smoke: API linkt en levenscyclus werkt. */
#include <stddef.h>
#include "../include/g7000.h"

void g7k_register_test(const char *name, int (*fn)(void));

static int test_version_string(void)
{
    return g7k_version() != NULL ? 0 : 1;
}

static int test_create_destroy(void)
{
    g7k_sys *sys = g7k_create();
    if (!sys) return 1;
    g7k_destroy(sys);
    return 0;
}

void register_smoke_tests(void)
{
    g7k_register_test("smoke_version_string", test_version_string);
    g7k_register_test("smoke_create_destroy", test_create_destroy);
}
