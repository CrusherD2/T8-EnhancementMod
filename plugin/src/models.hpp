#pragma once
#include <cstdint>

namespace models {
    constexpr int MAX_LODS = 8;
    constexpr size_t MESH_SURFACES = 0x10;
    constexpr size_t MESH_INFO = 0x18;
    constexpr size_t MESH_NUM_SURFS = 0x4C;
    constexpr size_t SURFACE_SIZE = 0x30;

    constexpr int XMODEL_POOL = 4;
    constexpr int MATERIAL_POOL = 6;
    constexpr int IMAGE_POOL = 9;
    constexpr size_t XMODEL_LODS = 0x78;
    constexpr size_t XMODEL_MATERIALS = 0xB8;
    constexpr size_t IMAGE_NAME = 0x20;

    struct LodTarget {
        uint64_t modelHash;
        int lod;
        uint8_t* entry;
        uint8_t* mesh;
        uint8_t* info;
        uint8_t* surfaces;
        uint8_t numSurfs;
    };

    // Copies memory without faulting; false if any byte is unreadable.
    bool SafeRead(const void* address, void* out, size_t size);

    // Writes memory, making the page writable first if needed.
    bool SafeWrite(void* address, const void* data, size_t size);

    // Fills out with every LOD of each model in the XModel pool. False until the pool exists.
    bool Resolve(const uint64_t* hashes, int hashCount, LodTarget* out, int* outCount);

    // False once the game has unloaded or moved the model this target was resolved from.
    bool StillValid(const LodTarget& target);

    // Staging buffer the game will decompress this LOD's stream block into.
    bool StagingBuffer(const LodTarget& target, uint8_t** buffer, uint32_t* size);

    // Points this LOD's staging buffer at `buffer`; its recorded size is left as the donor stream size.
    bool SetStagingBuffer(const LodTarget& target, uint8_t* buffer);

    // Asset in pool `poolIndex` whose (masked) name hash at `hashOffset` equals `hash`, or null.
    uint8_t* FindAsset(int poolIndex, size_t hashOffset, uint64_t hash);
} // namespace models
