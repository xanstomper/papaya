add_library(papaya-orchestrator STATIC
    src/orchestrator_service.cpp
)

target_include_directories(papaya-orchestrator PUBLIC
    include
)

target_link_libraries(papaya-orchestrator PUBLIC
    papaya-common
    papaya-steam
    papaya-rom
)

target_compile_features(papaya-orchestrator PUBLIC cxx_std_23)
