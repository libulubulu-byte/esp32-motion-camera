"""Mirror of the version-format guard in ota_check.c, so the accept/reject
table can actually be executed on this machine (no x86 C compiler is
installed - only the ESP32 cross toolchains, whose output cannot run here).

Mirrored line-by-line from ota_check_on_boot() and from app_version_parse()
in app_version.c. The C side is compile-checked with
    xtensa-esp32s3-elf-gcc -Wall -Wextra
via main/_verguard_test.c; this script covers the behaviour.
If you change the guard in ota_check.c, change both files.
"""
import re

fails = 0


def guard_ok(s):
    """ota_check.c: reject anything not  digits[.digits]..."""
    p, ok, fields = 0, len(s) > 0, 0
    while ok and p < len(s):
        if not s[p].isdigit():
            ok = False
            break
        while p < len(s) and s[p].isdigit():
            p += 1
        fields += 1
        if p < len(s) and s[p] == '.':
            if not (p + 1 < len(s) and s[p + 1].isdigit()):
                ok = False
                break
            p += 1
        elif p < len(s):
            ok = False
    return ok and fields <= 4


def parse(s):
    """app_version.c: app_version_parse - first three numeric fields."""
    mj = mi = pt = 0
    out = [mj, mi, pt]
    i, field = 0, 0
    while field < 3:
        while i < len(s) and not s[i].isdigit():
            if s[i] in '-+':
                return out[0], out[1], out[2]
            i += 1
        if i >= len(s):
            break
        v = 0
        while i < len(s) and s[i].isdigit():
            if v < 100000:
                v = v * 10 + int(s[i])
            i += 1
        out[field] = v
        if i >= len(s) or s[i] != '.':
            break
        i += 1
        field += 1
    return out[0], out[1], out[2]


def cmp_ver(a, b):
    a1, a2, a3 = parse(a)
    b1, b2, b3 = parse(b)
    for x, y in ((a1, b1), (a2, b2), (a3, b3)):
        if x != y:
            return 1 if x > y else -1
    return 0


def expect(fn, arg, want, why):
    global fails
    got = fn(arg)
    good = got == want
    if not good:
        fails += 1
    print(f"{'ok  ' if good else 'FAIL'} guard_ok({arg!r}) = {got!s:<5} "
          f"want {want!s:<5} ({why})")


print("=== must ACCEPT (real versions) ===")
expect(guard_ok, "0.1.1", True, "current")
expect(guard_ok, "0.1.2", True, "next")
expect(guard_ok, "1.2", True, "two fields")
expect(guard_ok, "3", True, "one field")
expect(guard_ok, "1.2.3.4", True, "four fields")
expect(guard_ok, "10.20.30", True, "multi-digit")

print("\n=== must REJECT (otherwise: update loop on every boot) ===")
expect(guard_ok, "676dc691-dirty", False, "git describe -> parses as 676 > 0.1.1")
expect(guard_ok, "0.1.1-rc1", False, "prerelease suffix")
expect(guard_ok, "v0.1.1", False, "leading v")
expect(guard_ok, "", False, "empty")
expect(guard_ok, "abc", False, "no digits")
expect(guard_ok, "0.1.1 ", False, "trailing space")
expect(guard_ok, "0..1", False, "double dot")
expect(guard_ok, "0.1.", False, "trailing dot")
expect(guard_ok, "0.1.1.2.3", False, "five fields")

print("\n=== the loop scenario, end to end ===")
dev = "0.1.1"
for srv in ("676dc691-dirty", "0.1.1", "0.1.2"):
    if not guard_ok(srv):
        print(f"  server={srv!r:<20} -> REJECTED by guard, no comparison, no update")
    else:
        print(f"  server={srv!r:<20} -> accepted, is_newer={cmp_ver(srv, dev) > 0}")

print("\n=== comparison sanity (app_version_cmp) ===")
for a, b, want in (("0.1.2", "0.1.1", 1), ("0.1.1", "0.1.1", 0),
                   ("0.1.1", "0.1.2", -1), ("0.2.0", "0.1.9", 1),
                   ("1.0.0", "0.9.9", 1), ("0.10.0", "0.9.0", 1)):
    got = cmp_ver(a, b)
    good = got == want
    if not good:
        fails += 1
    print(f"{'ok  ' if good else 'FAIL'} cmp({a},{b}) = {got} want {want}")

print(f"\n{'FAILED' if fails else 'ALL PASS'} ({fails} failure(s))")
raise SystemExit(1 if fails else 0)
