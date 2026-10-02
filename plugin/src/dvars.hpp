#pragma once
#include <cstdint>

namespace dvars {
    // Current value of the dvar whose name hashes to `hash`, read as an integer (strings are parsed).
    // False if the dvar does not exist (yet).
    bool ReadInt(uint64_t hash, int* out);
} // namespace dvars
