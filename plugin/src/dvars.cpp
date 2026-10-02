#include "dvars.hpp"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "models.hpp"

namespace dvars {
    namespace {
        constexpr uint64_t NAME_MASK = 0x7FFFFFFFFFFFFFFFull;
        constexpr uint32_t BUCKETS = 0x400;
        constexpr size_t DVAR_HASHNEXT = 0x10;
        constexpr size_t DVAR_VALUE = 0x18;
        constexpr size_t DVAR_TYPE = 0x20;
        constexpr int TYPE_BOOL = 1, TYPE_FLOAT = 2, TYPE_INT = 6, TYPE_ENUM = 7, TYPE_STRING = 8;

        // Dvar lookup: lea rdi, [rip+s_dvarHashTable]; movq rax, xmm0; ...; and eax, 3FFh; mov rdi, [rdi+rax*8]
        const int TABLE_PATTERN[] = { 0x48, 0x8D, 0x3D, -1, -1, -1, -1, 0x66, 0x48, 0x0F, 0x7E, 0xC0, 0x0F, 0x29,
                                      0x44, 0x24, 0x20, 0x25, 0xFF, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x3C, 0xC7 };
        constexpr size_t PATTERN_SIZE = sizeof(TABLE_PATTERN) / sizeof(TABLE_PATTERN[0]);

        uint8_t** table{};
        bool searched{};

        bool Matches(const uint8_t* p) {
            for (size_t i = 0; i < PATTERN_SIZE; i++) {
                if (TABLE_PATTERN[i] >= 0 && p[i] != TABLE_PATTERN[i]) {
                    return false;
                }
            }
            return true;
        }

        uint8_t** FindTable() {
            auto base = (uint8_t*)GetModuleHandleA(nullptr);
            auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
            uint8_t* end = base + nt->OptionalHeader.SizeOfImage;
            MEMORY_BASIC_INFORMATION mbi{};
            for (uint8_t* region = base; region < end; region = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
                if (!VirtualQuery(region, &mbi, sizeof(mbi))) {
                    break;
                }
                constexpr DWORD executable = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
                if (mbi.State != MEM_COMMIT || !(mbi.Protect & executable) || (mbi.Protect & PAGE_GUARD)) {
                    continue;
                }
                auto start = (const uint8_t*)mbi.BaseAddress;
                for (size_t off = 0; off + PATTERN_SIZE <= mbi.RegionSize; off++) {
                    if (start[off] == 0x48 && Matches(start + off)) {
                        int32_t rel;
                        std::memcpy(&rel, start + off + 3, 4);
                        return (uint8_t**)(start + off + 7 + rel);
                    }
                }
            }
            return nullptr;
        }
    } // namespace

    bool ReadInt(uint64_t hash, int* out) {
        if (!searched) {
            table = FindTable();
            searched = true;
        }
        if (!table) {
            return false;
        }
        uint8_t* dvar{};
        if (!models::SafeRead(table + (hash & (BUCKETS - 1)), &dvar, 8)) {
            return false;
        }
        for (int depth = 0; dvar && depth < 256; depth++) {
            uint64_t name{};
            if (!models::SafeRead(dvar, &name, 8)) {
                return false;
            }
            if ((name & NAME_MASK) == (hash & NAME_MASK)) {
                uint8_t* data{};
                int type{};
                uint8_t current[16]{};
                if (!models::SafeRead(dvar + DVAR_VALUE, &data, 8) || !data ||
                    !models::SafeRead(dvar + DVAR_TYPE, &type, 4) || !models::SafeRead(data, current, 16)) {
                    return false;
                }
                switch (type) {
                case TYPE_BOOL:
                    *out = current[0] != 0;
                    return true;
                case TYPE_FLOAT: {
                    float value;
                    std::memcpy(&value, current, 4);
                    *out = (int)value;
                    return true;
                }
                case TYPE_INT:
                case TYPE_ENUM:
                    std::memcpy(out, current, 4);
                    return true;
                case TYPE_STRING: {
                    const char* text{};
                    char buffer[16]{};
                    std::memcpy(&text, current, 8);
                    if (!text || !models::SafeRead(text, buffer, sizeof(buffer) - 1)) {
                        *out = 0;
                        return true;
                    }
                    *out = std::atoi(buffer);
                    return true;
                }
                default:
                    return false;
                }
            }
            if (!models::SafeRead(dvar + DVAR_HASHNEXT, &dvar, 8)) {
                return false;
            }
        }
        return false;
    }
} // namespace dvars
