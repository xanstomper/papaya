/* wsprintfA guest boot test: exercises the Papaya USER32 wsprintfA HLE
 * against the documented wsprintfA format spec (winuser.h), including
 * >2 varargs (guest stack args) and size prefixes. Exit 0 = all pass. */
#include <windows.h>
#include <string.h>
#include <stdio.h>

static int fails = 0;
static void check(const char* name, const char* got, const char* want) {
    if (strcmp(got, want) != 0) { printf("FAIL %s: got '%s' want '%s'\n", name, got, want); fails++; }
    else printf("ok %-9s -> '%s'\n", name, got);
}
int main(void) {
    char buf[128]; int r;
    r = wsprintfA(buf, "%d", -42);             check("d", buf, "-42");
    if (r != 3) { printf("FAIL ret d = %d\n", r); fails++; }
    wsprintfA(buf, "%s=%u", "n", 7u);          check("s-u", buf, "n=7");
    wsprintfA(buf, "%05d", 123);               check("05d", buf, "00123");
    wsprintfA(buf, "%x %X", 0xABC, 0xABC);     check("x-X", buf, "abc ABC");
    wsprintfA(buf, "%c%c", 'o', 'k');          check("c", buf, "ok");
    wsprintfA(buf, "%ld%%", 100L);             check("ld-pct", buf, "100%");
    wsprintfA(buf, "%-5s|", "ab");             check("-5s", buf, "ab   |");
    wsprintfA(buf, "%5d|", 42);                check("5d", buf, "   42|");
    wsprintfA(buf, "hello %d %s %c", 1, "two", '3'); check("mixed3", buf, "hello 1 two 3");
    wsprintfA(buf, "%d-%d-%d-%d-%d", 1, 2, 3, 4, 5); check("stack5", buf, "1-2-3-4-5");
    wsprintfA(buf, "%s|%s|%s|%s", "a", "b", "c", "d"); check("stack4s", buf, "a|b|c|d");
    wsprintfA(buf, "%lu", 3000000000u);        check("lu", buf, "3000000000");
    wsprintfA(buf, "%lx %lX", 0xABCDEF01u, 0xABCDEF01u); check("lx-lX", buf, "abcdef01 ABCDEF01");
    wsprintfA(buf, "%hd %hu", -2, -2);         check("hd-hu", buf, "-2 65534");
    wsprintfA(buf, "%.5d", 42);                check(".5d", buf, "00042");
    wsprintfA(buf, "%#x", 0x1a2b);             check("#x", buf, "0x1a2b");
    wsprintfA(buf, "%06d", -42);               check("06d-neg", buf, "-00042");
    wsprintfA(buf, "%.3s", "abcdef");          check(".3s", buf, "abc");
    r = wsprintfA(buf, "%p", (void*)0x140000000);
    {   /* 16 lowercase hex digits, no 0x (Windows-style pointer print) */
        int hexok = (r == 16);
        for (int i = 0; hexok && i < 16; ++i) {
            char ch = buf[i];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) hexok = 0;
        }
        if (hexok) printf("ok %-9s -> '%s'\n", "p", buf);
        else { printf("FAIL p: rc=%d buf='%s'\n", r, buf); fails++; }
    }
    if (fails == 0) { printf("ALL WSPRINTF CHECKS PASS\n"); return 0; }
    printf("WSPRINTF FAILURES: %d\n", fails);
    return 1;
}
