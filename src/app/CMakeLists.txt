add_executable(papaya
    main.cpp
)

target_link_libraries(papaya PRIVATE
    papaya-common
    papaya-steam
    papaya-cpu
    papaya-gpu
    papaya-profile
    papaya-kernel
    papaya-audio
    papaya-input
    papaya-rom
    papaya-frontend
)

target_compile_features(papaya PRIVATE cxx_std_23)
