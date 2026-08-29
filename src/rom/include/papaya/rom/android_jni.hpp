#pragma once

#include "papaya/common/types.hpp"
#include "papaya/common/error.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// Android NDK JNI Interface Declarations
int papaya_jni_init_runtime(int potato_mode, int device_tier);
int papaya_jni_load_rom_fd(int fd, long total_size_bytes);
void papaya_jni_step_frame();
void papaya_jni_send_touch_input(int slot, int buttons, int axis_x, int axis_y);
void papaya_jni_shutdown();

#ifdef __cplusplus
}
#endif
