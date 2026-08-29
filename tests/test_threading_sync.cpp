#include "papaya/common/logger.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include "papaya/hle/thread_manager.hpp"
#include "papaya/hle/kernel.hpp"
#include "papaya/hv/hypervisor.hpp"
#include "papaya/storage/vfs.hpp"
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_threading_sync");

    HandleTable handle_table;
    ThreadManager thread_mgr(handle_table);

    // 1. Test HleEvent (Auto-reset vs Manual-reset)
    {
        auto auto_evt = std::make_shared<HleEvent>(false, false);
        u32 h_auto = handle_table.insert(auto_evt);

        // Should timeout when not signaled
        assert(handle_table.wait_for_single_object(h_auto, 5) == WAIT_TIMEOUT);

        // Signal in worker thread
        std::thread t([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto_evt->set();
        });

        assert(handle_table.wait_for_single_object(h_auto, 500) == WAIT_OBJECT_0);
        // Because it is auto-reset, second wait should timeout immediately
        assert(handle_table.wait_for_single_object(h_auto, 0) == WAIT_TIMEOUT);

        t.join();
        handle_table.remove(h_auto);
        log::info("TEST", "HleEvent auto-reset tests passed!");
    }

    // 2. Test HleMutex (Recursive locking and mutual exclusion)
    {
        auto mtx = std::make_shared<HleMutex>(false);
        u32 h_mtx = handle_table.insert(mtx);

        assert(mtx->acquire(0x1000, 100) == true);
        assert(mtx->acquire(0x1000, 100) == true); // Recursive lock

        std::atomic<bool> thread_acquired{false};
        std::thread t([&]() {
            // TID 0x2000 should fail to acquire while TID 0x1000 holds it
            bool acq = mtx->acquire(0x2000, 20);
            thread_acquired = acq;
        });

        t.join();
        assert(thread_acquired == false);

        assert(mtx->release(0x1000) == true);
        assert(mtx->release(0x1000) == true); // Fully released

        handle_table.remove(h_mtx);
        log::info("TEST", "HleMutex recursive locking tests passed!");
    }

    // 3. Test HleFutex (WaitOnAddress and WakeByAddressSingle)
    {
        alignas(4) std::atomic<u32> sync_val{0};

        std::thread worker([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            sync_val.store(42);
            HleFutex::wake_by_address_single(&sync_val);
        });

        u32 expected = 0;
        bool futex_res = HleFutex::wait_on_address(&sync_val, expected, sizeof(u32), 500);
        assert(futex_res == true);
        assert(sync_val.load() == 42);

        worker.join();
        log::info("TEST", "HleFutex WaitOnAddress/WakeByAddress tests passed!");
    }

    // 4. Test ThreadLocal Storage (TLS)
    {
        u32 slot1 = thread_mgr.tls_alloc();
        u32 slot2 = thread_mgr.tls_alloc();
        assert(slot1 != TLS_OUT_OF_INDEXES);
        assert(slot2 != TLS_OUT_OF_INDEXES);
        assert(slot1 != slot2);

        // Store distinct values per TID
        thread_mgr.tls_set_value(slot1, 0x1111, 0x1000);
        thread_mgr.tls_set_value(slot1, 0x2222, 0x2000);

        assert(thread_mgr.tls_get_value(slot1, 0x1000) == 0x1111);
        assert(thread_mgr.tls_get_value(slot1, 0x2000) == 0x2222);

        assert(thread_mgr.tls_free(slot1) == true);
        log::info("TEST", "TLS slot allocator tests passed!");
    }

    // 5. Test Full Kernel Win32 HLE Sync Registration
    {
        std::shared_ptr<hv::IHypervisor> hv = hv::create_hypervisor(PlatformBackend::Kvm);
        auto vfs = std::make_shared<storage::VirtualFileSystem>();
        Kernel kernel(hv, vfs);
        assert(kernel.initialize().has_value());

        auto& thunk = kernel.get_thunk_manager();
        const auto& exports = thunk.get_exports();

        // Check if APIs are registered
        bool found_create_event = false;
        bool found_wait = false;
        bool found_create_thread = false;
        bool found_tls = false;

        for (const auto& [id, exp] : exports) {
            if (exp.function_name == "CreateEventA") found_create_event = true;
            if (exp.function_name == "WaitForSingleObject") found_wait = true;
            if (exp.function_name == "CreateThread") found_create_thread = true;
            if (exp.function_name == "TlsAlloc") found_tls = true;
        }

        assert(found_create_event);
        assert(found_wait);
        assert(found_create_thread);
        assert(found_tls);

        log::info("TEST", "Kernel Win32 HLE API export registrations verified!");
    }

    log::info("TEST", ">>> test_threading_sync PASSED ALL CHECKS! <<<");
    return 0;
}
