#include <windows.h>
#include <commctrl.h>

/* Regression: comctl32 batch (imported by ~108 game binaries).
 *   InitCommonControls, InitCommonControlsEx
 * Must exit 0. */
int main(void) {
    InitCommonControls();
    INITCOMMONCONTROLSEX iccex = { sizeof(iccex), ICC_STANDARD_CLASSES };
    BOOL ok = InitCommonControlsEx(&iccex);
    if (!ok) { return 1; }
    return 0;
}