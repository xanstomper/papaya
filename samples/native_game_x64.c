#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    // 1. Module query
    wchar_t exe_path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);

    // 2. High-precision timing
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    // 3. Memory allocation
    void* mem = VirtualAlloc(NULL, 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return 1;

    // 4. File I/O
    HANDLE hFile = CreateFileA("papaya_game_save.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0x80, NULL);
    if (hFile != (HANDLE)(LONG_PTR)-1) {
        DWORD written = 0;
        char save_data[] = "PAPAYA_NATIVE_ENGINE_SUCCESS";
        WriteFile(hFile, save_data, sizeof(save_data), &written, NULL);
        CloseHandle(hFile);
    }

    // 5. Cleanup
    VirtualFree(mem, 0, MEM_RELEASE);
    QueryPerformanceCounter(&end);

    return 0;
}
