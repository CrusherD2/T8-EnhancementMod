#pragma once
#include <cstdint>

namespace swap {
    using LogFn = void (*)(const char* format, ...);

    // Loads every project-bo4/bo3port/*.b3pk pack. Returns the number of packs loaded.
    int LoadPacks(LogFn log);

    // Resolves pack donors in the XModel pool and re-resolves after a map reload; cheap to call repeatedly.
    void EnsureResolved();

    // Called before OodleLZ_Decompress: returns a handle for the donor LOD dst belongs to, or null.
    void* Lookup(const uint8_t* dst, uint32_t* offset);

    // Called after a successful decompress into a donor LOD: replaces the chunk and patches metadata.
    void Apply(void* handle, uint32_t offset, uint8_t* dst, uint32_t len);
} // namespace swap
