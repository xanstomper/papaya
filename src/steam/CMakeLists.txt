add_library(papaya-steam STATIC
    src/steam_api_stub.cpp
)

target_include_directories(papaya-steam PUBLIC
    include
)

target_link_libraries(papaya-steam PUBLIC
    papaya-common
)

target_compile_features(papaya-steam PUBLIC cxx_std_23)
