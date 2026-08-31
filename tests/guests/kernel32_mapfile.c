#include <windows.h>
#include <stdio.h>

/* Regression for the kernel32 file-mapping + string/misc batch:
 *   CreateFileMappingW/MapViewOfFile/UnmapViewOfFile (memory-mapped file)
 *   lstrcmpW, MulDiv, GetTempPathW, GetSystemDirectoryW, IsWow64Process
 * Must exit 0. */
int main(void) {
    /* Create a small file, map it, write via the mapping. */
    const char* fname = "papaya_mapfile.bin";
    HANDLE hFile = CreateFileA(fname, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) { printf("fail: create file\n"); return 1; }
    DWORD wrote = 0;
    const char magic[8] = { 'A','B','C','D','E','F','G','\0' };
    WriteFile(hFile, magic, 8, &wrote, NULL);

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READWRITE, 0, 64, NULL);
    if (!hMap) { printf("fail: create mapping\n"); return 2; }
    unsigned char* view = (unsigned char*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) { printf("fail: map view\n"); return 3; }
    /* Verify the mapped bytes match what we wrote. */
    if (view[0] != 'A' || view[6] != 'G') { printf("fail: map content\n"); return 4; }
    view[7] = 'Z';

    if (!UnmapViewOfFile(view)) { printf("fail: unmap\n"); return 5; }
    CloseHandle(hMap);
    CloseHandle(hFile);

    /* string / misc */
    if (lstrcmpW(L"abc", L"abc") != 0) return 6;
    if (lstrcmpW(L"abc", L"abd") >= 0) return 7;
    if (MulDiv(10, 30, 3) != 100) return 8;

    WCHAR tbuf[260] = {0};
    if (GetTempPathW(260, tbuf) == 0 || tbuf[0] == 0) return 9;
    WCHAR sbuf[260] = {0};
    if (GetSystemDirectoryW(sbuf, 260) == 0 || sbuf[0] == 0) return 10;
    if (!IsWow64Process(GetCurrentProcess(), NULL)) return 11; /* NULL out-ptr tolerated */

    DeleteFileA(fname);
    return 0;
}