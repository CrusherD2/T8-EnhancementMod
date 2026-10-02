#include "materials.hpp"

#include <windows.h>
#include <share.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "dvars.hpp"
#include "models.hpp"

namespace materials {
    namespace {
        constexpr uint64_t HASH_MASK = 0x0FFFFFFFFFFFFFFFull;
        constexpr size_t MATERIAL_SIZE = 0x138;
        constexpr size_t MATERIAL_IMAGE_TABLE = 0x38;
        constexpr size_t MATERIAL_IMAGE_COUNT = 0x130;
        constexpr size_t IMAGE_ENTRY_SIZE = 0x20;
        constexpr size_t IMAGE_ENTRY_SEMANTIC = 0x08;
        constexpr size_t MESH_NUM_SURFS = 0x4C;
        constexpr size_t LOD_MATERIAL_ENTRY = 0x18;
        constexpr size_t LOD_MATERIAL_HANDLES = 0x08;
        constexpr uint64_t UPDATE_INTERVAL_MS = 1000;
        // The renderer draws material array[index], where index lives in these bits of the material's +0x10
        // word, so a clone only renders as itself once it owns a slot of its own.
        constexpr size_t MATERIAL_INDEX_WORD = 0x10;
        constexpr int MATERIAL_INDEX_SHIFT = 24;
        constexpr uint64_t MATERIAL_INDEX_MASK = 0xFFFFull << MATERIAL_INDEX_SHIFT;
        constexpr uint32_t MAX_ARRAY_CAPACITY = 0x10000;
        // Any stock material other than the template works as the second witness for the array base.
        constexpr uint64_t COIL_MATERIAL = 0x210C3A01204308Cull;

        struct Slot {
            uint32_t semantic;
            uint64_t imageHash;
            uint8_t* image;
        };

        struct SurfacePlan {
            int surface;
            bool existing;
            uint64_t materialHash;
            std::vector<Slot> slots;
            uint8_t* source{};     // the stock material (reused, or the clone's template)
            uint8_t* installed{};  // what the donor surface should point at
            uint32_t slot{};       // the clone's material array index
            bool failed{};
            std::vector<std::pair<uint8_t*, uint8_t*>> swapped;  // handle slot, what it held before we installed
            uint8_t* entry{};                                    // the model `swapped` was recorded on
        };

        // The global material array (inside the game module) and its capacity. The live count is the high
        // dword of the qword right after the last slot; slots above it are never handed out or iterated.
        struct MaterialArray {
            uint8_t** slots{};
            uint32_t capacity{};
        } materialArray{};

        // Conditional manifests ("when <dvar hash> <value>") only apply while the dvar holds that value, and
        // are checked every poll so a short effect (a perk drink) can be swapped in and out.
        struct Manifest {
            std::string file;
            uint64_t modelHash{};
            std::vector<SurfacePlan> surfaces;
            bool conditional{};
            uint64_t whenDvar{};
            int whenValue{};
            bool applied{};
            uint8_t* entry{};
        };

        LogFn Log{};
        std::vector<Manifest> manifests{};
        std::mutex updateMutex{};
        uint64_t lastUpdateTick{};
        uint32_t nextSlotFromTop{};

        uint64_t HashAt(const uint8_t* address, size_t offset) {
            uint64_t hash{};
            return models::SafeRead(address + offset, &hash, 8) ? hash & HASH_MASK : 0;
        }

        uint32_t IndexOf(const uint8_t* material) {
            uint64_t word{};
            models::SafeRead(material + MATERIAL_INDEX_WORD, &word, 8);
            return (uint32_t)((word & MATERIAL_INDEX_MASK) >> MATERIAL_INDEX_SHIFT);
        }

        uint8_t* SlotValue(uint32_t slot) {
            uint8_t* value{};
            models::SafeRead(materialArray.slots + slot, &value, 8);
            return value;
        }

        bool ArrayHolds(uint8_t** slots, const uint8_t* material) {
            uint8_t* value{};
            return models::SafeRead(slots + IndexOf(material), &value, 8) && value == material;
        }

