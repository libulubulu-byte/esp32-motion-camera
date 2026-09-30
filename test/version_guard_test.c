/* Standalone check of the version-format guard in ota_check.c.
 * Compiled with the host compiler, no IDF needed.
 *   gcc -I../main version_guard_test.c ../main/app_version.c -o t && ./t
 * (this machine has no x86 gcc - only ESP32 cross toolchains, which cannot
 *  run here; use version_guard_test.py to actually execute the cases)
 * The guard body below is copy-pasted verbatim from ota_check_on_boot();
 * if you change one, change both (it is only 20 lines and keeps this test
 * free of IDF headers). */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_version.h"

static bool guard_ok(const char *srv_ver)
{
    const char *p = srv_ver;
    bool ok = (*p != '\0');
    int fields = 0;
    while (ok && *p) {
        if (!isdigit((unsigned char)*p)) {
            ok = false;
            break;
        }
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        fields++;
        if (*p == '.') {
            if (!isdigit((unsigned char)p[1])) {
                ok = false;
                break;
            }
            p++;
        } else if (*p != '\0') {
            ok = false;
        }
    }
    return ok && fields <= 4;
}

static int fails = 0;

static void expect(const char *v, bool want, const char *why)
{
    bool got = guard_ok(v);
    const char *mark = (got == want) ? "ok  " : "FAIL";
    if (got != want) {
        fails++;
    }
    printf("%s guard(\"%s\") = %s   want %s   (%s)\n",
           mark, v, got ? "accept" : "reject", want ? "accept" : "reject", why);
}

int main(void)
{
    puts("--- must ACCEPT (real versions) ---");
    expect("0.1.1", true, "current");
    expect("0.1.2", true, "next");
    expect("1.2", true, "two fields");
    expect("3", true, "one field");
    expect("1.2.3.4", true, "four fields");
    expect("10.20.30", true, "multi-digit");

    puts("--- must REJECT (would cause an update loop) ---");
    expect("676dc691-dirty", false, "git describe: parses as 676 > 0.1.1");
    expect("0.1.1-rc1", false, "prerelease suffix");
    expect("v0.1.1", false, "leading v");
    expect("", false, "empty");
    expect("abc", false, "no digits");
    expect("0.1.1 ", false, "trailing space");
    expect("0..1", false, "double dot");
    expect("0.1.", false, "trailing dot");
    expect("0.1.1.2.3", false, "five fields");

    puts("--- comparison sanity (app_version.c) ---");
    struct { const char *a, *b; int want; } cs[] = {
        {"0.1.2", "0.1.1",  1},
        {"0.1.1", "0.1.1",  0},
        {"0.1.1", "0.1.2", -1},
        {"0.2.0", "0.1.9",  1},
        {"1.0.0", "0.9.9",  1},
        {"0.10.0", "0.9.0", 1},   /* numeric, not lexicographic */
    };
    for (int i = 0; i < (int)(sizeof(cs) / sizeof(cs[0])); i++) {
        int got = app_version_cmp(cs[i].a, cs[i].b);
        if (got != cs[i].want) {
            fails++;
            printf("FAIL cmp(%s,%s) = %d want %d\n", cs[i].a, cs[i].b, got, cs[i].want);
        } else {
            printf("ok   cmp(%s,%s) = %d\n", cs[i].a, cs[i].b, got);
        }
    }

    printf("\n%s (%d failure(s))\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails != 0;
}
