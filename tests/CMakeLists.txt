add_executable(test_steam_stub
    test_steam_stub.cpp
)
target_link_libraries(test_steam_stub PRIVATE
    papaya-common
    papaya-steam
)

add_executable(test_cpu_pages
    test_cpu_pages.cpp
)
target_link_libraries(test_cpu_pages PRIVATE
    papaya-common
    papaya-cpu
)

add_executable(test_potato_gpu
    test_potato_gpu.cpp
)
target_link_libraries(test_potato_gpu PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_shader_stripping
    test_shader_stripping.cpp
)
target_link_libraries(test_shader_stripping PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_swapchain_upscaler
    test_swapchain_upscaler.cpp
)
target_link_libraries(test_swapchain_upscaler PRIVATE
    papaya-common
    papaya-gpu
)

add_executable(test_hardware_spoofer
    test_hardware_spoofer.cpp
)
target_link_libraries(test_hardware_spoofer PRIVATE
    papaya-common
    papaya-profile
)

add_executable(test_auto_configurator
    test_auto_configurator.cpp
)
target_link_libraries(test_auto_configurator PRIVATE
    papaya-common
    papaya-profile
)

add_executable(test_memory_watchdog
    test_memory_watchdog.cpp
)
target_link_libraries(test_memory_watchdog PRIVATE
    papaya-common
    papaya-profile
)

add_executable(test_ntsync_iouring
    test_ntsync_iouring.cpp
)
target_link_libraries(test_ntsync_iouring PRIVATE
    papaya-common
    papaya-kernel
)

add_executable(test_audio_input
    test_audio_input.cpp
)
target_link_libraries(test_audio_input PRIVATE
    papaya-common
    papaya-audio
    papaya-input
)

add_executable(test_full_runtime_stack
    test_full_runtime_stack.cpp
)
target_link_libraries(test_full_runtime_stack PRIVATE
    papaya-common
    papaya-frontend
)

add_executable(test_rom_loader
    test_rom_loader.cpp
)
target_link_libraries(test_rom_loader PRIVATE
    papaya-common
    papaya-rom
)

add_executable(test_steam_rom_bridge
    test_steam_rom_bridge.cpp
)
target_link_libraries(test_steam_rom_bridge PRIVATE
    papaya-common
    papaya-rom
)

add_executable(test_android_storage_bridge
    test_android_storage_bridge.cpp
)
target_link_libraries(test_android_storage_bridge PRIVATE
    papaya-common
    papaya-rom
)

add_executable(test_rom_game_loop
    test_rom_game_loop.cpp
)
target_link_libraries(test_rom_game_loop PRIVATE
    papaya-common
    papaya-rom
    papaya-frontend
)

add_executable(test_orchestrator
    test_orchestrator.cpp
)
target_link_libraries(test_orchestrator PRIVATE
    papaya-common
    papaya-orchestrator
)

add_executable(test_pe_loader
    test_pe_loader.cpp
)
target_link_libraries(test_pe_loader PRIVATE
    papaya-common
    papaya-win32
    papaya-steam
    papaya-input
)

# D3D11 Create*Shader wired to the DXBC->GLSL translation layer (Stage 3d).
add_executable(test_d3d11_shaders test_d3d11_shaders.cpp)
target_link_libraries(test_d3d11_shaders PRIVATE
    papaya-win32
    papaya-gpu
)
add_test(NAME D3D11ShadersTest COMMAND test_d3d11_shaders)

# D3D11-state -> Vulkan pipeline mapping (Stage 4b).
add_executable(test_pipeline_map test_pipeline_map.cpp)
target_link_libraries(test_pipeline_map PRIVATE papaya-gpu)
add_test(NAME PipelineMapTest COMMAND test_pipeline_map)

# In-process DXBC -> SPIR-V via glslang (Stage 4c).
add_executable(test_shader_compile test_shader_compile.cpp)
target_link_libraries(test_shader_compile PRIVATE papaya-gpu)
add_test(NAME ShaderCompileTest COMMAND test_shader_compile)

# Device-bound VkPipeline creation from DXBC (Stage 4d; skips without a device).
add_executable(test_vulkan_pipeline test_vulkan_pipeline.cpp)
target_link_libraries(test_vulkan_pipeline PRIVATE papaya-gpu)
add_test(NAME VulkanPipelineTest COMMAND test_vulkan_pipeline)

add_test(NAME SteamStubTest COMMAND test_steam_stub)
add_test(NAME CpuPagesTest COMMAND test_cpu_pages)
add_test(NAME PotatoGpuTest COMMAND test_potato_gpu)
add_test(NAME ShaderStrippingTest COMMAND test_shader_stripping)
add_test(NAME SwapchainUpscalerTest COMMAND test_swapchain_upscaler)
add_test(NAME HardwareSpooferTest COMMAND test_hardware_spoofer)
add_test(NAME AutoConfiguratorTest COMMAND test_auto_configurator)
add_test(NAME MemoryWatchdogTest COMMAND test_memory_watchdog)
add_test(NAME NtSyncIoUringTest COMMAND test_ntsync_iouring)
add_test(NAME AudioInputTest COMMAND test_audio_input)
add_test(NAME FullRuntimeStackTest COMMAND test_full_runtime_stack)
add_test(NAME RomLoaderTest COMMAND test_rom_loader)
add_test(NAME SteamRomBridgeTest COMMAND test_steam_rom_bridge)
add_test(NAME AndroidStorageBridgeTest COMMAND test_android_storage_bridge)
add_test(NAME RomGameLoopTest COMMAND test_rom_game_loop)
add_test(NAME OrchestratorTest COMMAND test_orchestrator)
add_test(NAME PeLoaderTest COMMAND test_pe_loader)
add_test(NAME PythonDaemonTest COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_daemon.py)

# D3D -> Vulkan presentation backend (no crash; clean unsupported handling).
add_executable(test_vulkan_swapchain test_vulkan_swapchain.cpp)
target_link_libraries(test_vulkan_swapchain PRIVATE papaya-gpu)
add_test(NAME VulkanSwapchainTest COMMAND test_vulkan_swapchain)

# .NET/Mono host bridge groundwork (no crash; clean unsupported handling).
add_executable(test_dotnet_bridge test_dotnet_bridge.cpp)
target_link_libraries(test_dotnet_bridge PRIVATE papaya-kernel)
add_test(NAME DotnetBridgeTest COMMAND test_dotnet_bridge)

# CPU translation-backend resolution (ARM64/Box64 groundwork).
add_executable(test_cpu_backend test_cpu_backend.cpp)
target_link_libraries(test_cpu_backend PRIVATE papaya-cpu)
add_test(NAME CpuBackendTest COMMAND test_cpu_backend)

# DXBC shader-container parser (Stage 1 of D3D11 shader translation).
add_executable(test_shader_translator test_shader_translator.cpp)
target_link_libraries(test_shader_translator PRIVATE papaya-gpu)
add_test(NAME ShaderTranslatorTest COMMAND test_shader_translator)

# SM4/SM5 instruction decoder (Stage 2a of D3D11 shader translation).
add_executable(test_shader_decoder test_shader_decoder.cpp)
target_link_libraries(test_shader_decoder PRIVATE papaya-gpu)
add_test(NAME ShaderDecoderTest COMMAND test_shader_decoder)

if (PAPAYA_ENABLE_FUZZING)
    add_subdirectory(fuzz)
endif()
