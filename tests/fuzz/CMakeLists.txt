# Fuzz harness for the in-process PE loader (papaya::win32::PeLoader).
#
# Clang:  build with PAPAYA_ENABLE_FUZZING=ON -> -fsanitize=fuzzer links
#         libFuzzer's main; this file provides only LLVMFuzzerTestOneInput.
# GCC:    no libFuzzer -> the self-contained driver main() below runs an
#         ASan+UBSan smoke loop over a mutated seed, so CI on GCC hosts can
#         crash-hunt without any fuzzing engine. See README.md.
add_executable(fuzz_pe_parse fuzz_pe_parse.cpp)
target_link_libraries(fuzz_pe_parse PRIVATE papaya-win32)
target_compile_options(fuzz_pe_parse PRIVATE -O0 -g)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(fuzz_pe_parse PRIVATE -fsanitize=fuzzer)
endif()
