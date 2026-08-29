add_executable(test_kvm_boot
    test_kvm_boot.cpp
)

target_link_libraries(test_kvm_boot PRIVATE
    papaya-common
    papaya-hv
)

add_executable(test_gpu_pm4
    test_gpu_pm4.cpp
)

target_link_libraries(test_gpu_pm4 PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_gcn_state
    test_gcn_state.cpp
)

target_link_libraries(test_gcn_state PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_shader_translator
    test_shader_translator.cpp
)

target_link_libraries(test_shader_translator PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_threading_sync
    test_threading_sync.cpp
)

target_link_libraries(test_threading_sync PRIVATE
    papaya-common
    papaya-storage
    papaya-hle
    papaya-hv
)

add_executable(test_audio_engine
    test_audio_engine.cpp
)

target_link_libraries(test_audio_engine PRIVATE
    papaya-common
    papaya-audio
)

add_executable(test_emulator_runtime
    test_emulator_runtime.cpp
)

target_link_libraries(test_emulator_runtime PRIVATE
    papaya-common
    papaya-frontend
)

add_executable(test_elf_loader
    test_elf_loader.cpp
)

target_link_libraries(test_elf_loader PRIVATE
    papaya-common
    papaya-storage
)

add_executable(test_param_parser
    test_param_parser.cpp
)

target_link_libraries(test_param_parser PRIVATE
    papaya-common
    papaya-storage
)

add_executable(test_memory_manager
    test_memory_manager.cpp
)

target_link_libraries(test_memory_manager PRIVATE
    papaya-common
    papaya-hle
)

add_executable(test_freebsd_syscalls
    test_freebsd_syscalls.cpp
)

target_link_libraries(test_freebsd_syscalls PRIVATE
    papaya-common
    papaya-hle
)

add_executable(test_sony_nids
    test_sony_nids.cpp
)

target_link_libraries(test_sony_nids PRIVATE
    papaya-common
    papaya-hle
)

add_executable(test_kraken
    test_kraken.cpp
)

target_link_libraries(test_kraken PRIVATE
    papaya-common
    papaya-storage
)

add_executable(test_full_game_loop
    test_full_game_loop.cpp
)

target_link_libraries(test_full_game_loop PRIVATE
    papaya-common
    papaya-frontend
)

add_test(NAME KvmBootTest COMMAND test_kvm_boot)
add_test(NAME GpuPm4Test COMMAND test_gpu_pm4)
add_test(NAME GcnStateTest COMMAND test_gcn_state)
add_test(NAME ShaderTranslatorTest COMMAND test_shader_translator)
add_test(NAME ThreadingSyncTest COMMAND test_threading_sync)
add_test(NAME AudioEngineTest COMMAND test_audio_engine)
add_test(NAME EmulatorRuntimeTest COMMAND test_emulator_runtime)
add_test(NAME ElfLoaderTest COMMAND test_elf_loader)
add_test(NAME ParamParserTest COMMAND test_param_parser)
add_test(NAME MemoryManagerTest COMMAND test_memory_manager)
add_test(NAME FreeBsdSyscallsTest COMMAND test_freebsd_syscalls)
add_test(NAME SonyNidsTest COMMAND test_sony_nids)
add_test(NAME KrakenTest COMMAND test_kraken)
add_test(NAME FullGameLoopTest COMMAND test_full_game_loop)
