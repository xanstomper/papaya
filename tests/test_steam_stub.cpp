#include "papaya/common/logger.hpp"
#include "papaya/steam/steam_api_stub.hpp"
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
    using namespace papaya::steam;

    log::info("TEST", "Running unit test: test_steam_stub");

    SteamProfileConfig cfg{
        .app_id = 1086940, // Baldur's Gate 3
        .player_name = "Odin2Gamer",
        .language = "english",
        .steam_id = 76561198012345678ULL,
        .unlock_all_dlcs = true
    };

    SteamApiStub stub(cfg);
    auto init_res = stub.initialize();
    TEST_CHECK(init_res.has_value());
    TEST_CHECK(stub.is_initialized());
    TEST_CHECK(stub.get_app_id() == 1086940);
    TEST_CHECK(stub.get_player_name() == "Odin2Gamer");
    TEST_CHECK(stub.get_steam_id() == 76561198012345678ULL);

    // 1. SteamAPI Core Callbacks
    TEST_CHECK(stub.steam_api_init());
    TEST_CHECK(!stub.restart_app_if_necessary(1086940)); // Must return false so game proceeds

    // 2. Achievements
    TEST_CHECK(stub.set_achievement("ACH_FIRST_KILL"));
    bool achieved = false;
    TEST_CHECK(stub.get_achievement("ACH_FIRST_KILL", achieved) && achieved);
    TEST_CHECK(stub.get_achievement("ACH_NONEXISTENT", achieved) && !achieved);

    // 3. Stats
    TEST_CHECK(stub.set_stat("STAT_ENEMIES_SLAIN", 42));
    s32 val = 0;
    TEST_CHECK(stub.get_stat("STAT_ENEMIES_SLAIN", val) && val == 42);

    // 4. DLCs
    TEST_CHECK(stub.is_dlc_installed(1086941));
    TEST_CHECK(stub.is_subscribed_app(1086940));

    stub.shutdown();
    TEST_CHECK(!stub.is_initialized());

    log::info("TEST", ">>> test_steam_stub PASSED ALL CHECKS! <<<");
    return 0;
}
