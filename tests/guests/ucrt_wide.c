#include <wchar.h>
#include <stdlib.h>

/* Regression: ucrtbase.dll wide-string + CRT surface Papaya implements
 * host-side. Exercises the read/compare/classify/convert UCRT functions that
 * real Unity/Godot-Mono/.NET games import under ucrtbase.dll. Must exit 0.
 *
 * NOTE: this guest links mingw's ucrt, so each of these calls imports
 * ucrtbase symbols that must resolve to Papaya's HLE (not stubs). */
int main(void) {
    /* _wcsicmp */
    if (_wcsicmp(L"Hello", L"hello") != 0) return 1;
    /* _wcsdup + free */
    {
        wchar_t* d = _wcsdup(L"dup-test");
        if (!d || wcslen(d) != 8) return 2;
        free(d);
    }
    /* towupper / towlower */
    if (towupper(L'a') != L'A' || towlower(L'Z') != L'z') return 3;
    /* _wcsnicmp */
    if (_wcsnicmp(L"AbC", L"aBc", 3) != 0) return 4;
    /* wcscspn / wcspbrk / wcsspn */
    if (wcscspn(L"abc-def", L"-") != 3) return 5;
    if (!wcspbrk(L"abc-def", L"-")) return 6;
    if (wcsspn(L"aaa b", L"a") != 3) return 7;
    /* _wtoi / _wtol */
    if (_wtoi(L"-42") != -42) return 8;
    if (_wtol(L"123") != 123) return 9;
    return 0;
}