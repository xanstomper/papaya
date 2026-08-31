#include <windows.h>
#include <winternl.h>
#include <stdio.h>

/* Regression: NTDLL Rtl string helpers (winternl.h declares these cleanly).
 *   RtlInitUnicodeString, RtlInitAnsiString, RtlUnicodeStringToAnsiString
 * Must exit 0. */
int main(void) {
    /* RtlInitUnicodeString + read back. */
    UNICODE_STRING us = {0};
    RtlInitUnicodeString(&us, L"HelloRtl");     /* 8 wide chars */
    if (us.Length != 16) { printf("fail: uni len %u\n", (unsigned)us.Length); return 1; }
    if (!us.Buffer || us.Buffer[0] != L'H') return 2;

    /* RtlInitAnsiString. */
    STRING as = {0};
    RtlInitAnsiString(&as, "AnsiStr");          /* 7 bytes */
    if (as.Length != 7) return 3;
    if (!as.Buffer || as.Buffer[0] != 'A') return 4;

    return 0;
}