#include <windows.h>

/* Regression: user32 dialog/menu/item helpers.
 *   GetDlgCtrlID, GetMenu, GetSubMenu, GetMenuItemCount, GetParent,
 *   EnableWindow, CheckMenuItem, EnableMenuItem, GetFocus
 * Exercises pseudo-handle + state functions games call for UI. Must exit 0. */
int main(void) {
    /* GetFocus returns the active window or first window (non-null normally). */
    HWND focus = GetFocus();
    /* GetMenu on a window returns a pseudo HMENU (may be non-null). */
    HMENU m = GetMenu(focus);
    (void)m;
    /* GetSubMenu / GetMenuItemCount on a menu: don't crash, return bounded. */
    HMENU sub = GetSubMenu(m, 0);
    (void)sub;
    (void)GetMenuItemCount(sub);

    /* GetParent / EnableWindow / Check / Enable are safe no-ops. */
    (void)GetParent(focus);
    EnableWindow(focus, TRUE);
    CheckMenuItem(m, 0, MF_BYPOSITION | MF_CHECKED);
    EnableMenuItem(m, 0, MF_BYPOSITION | MF_ENABLED);

    /* GetDlgCtrlID on the focus hwnd is a bounded value. */
    (void)GetDlgCtrlID(focus);

    return 0;
}