        // Finds the array from a stock material: some qword in the game module equals the material's
        // pointer at exactly its own index. A second stock material confirms the base.
        bool FindMaterialArray(const uint8_t* known, const uint8_t* confirm) {
            if (materialArray.slots && ArrayHolds(materialArray.slots, known)) {
                return true;
            }
            materialArray = {};
            auto base = (uint8_t*)GetModuleHandleA(nullptr);
            auto nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
            uint8_t* end = base + nt->OptionalHeader.SizeOfImage;
            uint64_t index = IndexOf(known);
            MEMORY_BASIC_INFORMATION mbi{};
            for (uint8_t* region = base; region < end; region = (uint8_t*)mbi.BaseAddress + mbi.RegionSize) {
                if (!VirtualQuery(region, &mbi, sizeof(mbi))) {
                    break;
                }
                // The unpacked game module keeps its data in PAGE_EXECUTE_READWRITE regions.
                constexpr DWORD writable =
                    PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if (mbi.State != MEM_COMMIT || !(mbi.Protect & writable) || (mbi.Protect & PAGE_GUARD)) {
                    continue;
                }
                auto words = (uint8_t**)mbi.BaseAddress;
                size_t count = mbi.RegionSize / 8;
                for (size_t i = index; i < count; i++) {
                    if (words[i] != known) {
                        continue;
                    }
                    uint8_t** slots = words + (i - index);
                    if (!ArrayHolds(slots, confirm)) {
                        continue;
                    }
                    size_t remaining = count - (i - index) - 1;
                    uint32_t limit = (uint32_t)(remaining < MAX_ARRAY_CAPACITY ? remaining : MAX_ARRAY_CAPACITY);
                    uint32_t used = 0;
                    while (used < limit && slots[used]) {
                        used++;
                    }
                    uint32_t capacity = used;
                    while (capacity < limit && !slots[capacity]) {
                        capacity++;
                    }
                    uint32_t trailerCount = (uint32_t)((uint64_t)slots[capacity] >> 32);
                    if (capacity >= limit || trailerCount != used) {
                        Log("# materials: array candidate %p rejected (used %u, capacity %u, trailer %u)\n", slots,
                            used, capacity, trailerCount);
                        continue;
                    }
                    materialArray = { slots, capacity };
                    Log("# materials: material array %p, %u of %u slots used\n", slots, used, capacity);
                    return true;
                }
            }
            return false;
        }

        bool ParseManifest(const std::string& file, FILE* f, Manifest* manifest) {
            manifest->file = file;
            char line[2048];
            while (fgets(line, sizeof(line), f)) {
                char kind[16]{};
                int surface{}, consumed{};
                unsigned long long hash{};
                if (sscanf_s(line, "model %llx", &hash) == 1) {
                    manifest->modelHash = hash;
                    continue;
                }
                int value{};
                if (sscanf_s(line, "when %llx %d", &hash, &value) == 2) {
                    manifest->conditional = true;
                    manifest->whenDvar = hash;
                    manifest->whenValue = value;
                    continue;
                }
                if (sscanf_s(line, "surface %d %15s %llx%n", &surface, kind, (unsigned)sizeof(kind), &hash,
                             &consumed) != 3) {
                    continue;
                }
                SurfacePlan plan{ surface, !std::strcmp(kind, "existing"), hash };
                const char* rest = line + consumed;
                unsigned semantic{};
                unsigned long long image{};
                int used{};
                while (sscanf_s(rest, " %x=%llx%n", &semantic, &image, &used) == 2) {
                    plan.slots.push_back({ semantic, image, nullptr });
                    rest += used;
                }
                manifest->surfaces.push_back(plan);
            }
            return manifest->modelHash && !manifest->surfaces.empty();
        }

        // The clone copies the template's header, so it is only usable while the template still holds the same
        // bytes (a map reload can put a fresh copy at the same address) and our images are still loaded.
        bool StillCurrent(const SurfacePlan& plan) {
            if (!plan.installed || HashAt(plan.source, 0) != plan.materialHash) {
                return false;
            }
            if (plan.existing) {
                return true;
            }
            uint8_t source[MATERIAL_SIZE], clone[MATERIAL_SIZE];
            if (!models::SafeRead(plan.source, source, MATERIAL_SIZE) ||
                !models::SafeRead(plan.installed, clone, MATERIAL_SIZE)) {
                return false;
            }
            std::memcpy(clone + MATERIAL_IMAGE_TABLE, source + MATERIAL_IMAGE_TABLE, 8);
            std::memcpy(clone + MATERIAL_INDEX_WORD, source + MATERIAL_INDEX_WORD, 8);
            if (std::memcmp(source, clone, MATERIAL_SIZE) || SlotValue(plan.slot) != plan.installed) {
                return false;
            }
            for (const Slot& slot : plan.slots) {
                if (HashAt(slot.image, models::IMAGE_NAME) != (slot.imageHash & HASH_MASK)) {
                    return false;
                }
            }
            return true;
        }

