# Papaya — Game API Coverage Catalog

Generated from the real mingw-w64 import libraries on this host (37 game-facing DLLs, 12,695 exported functions), cross-referenced against Papaya's Win32 HLE `register_function` table. This is the concrete roadmap for what games actually import.

## Coverage summary (real, data-driven)

| DLL | total | HLE implements | coverage | status |
|---|---|---:|---:|---|
| advapi32.dll | 868 | 16 | 2% | LOW |
| bcrypt.dll | 64 | 1 | 2% | LOW |
| comdlg32.dll | 26 | 0 | 0% | NONE |
| crypt32.dll | 298 | 5 | 2% | LOW |
| d2d1.dll | 12 | 0 | 0% | NONE |
| d3d11.dll | 51 | 2 | 4% | LOW |
| d3d12.dll | 17 | 0 | 0% | NONE |
| d3d9.dll | 16 | 0 | 0% | NONE |
| dcomp.dll | 12 | 0 | 0% | NONE |
| ddraw.dll | 20 | 0 | 0% | NONE |
| dinput.dll | 7 | 0 | 0% | NONE |
| dinput8.dll | 5 | 1 | 20% | PARTIAL |
| dsound.dll | 12 | 0 | 0% | NONE |
| dwmapi.dll | 41 | 2 | 5% | LOW |
| dwrite.dll | 1 | 0 | 0% | NONE |
| dxgi.dll | 57 | 3 | 5% | LOW |
| gdi32.dll | 971 | 14 | 1% | LOW |
| imm32.dll | 137 | 6 | 4% | LOW |
| iphlpapi.dll | 347 | 0 | 0% | NONE |
| kernel32.dll | 1617 | 134 | 8% | LOW |
| msvcrt.dll | 1387 | 45 | 3% | LOW |
| ntdll.dll | 2336 | 6 | 0% | LOW |
| ole32.dll | 527 | 6 | 1% | LOW |
| oleaut32.dll | 413 | 0 | 0% | NONE |
| opengl32.dll | 368 | 4 | 1% | LOW |
| setupapi.dll | 759 | 0 | 0% | NONE |
| shell32.dll | 386 | 8 | 2% | LOW |
| shlwapi.dll | 379 | 0 | 0% | NONE |
| user32.dll | 961 | 53 | 6% | LOW |
| uxtheme.dll | 81 | 0 | 0% | NONE |
| version.dll | 19 | 0 | 0% | NONE |
| winmm.dll | 209 | 11 | 5% | LOW |
| ws2_32.dll | 197 | 17 | 9% | LOW |
| wsock32.dll | 75 | 0 | 0% | NONE |
| xinput1_3.dll | 8 | 7 | 88% | COVERED |
| xinput1_4.dll | 7 | 7 | 100% | COVERED |
| xinput9_1_0.dll | 4 | 3 | 75% | PARTIAL |
| **TOTAL** | **12695** | **351** | **2%** | |

## Highest-value gaps that are REALISTIC to implement (not DXVK-class GPU)

### XInput (gamepad) — DONE for 1.3/1.4/9_1_0
XInputGetState/SetState/GetCapabilities/Enable/GetBatteryInformation/GetKeystroke/GetAudioDeviceIds registered; a real XInput1.3 exe verified working.

### DirectSound (dsound.dll) — real audio, crackable
- missing: DirectSoundCaptureCreate, DirectSoundCaptureCreate8, DirectSoundCaptureEnumerateA, DirectSoundCaptureEnumerateW, DirectSoundCreate, DirectSoundCreate8, DirectSoundEnumerateA, DirectSoundEnumerateW, DirectSoundFullDuplexCreate, DllCanUnloadNow, DllGetClassObject, GetDeviceID

### Winsock (ws2_32) — real networking, crackable
- missing: FreeAddrInfoEx, FreeAddrInfoExW, FreeAddrInfoW, GetAddrInfoExA, GetAddrInfoExCancel, GetAddrInfoExOverlappedResult, GetAddrInfoExW, GetAddrInfoW, GetHostNameW, GetNameInfoW, InetNtopW, InetPtonW, ProcessSocketNotifications, SetAddrInfoExA, SetAddrInfoExW, WEP, WPUCompleteOverlappedRequest, WPUGetProviderPathEx, WSAAccept, WSAAddressToStringA, WSAAddressToStringW, WSAAdvertiseProvider, WSAAsyncGetHostByAddr, WSAAsyncGetHostByName, WSAAsyncGetProtoByName, WSAAsyncGetProtoByNumber, WSAAsyncGetServByName, WSAAsyncGetServByPort, WSAAsyncSelect, WSACancelAsyncRequest, WSACancelBlockingCall, WSAClose

### winmm still-uncovered audio/misc
- e.g. CloseDriver, DefDriverProc, DriverCallback, DrvGetModuleHandle, GetDriverModuleHandle, MigrateAllDrivers, MigrateSoundEvents, NotifyCallbackData, OpenDriver, PlaySound, SendDriverMessage, WOW32DriverCallback, WOW32ResolveMultiMediaHandle, WOWAppExit, WinmmLogoff, WinmmLogon, aux32Message, auxGetDevCapsA, auxGetDevCapsW, auxGetNumDevs, auxGetVolume, auxOutMessage, auxSetVolume, gfxAddGfx, gfxBatchC

## Not feasible in-scope (DXVK-class / years)
- Full D3D11/D3D12/DXGI Vulkan translation (real GPU 3D)
- Full ntdll/setupapi/shell32/oleaut32 surface (large, lower game value)