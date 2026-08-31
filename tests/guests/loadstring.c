#include <windows.h>

/* Regression: PE RT_STRING resource loading via LoadStringW/A.
 * Links loadstring.rc (ids 1001/1002) so LoadString must parse the module's
 * resource directory and return the UTF-16 string. Cursor/icon return valid
 * pseudo-handles (non-null). Must exit 0. */
int main(void) {
    /* The module instance is the exe (GetModuleHandle(NULL)). */
    HINSTANCE hInst = GetModuleHandleA(NULL);

    wchar_t buf[128] = {0};
    int n = LoadStringW(hInst, 1001, buf, 128);
    if (n <= 0) return 1;
    if (n != 23) return 2;   /* "Hello from PapaResource" = 23 wide chars */

    char abuf[128] = {0};
    int an = LoadStringA(hInst, 1002, abuf, 128);
    if (an <= 0) return 3;
    if (abuf[0] != 'S') return 4;

    /* Cursor/icon pseudo-handles must be non-null. */
    if (!LoadCursorW(NULL, IDC_ARROW)) return 5;
    if (!LoadIconW(NULL, IDI_APPLICATION)) return 6;

    return 0;
}