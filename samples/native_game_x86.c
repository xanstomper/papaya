#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    HANDLE hFile = CreateFileA("papaya_x86_save.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0x80, NULL);
    if (hFile != (HANDLE)(LONG_PTR)-1) {
        DWORD written = 0;
        char save_data[] = "PAPAYA_X86_HEAVENS_GATE_SUCCESS";
        WriteFile(hFile, save_data, sizeof(save_data), &written, NULL);
        CloseHandle(hFile);
    }
    return 0;
}
