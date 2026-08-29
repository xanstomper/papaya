add_library(papaya-win32 STATIC
    src/win32_api_hle.cpp
    src/win32_window.cpp
    src/win32_seh.cpp
    src/win32_d3d.cpp
    src/win32_audio.cpp
    src/win32_dsound.cpp
    src/win32_registry.cpp
    src/win32_dinput.cpp
    src/pe_loader.cpp
)

target_include_directories(papaya-win32 PUBLIC
    include
)

target_link_libraries(papaya-win32 PUBLIC
    papaya-common
    papaya-steam
    papaya-input
    X11
)

target_compile_features(papaya-win32 PUBLIC cxx_std_23)
