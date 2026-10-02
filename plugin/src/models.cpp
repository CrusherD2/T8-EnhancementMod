#include "models.hpp"

#include <windows.h>

#include <cstring>

namespace models {
    namespace {
        constexpr uintptr_t POOL_TABLE_RVA = 0x889AD50;
        constexpr uint64_t FIRST_MODEL_HASH = 0x04647533E968C910ull;
        constexpr uint64_t HASH_MASK = 0x0FFFFFFFFFFFFFFFull;
        constexpr size_t INFO_BUFFER = 0x20;
        constexpr size_t INFO_BUFFER_SIZE = 0x28;

        struct PoolInfo {
            uint8_t* entries;
            uint32_t assetSize;
            uint32_t capacity;
        };

        bool ReadPoolAt(uintptr_t table, int index, PoolInfo* pool) {
            uint8_t info[0x10];
            if (!SafeRead((void*)(table + index * 0x20), info, sizeof(info))) {
                return false;
            }
            std::memcpy(&pool->entries, info, 8);
            std::memcpy(&pool->assetSize, info + 8, 4);
            std::memcpy(&pool->capacity, info + 12, 4);
            return pool->entries && pool->assetSize && pool->assetSize < 0x1000;
        }

        bool ReadPool(uintptr_t table, PoolInfo* pool) {
            uint64_t first{};
            return ReadPoolAt(table, XMODEL_POOL, pool) && SafeRead(pool->entries, &first, 8) &&
                   (first & HASH_MASK) == FIRST_MODEL_HASH;
        }

        // lea rax, [rip+table] inside the pool accessor:
        // 48 89 5c 24 ?? 57 48 83 ec ?? 0f b6 f9 48 8d 05 <rel32>
        bool MatchAccessor(const uint8_t* p) {
            static const int pattern[] = { 0x48, 0x89, 0x5c, 0x24, -1, 0x57, 0x48, 0x83, 0xec, -1,
                                           0x0f, 0xb6, 0xf9, 0x48, 0x8d, 0x05 };
            for (size_t i = 0; i < sizeof(pattern) / sizeof(pattern[0]); i++) {
                if (pattern[i] >= 0 && p[i] != pattern[i]) {
                    return false;
                }
            }
            return true;
        }

        constexpr int MAX_CANDIDATES = 16;
        uintptr_t candidates[MAX_CANDIDATES]{};
        int candidateCount{};

        void CollectCandidates() {
            auto base = (uintptr_t)GetModuleHandleA(nullptr);
            candidates[candidateCount++] = base + POOL_TABLE_RVA;

            auto dos = (IMAGE_DOS_HEADER*)base;
            auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t region = base; region < end; region = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
                if (!VirtualQuery((void*)region, &mbi, sizeof(mbi))) {
                    break;
                }
                constexpr DWORD readable = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE;
                if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD)) {
                    continue;
                }
                auto start = (const uint8_t*)mbi.BaseAddress;
                for (size_t off = 0; off + 20 <= mbi.RegionSize; off++) {
                    if (start[off] != 0x48 || !MatchAccessor(start + off)) {
                        continue;
                    }
                    int32_t rel;
                    std::memcpy(&rel, start + off + 16, 4);
                    if (candidateCount < MAX_CANDIDATES) {
                        candidates[candidateCount++] = (uintptr_t)(start + off + 20) + rel;
                    }
                }
            }
        }

        uintptr_t poolTable{};

        bool FindPool(PoolInfo* pool) {
            if (!candidateCount) {
                CollectCandidates();
            }
            for (int i = 0; i < candidateCount; i++) {
                if (ReadPool(candidates[i], pool)) {
                    poolTable = candidates[i];
                    return true;
                }
            }
            return false;
        }

        uint8_t* ScanPool(const PoolInfo& pool, size_t hashOffset, uint64_t hash) {
            __try {
                for (uint32_t i = 0; i < pool.capacity; i++) {
                    uint8_t* entry = pool.entries + (size_t)i * pool.assetSize;
                    if ((*(uint64_t*)(entry + hashOffset) & HASH_MASK) == hash) {
                        return entry;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return nullptr;
        }
    } // namespace

    bool SafeRead(const void* address, void* out, size_t size) {
        __try {
            std::memcpy(out, address, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool SafeWrite(void* address, const void* data, size_t size) {
        DWORD old{};
        if (!VirtualProtect(address, size, PAGE_READWRITE, &old)) {
            return false;
        }
        bool ok{};
        __try {
            std::memcpy(address, data, size);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        DWORD ignored{};
        VirtualProtect(address, size, old, &ignored);
        return ok;
    }

    bool Resolve(const uint64_t* hashes, int hashCount, LodTarget* out, int* outCount) {
        PoolInfo pool{};
        if (!FindPool(&pool)) {
            return false;
        }
        int count = 0;
        for (int n = 0; n < hashCount; n++) {
            for (uint32_t i = 0; i < pool.capacity; i++) {
                uint8_t* entry = pool.entries + (size_t)i * pool.assetSize;
                uint64_t hash{};
                if (!SafeRead(entry, &hash, 8) || (hash & HASH_MASK) != hashes[n]) {
                    continue;
                }
                for (int lod = 0; lod < MAX_LODS; lod++) {
                    LodTarget target{ hashes[n], lod, entry };
                    if (!SafeRead(entry + XMODEL_LODS + lod * 8, &target.mesh, 8) || !target.mesh ||
                        !SafeRead(target.mesh + MESH_INFO, &target.info, 8) || !target.info ||
                        !SafeRead(target.mesh + MESH_SURFACES, &target.surfaces, 8) || !target.surfaces ||
                        !SafeRead(target.mesh + MESH_NUM_SURFS, &target.numSurfs, 1)) {
                        continue;
                    }
                    out[count++] = target;
                }
                break;
            }
        }
        *outCount = count;
        return count > 0;
    }

    bool StillValid(const LodTarget& target) {
        uint64_t hash{};
        uint8_t* mesh{};
        return SafeRead(target.entry, &hash, 8) && (hash & HASH_MASK) == target.modelHash &&
               SafeRead(target.entry + XMODEL_LODS + target.lod * 8, &mesh, 8) && mesh == target.mesh;
    }

    uint8_t* FindAsset(int poolIndex, size_t hashOffset, uint64_t hash) {
        PoolInfo pool{};
        if ((!poolTable && !FindPool(&pool)) || !ReadPoolAt(poolTable, poolIndex, &pool)) {
            return nullptr;
        }
        return ScanPool(pool, hashOffset, hash & HASH_MASK);
    }

    bool StagingBuffer(const LodTarget& target, uint8_t** buffer, uint32_t* size) {
        return SafeRead(target.info + INFO_BUFFER, buffer, 8) &&
               SafeRead(target.info + INFO_BUFFER_SIZE, size, 4) && *buffer && *size;
    }

    bool SetStagingBuffer(const LodTarget& target, uint8_t* buffer) {
        return SafeWrite(target.info + INFO_BUFFER, &buffer, 8);
    }
} // namespace models