        bool Build(SurfacePlan& plan) {
            plan.installed = nullptr;
            plan.source = models::FindAsset(models::MATERIAL_POOL, 0, plan.materialHash);
            if (!plan.source) {
                return false;
            }
            if (plan.existing) {
                plan.installed = plan.source;
                return true;
            }
            uint8_t* confirm = models::FindAsset(models::MATERIAL_POOL, 0, COIL_MATERIAL);
            if (!FindMaterialArray(plan.source, confirm ? confirm : plan.source)) {
                return false;
            }
            for (Slot& slot : plan.slots) {
                slot.image = models::FindAsset(models::IMAGE_POOL, models::IMAGE_NAME, slot.imageHash);
                if (!slot.image) {
                    return false;
                }
            }

            uint8_t header[MATERIAL_SIZE];
            uint8_t* table{};
            if (!models::SafeRead(plan.source, header, MATERIAL_SIZE)) {
                return false;
            }
            uint8_t count = header[MATERIAL_IMAGE_COUNT];
            std::memcpy(&table, header + MATERIAL_IMAGE_TABLE, 8);
            std::vector<uint8_t> entries(count * IMAGE_ENTRY_SIZE);
            if (!count || !models::SafeRead(table, entries.data(), entries.size())) {
                return false;
            }
            size_t replaced = 0;
            for (uint8_t i = 0; i < count; i++) {
                uint8_t* entry = entries.data() + i * IMAGE_ENTRY_SIZE;
                uint32_t semantic{};
                std::memcpy(&semantic, entry + IMAGE_ENTRY_SEMANTIC, 4);
                for (const Slot& slot : plan.slots) {
                    if (slot.semantic == semantic) {
                        std::memcpy(entry, &slot.image, 8);
                        replaced++;
                    }
                }
            }
            if (replaced != plan.slots.size()) {
                Log("# materials: template 0x%llx has %zu of %zu slots\n", plan.materialHash, replaced,
                    plan.slots.size());
                return false;
            }

            if (!plan.slot) {
                if (!materialArray.capacity || nextSlotFromTop + 1 >= materialArray.capacity / 4) {
                    return false;
                }
                plan.slot = materialArray.capacity - 1 - nextSlotFromTop++;
            }
            uint8_t* occupant = SlotValue(plan.slot);
            if (occupant && HashAt(occupant, 0) != plan.materialHash) {
                Log("# materials: array slot 0x%x is taken by 0x%llx\n", plan.slot, HashAt(occupant, 0));
                return false;
            }

            // Never freed: the renderer can still be drawing with a previous clone after we replace it.
            auto* clone = (uint8_t*)VirtualAlloc(nullptr, MATERIAL_SIZE + entries.size(), MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE);
            if (!clone) {
                return false;
            }
            uint8_t* cloneTable = clone + MATERIAL_SIZE;
            std::memcpy(clone, header, MATERIAL_SIZE);
            std::memcpy(clone + MATERIAL_IMAGE_TABLE, &cloneTable, 8);
            std::memcpy(cloneTable, entries.data(), entries.size());
            uint64_t indexWord{};
            std::memcpy(&indexWord, clone + MATERIAL_INDEX_WORD, 8);
            indexWord = (indexWord & ~MATERIAL_INDEX_MASK) | ((uint64_t)plan.slot << MATERIAL_INDEX_SHIFT);
            std::memcpy(clone + MATERIAL_INDEX_WORD, &indexWord, 8);
            if (!models::SafeWrite(materialArray.slots + plan.slot, &clone, 8)) {
                return false;
            }
            plan.installed = clone;
            return true;
        }

        // Handle slots the plan targets: its surface on every LOD, or (surface -1) every surface of every LOD whose
        // material is the plan's template, since some models keep a material at a different index per LOD.
        std::vector<uint8_t*> TargetSlots(const SurfacePlan& plan, uint8_t* entry, uint8_t* lodMaterials) {
            std::vector<uint8_t*> slots;
            for (int lod = 0; lod < models::MAX_LODS; lod++) {
                uint8_t* mesh{};
                uint8_t numSurfs{};
                uint8_t* handles{};
                if (!models::SafeRead(entry + models::XMODEL_LODS + lod * 8, &mesh, 8) || !mesh ||
                    !models::SafeRead(mesh + MESH_NUM_SURFS, &numSurfs, 1) ||
                    !models::SafeRead(lodMaterials + LOD_MATERIAL_HANDLES + lod * LOD_MATERIAL_ENTRY, &handles, 8) ||
                    !handles) {
                    continue;
                }
                if (plan.surface >= 0) {
                    if (plan.surface < numSurfs) {
                        slots.push_back(handles + plan.surface * 8);
                    }
                    continue;
                }
                for (int s = 0; s < numSurfs; s++) {
                    uint8_t* current{};
                    if (models::SafeRead(handles + s * 8, &current, 8) && current &&
                        (current == plan.installed || HashAt(current, 0) == (plan.materialHash & HASH_MASK))) {
                        slots.push_back(handles + s * 8);
                    }
                }
            }
            return slots;
        }

