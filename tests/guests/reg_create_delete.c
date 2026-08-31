#include <windows.h>
#include <stdio.h>

/* Regression: advapi32 registry create/delete-key + delete-value.
 *   RegCreateKeyW/A, RegSetValueExW, RegQueryValueExW,
 *   RegDeleteValueW, RegDeleteKeyW (and whole-subtree).
 * Must exit 0. Uses the in-memory registry (papaya_registry.json). */
int main(void) {
    HKEY root = HKEY_CURRENT_USER;
    HKEY sub = NULL;
    /* Create a test key: HKCU\Software\PapayaRegTest. */
    long r = RegCreateKeyW(root, L"Software\\PapayaRegTest", &sub);
    if (r != 0 || !sub) { printf("fail: RegCreateKeyW rc=%ld\n", r); return 1; }

    /* Set a DWORD value. */
    DWORD v = 1234;
    r = RegSetValueExW(sub, L"magic", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    if (r != 0) { printf("fail: RegSetValueExW rc=%ld\n", r); return 2; }

    /* Query it back. */
    DWORD type = 0, got = 0, cb = sizeof(got);
    r = RegQueryValueExW(sub, L"magic", NULL, &type, (BYTE*)&got, &cb);
    if (r != 0 || type != REG_DWORD || got != 1234) { printf("fail: query rc=%ld\n", r); return 3; }

    /* Delete the value. */
    r = RegDeleteValueW(sub, L"magic");
    if (r != 0) { printf("fail: RegDeleteValueW rc=%ld\n", r); return 4; }

    RegCloseKey(sub);

    /* Create a child key under the parent (via full path from root), then
     * delete the whole parent (removes child subtree). */
    HKEY child = NULL;
    r = RegCreateKeyA(root, "Software\\PapayaRegTest\\child", &child);
    if (r != 0) { printf("fail: child create rc=%ld\n", r); return 5; }
    RegCloseKey(child);

    /* Delete HKCU\Software\PapayaRegTest (removes child too). */
    r = RegDeleteKeyW(root, L"Software\\PapayaRegTest");
    if (r != 0) { printf("fail: RegDeleteKeyW rc=%ld\n", r); return 6; }

    /* Deleting it again should now fail (not found). */
    r = RegDeleteKeyW(root, L"Software\\PapayaRegTest");
    if (r == 0) { printf("fail: double-delete should fail\n"); return 7; }

    return 0;
}