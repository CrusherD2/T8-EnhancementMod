#pragma once
#include <cstdint>

namespace materials {
    using LogFn = void (*)(const char* format, ...);

    // Loads every project-bo4/bo3port/*.b3mt manifest. Returns the number loaded.
    int LoadManifests(LogFn log);

    // Points donor model surfaces at their BO3 materials once the assets exist, and again after a map
    // reload; cheap to call repeatedly.
    void Update();
} // namespace materials
