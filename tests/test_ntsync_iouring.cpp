#include "papaya/common/logger.hpp"
#include "papaya/kernel/ntsync.hpp"
#include "papaya/kernel/io_uring_streamer.hpp"
#include "papaya/kernel/wine_prefix.hpp"
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
    using namespace papaya::kernel;

    log::info("TEST", "Running unit test: test_ntsync_iouring");

    // 1. Test NTSync Manager
    NtSyncManager ntsync;
    TEST_CHECK(ntsync.initialize().has_value());

    auto evt_res = ntsync.create_nt_event(false, false);
    TEST_CHECK(evt_res.has_value());
    TEST_CHECK(ntsync.signal_nt_object(*evt_res));
    TEST_CHECK(ntsync.wait_for_nt_object(*evt_res, 100));

    // 2. Test io_uring Direct I/O Streamer
    IoUringStreamer io_streamer(128);
    TEST_CHECK(io_streamer.initialize().has_value());
    TEST_CHECK(io_streamer.is_supported());

    // 3. Test Wine Prefix Path Translation
    WinePrefixManager prefix;
    TEST_CHECK(prefix.initialize_prefix().has_value());
    auto game_posix = prefix.resolve_windows_path("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Game\\game.exe");
    TEST_CHECK(game_posix.string().find("drive_c") != std::string::npos);

    log::info("TEST", ">>> test_ntsync_iouring PASSED ALL CHECKS! <<<");
    return 0;
}
