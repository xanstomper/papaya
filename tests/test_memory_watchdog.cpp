#include "papaya/common/logger.hpp"
#include "papaya/profile/memory_watchdog.hpp"
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
    using namespace papaya::profile;

    log::info("TEST", "Running unit test: test_memory_watchdog");

    // Set threshold low (0.01) to verify callback triggers
    MemoryWatchdog watchdog(0.01f);

    bool callback_fired = false;
    watchdog.register_flush_callback([&]() {
        callback_fired = true;
    });

    auto mem = watchdog.query_memory_status();
    TEST_CHECK(mem.total_ram_bytes > 0);
    TEST_CHECK(mem.memory_pressure_ratio >= 0.0f);

    bool enforced = watchdog.poll_and_enforce();
    TEST_CHECK(enforced);
    TEST_CHECK(callback_fired);
    TEST_CHECK(watchdog.get_flush_event_count() == 1);

    log::info("TEST", "Memory watchdog queried {} MB RAM, pressure={:.1f}%",
              mem.total_ram_bytes / MiB, mem.memory_pressure_ratio * 100.0f);

    log::info("TEST", ">>> test_memory_watchdog PASSED ALL CHECKS! <<<");
    return 0;
}
