# Papaya on Android Handhelds & SBCs

Papaya is engineered specifically for modern ARM Android devices (Snapdragon 8 Gen 2 / Gen 3, AYN Odin 2, Retroid Pocket 5, Dimensity 9300).

---

## Android 15+ 16KB Page Compatibility

Standard x86/x64 PC games expect memory pages of 4096 bytes (4KB). Modern Android 15 kernels default to 16384 bytes (16KB) page sizes, causing unhandled SIGBUS crashes in standard emulators.

Papaya's `PageSizeManager` (`src/cpu/`) includes a sub-page mapping layer that translates 4KB Windows allocations onto 16KB Android page boundaries, allowing seamless compatibility without kernel modifications.

---

## Building the Android APK

1. Install Android NDK (r26b or newer) and CMake 3.22+.
2. From the project root, build the native APK:
   ```bash
   cd android
   ./gradlew assembleRelease
   ```
3. Sideload the APK to your Android handheld:
   ```bash
   adb install -r app/build/outputs/apk/release/app-release.apk
   ```

---

## Playing ROMs via Storage Access Framework (SAF)

1. Launch **Papaya** from your Android app drawer.
2. Select any ROM or ISO using the system Storage Access Framework picker.
3. Papaya will stream the disc sectors directly through the `RomImageLoader` zero-copy bridge and boot the title into Potato Mode.
