#include <stdio.h>
#include <wchar.h>

/* Regression: hle_wfopen must convert guest UTF-16 wide paths to host UTF-8
 * via win_utf16_to_utf8 (NOT wcstombs, which fails and yields an empty path,
 * silently breaking every wide-path open — e.g. Godot loading its .pck).
 * Calls _wfopen with a "C:\" path (resolves CWD-relative) and writes/reads. */
int main(void) {
    const wchar_t* wpath = L"C:\\papaya_wfopen_probe.txt";
    FILE* f = _wfopen(wpath, L"wb");
    if (!f) { printf("fail: open\n"); return 1; }
    if (fputs("papaya-wfopen", f) == EOF) { fclose(f); printf("fail: write\n"); return 2; }
    fclose(f);

    f = _wfopen(wpath, L"rb");
    if (!f) { printf("fail: reopen\n"); return 3; }
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); printf("fail: read\n"); return 4; }
    fclose(f);
    f = _wfopen(wpath, L"rb");
    if (f) { fclose(f); }

    remove("papaya_wfopen_probe.txt");
    printf("ok '%s'\n", buf);
    return buf[0] == 'p' ? 0 : 5;
}