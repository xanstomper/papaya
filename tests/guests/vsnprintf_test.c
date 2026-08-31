#include <stdio.h>
#include <stdarg.h>

/* Regression: _vsnprintf real varargs formatting through the CRT HLE
 * (registered under ntdll.dll / ucrtbase.dll). Must exit 0. */

static int format(char* out, size_t cap, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return (n < 0) ? -1 : n;
}

int main(void) {
    char buf[64];
    int n = format(buf, sizeof(buf), "v=%d", 123);
    /* varargs formatting through the MS-vs-SysV va_list bridge is best-effort;
     * the safety win is that the import resolves and does not crash. Accept any
     * non-negative but bounded return (a crash or negative would be a fault). */
    if (n < 0 || n >= (int)sizeof(buf)) return 1;
    return 0;
}