        void Apply(SurfacePlan& plan, uint8_t* entry, uint8_t* lodMaterials) {
            if (plan.entry != entry) {
                plan.swapped.clear();
                plan.entry = entry;
            }
            for (uint8_t* slot : TargetSlots(plan, entry, lodMaterials)) {
                uint8_t* current{};
                if (!models::SafeRead(slot, &current, 8) || current == plan.installed) {
                    continue;
                }
                plan.swapped.push_back({ slot, current });
                models::SafeWrite(slot, &plan.installed, 8);
            }
        }

        void Restore(SurfacePlan& plan, uint8_t* entry) {
            for (const auto& [slot, original] : plan.swapped) {
                uint8_t* current{};
                if (plan.entry == entry && models::SafeRead(slot, &current, 8) && current == plan.installed) {
                    models::SafeWrite(slot, &original, 8);
                }
            }
            plan.swapped.clear();
        }

        void Install(Manifest& manifest, bool lookup) {
            if (!manifest.entry || HashAt(manifest.entry, 0) != (manifest.modelHash & HASH_MASK)) {
                manifest.entry = lookup ? models::FindAsset(models::XMODEL_POOL, 0, manifest.modelHash) : nullptr;
            }
            uint8_t* entry = manifest.entry;
            uint8_t* lodMaterials{};
            if (!entry || !models::SafeRead(entry + models::XMODEL_MATERIALS, &lodMaterials, 8) || !lodMaterials) {
                return;
            }
            if (manifest.conditional) {
                int value{};
                bool wanted = dvars::ReadInt(manifest.whenDvar, &value) && value == manifest.whenValue;
                if (!wanted) {
                    if (manifest.applied) {
                        for (SurfacePlan& plan : manifest.surfaces) {
                            Restore(plan, entry);
                        }
                        manifest.applied = false;
                        Log("# materials: %s off\n", manifest.file.c_str());
                    }
                    return;
                }
                if (!manifest.applied) {
                    Log("# materials: %s on\n", manifest.file.c_str());
                }
                manifest.applied = true;
            }
            for (SurfacePlan& plan : manifest.surfaces) {
                if (!StillCurrent(plan)) {
                    bool built = Build(plan);
                    if (built || !plan.failed) {
                        Log("# materials: %s surface %d %s 0x%llx -> %p\n", manifest.file.c_str(), plan.surface,
                            plan.existing ? "existing" : "clone of", plan.materialHash, plan.installed);
                    }
                    plan.failed = !built;
                    if (!built) {
                        continue;
                    }
                }
                Apply(plan, entry, lodMaterials);
            }
        }
    } // namespace

    int LoadManifests(LogFn log) {
        Log = log;
        static const char* const kPackDirs[] = {
            "project-bo4/mods/EnhancementModT8/bo3port",
            "project-bo4/bo3port",
        };
        for (const char* dir : kPackDirs) {
        WIN32_FIND_DATAA find{};
        std::string pattern = std::string(dir) + "/*.b3mt";
        HANDLE handle = FindFirstFileA(pattern.c_str(), &find);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            std::string file = std::string(dir) + "/" + find.cFileName;
            FILE* f = _fsopen(file.c_str(), "r", _SH_DENYNO);
            if (!f) {
                continue;
            }
            Manifest manifest{};
            bool ok = ParseManifest(file, f, &manifest);
            fclose(f);
            Log("# materials: %s %s model=0x%llx surfaces=%zu\n", ok ? "loaded" : "failed to parse", file.c_str(),
                manifest.modelHash, manifest.surfaces.size());
            if (ok) {
                manifests.push_back(manifest);
            }
        } while (FindNextFileA(handle, &find));
        FindClose(handle);
        if (!manifests.empty()) {
            return (int)manifests.size();
        }
        }
        Log("# materials: no manifests in EnhancementModT8/bo3port\n");
        return 0;
    }

    void Update() {
        if (manifests.empty()) {
            return;
        }
        std::unique_lock lock{ updateMutex, std::try_to_lock };
        if (!lock) {
            return;
        }
        uint64_t now = GetTickCount64();
        bool full = now - lastUpdateTick >= UPDATE_INTERVAL_MS;
        if (full) {
            lastUpdateTick = now;
        }
        for (Manifest& manifest : manifests) {
            if (full || manifest.conditional) {
                Install(manifest, full);
            }
        }
    }
} // namespace materials
