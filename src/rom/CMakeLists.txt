add_library(papaya-rom STATIC
    src/rom_loader.cpp
    src/steam_rom_bridge.cpp
    src/android_storage_bridge.cpp
    src/android_jni.cpp
)

target_include_directories(papaya-rom PUBLIC
    include
)

target_link_libraries(papaya-rom PUBLIC
    papaya-common
    papaya-steam
    papaya-frontend
)

target_compile_features(papaya-rom PUBLIC cxx_std_23)
