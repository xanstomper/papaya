#include "papaya/common/logger.hpp"
#include "papaya/hle/sync_primitives.hpp"
#include "papaya/hle/thread_manager.hpp"
#include "papaya/hle/kernel.hpp"
#include <cassert>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>

int main() {
    using namespace papaya;
    using namespace papaya::hle;

    log::info("TEST", "Running unit test: test_threading_sync");

    HandleTable handle_table;

    // 1. Test Win32 / PlayStation Event Primitive
    {
        auto auto_event = std::make_shared<HleEvent>(false, false); // Auto-reset
        u32 evt_handle = handle_table.insert(auto_event);

        std::atomic<bool> thread_woke{false};
        std::thread t([&]() {
            u32 wait_res = handle_table.wait_for_single_object(evt_handle, 1000);
            if (wait_res == 0) {
                thread_woke = true;
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!thread_woke.load());

        auto_event->set();
        t.join();
        assert(thread_woke.load());
        log::info("TEST", "Auto-reset Event signaling tests passed!");
    }

    // 2. Test Mutex Primitive
    {
        auto mtx = std::make_shared<HleMutex>(false, 0);

        assert(mtx->acquire(0x1000, 0) == true);
        assert(mtx->acquire(0x1000, 0) == true); // Recursive lock
        assert(mtx->acquire(0x2000, 0) == false); // Blocked for other thread

        assert(mtx->release(0x1000) == true);
        assert(mtx->release(0x1000) == true); // Fully released
        assert(mtx->acquire(0x2000, 0) == true); // Now thread 2 can lock
        assert(mtx->release(0x2000) == true);

        log::info("TEST", "Recursive Mutex tests passed!");
    }

    // 3. Test Linux Futex Primitive (WaitOnAddress / WakeByAddress)
    {
        alignas(8) u32 futex_word = 0;
        std::atomic<bool> worker_done{false};

        std::thread worker([&]() {
            u32 expected = 0;
            // Wait until futex_word != 0
            while (futex_word == 0) {
                HleFutex::wait_on_address(&futex_word, expected, sizeof(u32), 500);
            }
            worker_done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!worker_done.load());

        futex_word = 1;
        HleFutex::wake_by_address_all(&futex_word);
        worker.join();
        assert(worker_done.load());
        log::info("TEST", "Linux Futex (WaitOnAddress/WakeByAddress) tests passed!");
    }

    // 4. Test Thread Manager & TLS Allocation
    {
        ThreadManager thread_mgr(handle_table);
        u32 slot0 = thread_mgr.tls_alloc();
        u32 slot1 = thread_mgr.tls_alloc();
        assert(slot0 != static_cast<u32>(-1));
        assert(slot1 != static_cast<u32>(-1));

        assert(thread_mgr.tls_set_value(slot0, 0xCAFEBABE, 0x1000) == true);
        assert(thread_mgr.tls_get_value(slot0, 0x1000) == 0xCAFEBABE);
        assert(thread_mgr.tls_get_value(slot0, 0x2000) == 0); // Other thread has 0

        assert(thread_mgr.tls_set_value(slot1, 0x1111, 0x1000) == true);
        assert(thread_mgr.tls_set_value(slot1, 0x2222, 0x2000) == true);

        assert(thread_mgr.tls_get_value(slot1, 0x1000) == 0x1111);
        assert(thread_mgr.tls_get_value(slot1, 0x2000) == 0x2222);

        assert(thread_mgr.tls_free(slot1) == true);
        log::info("TEST", "TLS slot allocator tests passed!");
    }

    // 5. Test Full Kernel PlayStation HLE Registration
    {
        std::vector<u8> ram(32 * MiB, 0);
        std::shared_ptr<hv::IHypervisor> hv = hv::create_hypervisor(PlatformBackend::Kvm);
        auto vfs = std::make_shared<storage::VirtualFileSystem>();
        Kernel kernel(hv, vfs, nullptr, nullptr, ConsoleTarget::PlayStation4);
        assert(kernel.initialize(ram.data(), ram.size()).has_value());

        auto& thunk = kernel.get_thunk_manager();
        const auto& exports = thunk.get_exports();

        // Check if PlayStation APIs are registered
        bool found_pthread_create = false;
        bool found_event_flag = false;
        bool found_pad = false;
        bool found_audio = false;

        for (const auto& [id, exp] : exports) {
            if (exp.function_name == "scePthreadCreate") found_pthread_create = true;
            if (exp.function_name == "sceKernelCreateEventFlag") found_event_flag = true;
            if (exp.function_name == "scePadInit") found_pad = true;
            if (exp.function_name == "sceAudioOutInit") found_audio = true;
        }

        assert(found_pthread_create);
        assert(found_event_flag);
        assert(found_pad);
        assert(found_audio);

        log::info("TEST", "PlayStation HLE API export registrations verified!");
    }

    log::info("TEST", ">>> test_threading_sync PASSED ALL CHECKS! <<<");
    return 0;
}
