#include "papaya/common/logger.hpp"
#include "papaya/rom/android_storage_bridge.hpp"
#include "papaya/rom/android_jni.hpp"
#include <iostream>
#include <cstdlib>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::rom;

    log::info("TEST", "Running unit test: test_android_storage_bridge");

    // 1. Check Content URI detection
    TEST_CHECK(AndroidStorageBridge::is_content_uri("content://com.android.providers.media.documents/document/1234"));
    TEST_CHECK(!AndroidStorageBridge::is_content_uri("/sdcard/ROMs/game.iso"));
    TEST_CHECK(!AndroidStorageBridge::is_content_uri("./local_rom.iso"));

    // 2. Check Android NDK JNI Lifecycle functions
    int init_res = papaya_jni_init_runtime(1, 2);
    TEST_CHECK(init_res == 0);

    papaya_jni_step_frame();
    papaya_jni_send_touch_input(0, 0x1000, 16000, -16000);
    papaya_jni_shutdown();

    log::info("TEST", ">>> test_android_storage_bridge PASSED ALL CHECKS! <<<");
    return 0;
}
