#include "papaya/common/logger.hpp"
#include "papaya/win32/pe_loader.hpp"
#include "papaya/win32/win32_api_hle.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>

#define TEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAILED: " #expr << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            std::abort(); \
        } \
    } while (0)

int main() {
    using namespace papaya;
    using namespace papaya::win32;

    log::info("TEST", "Running unit test: test_pe_loader");

    // 1. Construct a valid in-memory PE32+ (x86-64) binary buffer
    std::vector<u8> pe_buf(4096, 0);

    // Setup DOS Header
    auto* dos = reinterpret_cast<ImageDosHeader*>(pe_buf.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; // "MZ"
    dos->e_lfanew = 128; // Offset to NT header

    // Setup NT Headers
    auto* nt = reinterpret_cast<ImageNtHeaders64*>(pe_buf.data() + dos->e_lfanew);
    nt->signature = IMAGE_NT_SIGNATURE; // "PE\0\0"
    nt->file_header.machine = IMAGE_FILE_MACHINE_AMD64;
    nt->file_header.number_of_sections = 1;
    nt->file_header.size_of_optional_header = sizeof(ImageOptionalHeader64);

    nt->optional_header.magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->optional_header.image_base = 0x140000000;
    nt->optional_header.section_alignment = 4096;
    nt->optional_header.file_alignment = 512;
    nt->optional_header.size_of_image = 8192;
    nt->optional_header.size_of_headers = 512;
    nt->optional_header.address_of_entry_point = 0x1000;

    // Setup Section Header (.text)
    auto* sec = reinterpret_cast<ImageSectionHeader*>(
        reinterpret_cast<u8*>(&nt->optional_header) + sizeof(ImageOptionalHeader64)
    );
    std::memcpy(sec->name, ".text\0\0\0", 8);
    sec->misc.virtual_size = 0x500;
    sec->virtual_address = 0x1000;
    sec->size_of_raw_data = 512;
    sec->pointer_to_raw_data = 512;
    sec->characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    // Write dummy x86_64 ret opcode (0xC3) at section offset
    pe_buf[512] = 0xC3;

    // 2. Load PE with PeLoader
    auto hle = std::make_shared<Win32ApiHle>();
    PeLoader loader(hle);

    auto load_res = loader.load_from_memory(pe_buf);
    TEST_CHECK(load_res.has_value());

    auto& img = *load_res;
    TEST_CHECK(img.image_base != nullptr);
    TEST_CHECK(img.is_64bit == true);
    TEST_CHECK(img.machine == IMAGE_FILE_MACHINE_AMD64);
    TEST_CHECK(img.entry_point == reinterpret_cast<void*>(reinterpret_cast<u8*>(img.image_base) + 0x1000));

    // Verify mapped byte
    u8* ep = reinterpret_cast<u8*>(img.entry_point);
    TEST_CHECK(*ep == 0xC3);

    // 3. Test HLE Dispatch Table
    void* valloc_fn = hle->resolve_symbol("KERNEL32.DLL", "VirtualAlloc");
    TEST_CHECK(valloc_fn != nullptr);

    void* sleep_fn = hle->resolve_symbol("KERNEL32.DLL", "Sleep");
    TEST_CHECK(sleep_fn != nullptr);

    s64 qpc = 0;
    TEST_CHECK(Win32ApiHle::hle_query_performance_counter(&qpc) == 1);
    TEST_CHECK(qpc > 0);

    loader.unload_image(img);

    log::info("TEST", ">>> test_pe_loader PASSED ALL CHECKS! <<<");
    return 0;
}
