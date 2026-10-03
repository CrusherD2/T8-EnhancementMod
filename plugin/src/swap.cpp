#include "swap.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "models.hpp"

namespace swap {
    namespace {
        constexpr uint32_t PACK_MAGIC = 'KP3B';
        constexpr uint32_t PACK_VERSION = 1;
        constexpr size_t INFO_VERTS = 0x04;
        constexpr size_t INFO_WEIGHTS = 0x08;
        constexpr size_t INFO_FACE_ELEMS = 0x0C;
        constexpr size_t INFO_STREAM_SIZE = 0x28;
        constexpr size_t INFO_VERTEX_OFFSET = 0x2C;
        constexpr size_t INFO_UV_OFFSET = 0x30;
        constexpr size_t INFO_WEIGHTS_OFFSET = 0x38;
        constexpr size_t SURFACE_SEGMENTS = 0x02;
        constexpr size_t SURFACE_VERTS = 0x04;
        constexpr size_t SURFACE_TABLE = 0x18;
        constexpr size_t MESH_LOD_THRESHOLD = 0x48;
        constexpr size_t XMODEL_LOD_THRESHOLDS = 0xCC;
        constexpr uint32_t REACH_FROM_LOD = 0;
        constexpr uint32_t TOP_STREAMED_LOD = 6;

        struct Segment {
            uint16_t bone, verts, triStart, tris;
        };

        struct Surface {
            uint16_t verts, tris;
            uint32_t vertexIndex, faceIndex;
            Segment* table;
            uint32_t segmentCount;
        };

        struct PackLod {
            uint32_t lod;
            uint32_t expectVerts, expectFaceElems, expectStreamSize;
            uint32_t verts, weights, faceElems, vertexOffset, uvOffset, weightsOffset;
            std::vector<uint8_t> block;
            std::vector<Surface> surfaces;
            models::LodTarget target{};
            // Staging buffer borrowed from a larger LOD when the block exceeds this LOD's stream.
            uint8_t* borrowed{};
            bool active{};
            std::atomic<bool> patched{};
        };

        struct Pack {
            std::string file;
            uint64_t modelHash;
            std::vector<PackLod*> lods;
        };

        LogFn Log{};
        std::vector<Pack> packs{};
        std::vector<PackLod*> lods{};
        constexpr uint64_t RESOLVE_RETRY_MS = 500;
        constexpr uint64_t STALE_CHECK_MS = 1000;

        std::atomic<bool> resolved{};
        std::mutex resolveMutex{};
        std::shared_mutex lodsMutex{};
        std::atomic<uint64_t> lastResolveTick{};

        template <typename T>
        bool Take(const std::vector<uint8_t>& data, size_t& pos, T* out, size_t count = 1) {
            size_t bytes = sizeof(T) * count;
            if (pos + bytes > data.size()) {
                return false;
            }
            std::memcpy(out, data.data() + pos, bytes);
            pos += bytes;
            return true;
        }

        bool ParsePack(const std::string& file, const std::vector<uint8_t>& data, Pack* pack) {
            size_t pos = 0;
            uint32_t magic{}, version{}, lodCount{};
            if (!Take(data, pos, &magic) || magic != PACK_MAGIC || !Take(data, pos, &version) ||
                version != PACK_VERSION || !Take(data, pos, &pack->modelHash) || !Take(data, pos, &lodCount)) {
                return false;
            }
            pack->file = file;
            for (uint32_t i = 0; i < lodCount; i++) {
                auto* lod = new PackLod{};
                uint32_t header[11]{};
                uint32_t blockSize{}, surfaceCount{};
                if (!Take(data, pos, header, 11) || !Take(data, pos, &surfaceCount)) {
                    return false;
                }
                lod->lod = header[0];
                lod->expectVerts = header[1];
                lod->expectFaceElems = header[2];
                lod->expectStreamSize = header[3];
                lod->verts = header[4];
                lod->weights = header[5];
                lod->faceElems = header[6];
                lod->vertexOffset = header[7];
                lod->uvOffset = header[8];
                lod->weightsOffset = header[9];
                blockSize = header[10];
                for (uint32_t s = 0; s < surfaceCount; s++) {
                    Surface surface{};
                    if (!Take(data, pos, &surface.verts) || !Take(data, pos, &surface.tris) ||
                        !Take(data, pos, &surface.vertexIndex) || !Take(data, pos, &surface.faceIndex) ||
                        !Take(data, pos, &surface.segmentCount)) {
                        return false;
                    }
                    // Lives for the whole process: the game keeps pointing at it once patched.
                    surface.table = (Segment*)VirtualAlloc(nullptr, sizeof(Segment) * surface.segmentCount,
                                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (!surface.table || !Take(data, pos, surface.table, surface.segmentCount)) {
                        return false;
                    }
                    lod->surfaces.push_back(surface);
                }
                lod->block.resize(blockSize);
                if (!Take(data, pos, lod->block.data(), blockSize)) {
                    return false;
                }
                pack->lods.push_back(lod);
            }
            return true;
        }

        bool Validate(PackLod* lod) {
            const models::LodTarget& t = lod->target;
            uint32_t verts{}, faceElems{}, streamSize{};
            if (!models::SafeRead(t.info + INFO_VERTS, &verts, 4) ||
                !models::SafeRead(t.info + INFO_FACE_ELEMS, &faceElems, 4) ||
                !models::SafeRead(t.info + INFO_STREAM_SIZE, &streamSize, 4)) {
                return false;
            }
            if (verts != lod->expectVerts || faceElems != lod->expectFaceElems ||
                streamSize != lod->expectStreamSize || t.numSurfs != lod->surfaces.size()) {
                Log("# swap: lod %u mismatch verts=%u/%u faces=%u/%u stream=0x%x/0x%x surfs=%u/%zu\n", lod->lod,
                    verts, lod->expectVerts, faceElems, lod->expectFaceElems, streamSize, lod->expectStreamSize,
                    t.numSurfs, lod->surfaces.size());
                return false;
            }
            for (size_t s = 0; s < lod->surfaces.size(); s++) {
                uint8_t segments{};
                models::SafeRead(t.surfaces + s * models::SURFACE_SIZE + SURFACE_SEGMENTS, &segments, 1);
                if (segments != lod->surfaces[s].segmentCount) {
                    Log("# swap: lod %u surface %zu has %u segments, pack has %u\n", lod->lod, s, segments,
                        lod->surfaces[s].segmentCount);
                    return false;
                }
            }
            return true;
        }

        // The game sizes the GPU allocation from the patched counts, so only the staging buffer limits the
        // block. Staging buffers must stay inside the streamer's reserved region (it tracks pages by position
        // there), so an oversized LOD borrows the buffer of a larger LOD of the same model that the game never
        // streams (the top LOD). The lender is returned so the caller can stop serving it.
        const models::LodTarget* BorrowBuffer(PackLod* lod, const models::LodTarget* found, int count,
                                              bool* ok) {
            *ok = true;
            if (lod->block.size() <= lod->expectStreamSize) {
                return nullptr;
            }
            const models::LodTarget* lender{};
            uint8_t* buffer{};
            uint32_t size{};
            for (int i = 0; i < count; i++) {
                uint8_t* candidate{};
                uint32_t candidateSize{};
                if (found[i].modelHash == lod->target.modelHash && found[i].lod > (int)lod->lod &&
                    models::StagingBuffer(found[i], &candidate, &candidateSize) &&
                    candidateSize >= lod->block.size() && (!lender || found[i].lod > lender->lod)) {
                    lender = &found[i];
                    buffer = candidate;
                    size = candidateSize;
                }
            }
            *ok = lender && models::SetStagingBuffer(lod->target, buffer);
            if (*ok) {
                lod->borrowed = buffer;
            }
            Log("# swap: lod %u block 0x%zx borrows lod %d staging 0x%p size 0x%x ok=%d\n", lod->lod,
                lod->block.size(), lender ? lender->lod : -1, buffer, size, *ok);
            return *ok ? lender : nullptr;
        }

        // The low LOD buffers are too small for a recognisable port, so the top streamed LOD (which packs keep at
        // full detail) takes over at REACH_FROM_LOD's switch threshold. Thresholds must keep falling with the
        // LOD index.
        void ExtendReach(const Pack& pack) {
            const PackLod* full{};
            for (const PackLod* lod : pack.lods) {
                if (lod->active && lod->lod == TOP_STREAMED_LOD) {
                    full = lod;
                }
            }
            if (!full) {
                return;
            }
            uint8_t* model = full->target.entry;
            bool ok = true;
            for (size_t table : { (size_t)0, (size_t)1 }) {
                auto slot = [&](uint32_t lod) -> uint8_t* {
                    if (table) {
                        return model + XMODEL_LOD_THRESHOLDS + lod * 4;
                    }
                    uint8_t* mesh{};
                    models::SafeRead(model + models::XMODEL_LODS + lod * 8, &mesh, 8);
                    return mesh ? mesh + MESH_LOD_THRESHOLD : nullptr;
                };
                float base{};
                uint8_t* from = slot(REACH_FROM_LOD);
                ok &= from && models::SafeRead(from, &base, 4);
                for (uint32_t lod = REACH_FROM_LOD + 1; ok && lod <= full->lod; lod++) {
                    float value = base * (1.0f - 0.001f * (lod - REACH_FROM_LOD));
                    uint8_t* to = slot(lod);
                    ok &= to && models::SafeWrite(to, &value, 4);
                }
            }
            Log("# swap: %s lod %u used from lod %u distance ok=%d\n", pack.file.c_str(), full->lod,
                REACH_FROM_LOD, ok);
        }

        void PatchMetadata(PackLod* lod) {
            const models::LodTarget& t = lod->target;
            bool ok = models::SafeWrite(t.info + INFO_VERTS, &lod->verts, 4) &&
                      models::SafeWrite(t.info + INFO_WEIGHTS, &lod->weights, 4) &&
                      models::SafeWrite(t.info + INFO_FACE_ELEMS, &lod->faceElems, 4) &&
                      models::SafeWrite(t.info + INFO_VERTEX_OFFSET, &lod->vertexOffset, 4) &&
                      models::SafeWrite(t.info + INFO_UV_OFFSET, &lod->uvOffset, 4) &&
                      models::SafeWrite(t.info + INFO_WEIGHTS_OFFSET, &lod->weightsOffset, 4);
            for (size_t s = 0; ok && s < lod->surfaces.size(); s++) {
                const Surface& surface = lod->surfaces[s];
                uint8_t* record = t.surfaces + s * models::SURFACE_SIZE;
                uint8_t counts[12];
                std::memcpy(counts, &surface.verts, 2);
                std::memcpy(counts + 2, &surface.tris, 2);
                std::memcpy(counts + 4, &surface.vertexIndex, 4);
                std::memcpy(counts + 8, &surface.faceIndex, 4);
                ok = models::SafeWrite(record + SURFACE_VERTS, counts, sizeof(counts)) &&
                     models::SafeWrite(record + SURFACE_TABLE, &surface.table, 8);
            }
            Log("# swap: patched lod %u metadata ok=%d\n", lod->lod, ok);
        }
    } // namespace

    int LoadPacks(LogFn log) {
        Log = log;
        static const char* const kPackDirs[] = {
            "project-bo4/mods/EnhancementModT8/bo3port",
            "project-bo4/bo3port",
        };
        for (const char* dir : kPackDirs) {
        WIN32_FIND_DATAA find{};
        std::string pattern = std::string(dir) + "/*.b3pk";
        HANDLE handle = FindFirstFileA(pattern.c_str(), &find);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            std::string file = std::string(dir) + "/" + find.cFileName;
            FILE* f = _fsopen(file.c_str(), "rb", _SH_DENYNO);
            if (!f) {
                continue;
            }
            std::vector<uint8_t> data;
            fseek(f, 0, SEEK_END);
            data.resize(ftell(f));
            fseek(f, 0, SEEK_SET);
            fread(data.data(), 1, data.size(), f);
            fclose(f);
            Pack pack{};
            if (ParsePack(file, data, &pack)) {
                Log("# swap: loaded %s model=0x%016llx lods=%zu\n", file.c_str(), pack.modelHash, pack.lods.size());
                packs.push_back(pack);
            } else {
                Log("# swap: failed to parse %s\n", file.c_str());
            }
        } while (FindNextFileA(handle, &find));
        FindClose(handle);
        if (!packs.empty()) {
            return (int)packs.size();
        }
        }
        Log("# swap: no packs in EnhancementModT8/bo3port\n");
        return 0;
    }

    void EnsureResolved() {
        if (packs.empty()) {
            return;
        }
        uint64_t now = GetTickCount64();
        if (now - lastResolveTick < (resolved.load() ? STALE_CHECK_MS : RESOLVE_RETRY_MS)) {
            return;
        }
        std::unique_lock lock{ resolveMutex, std::try_to_lock };
        if (!lock || now - lastResolveTick < (resolved.load() ? STALE_CHECK_MS : RESOLVE_RETRY_MS)) {
            return;
        }
        lastResolveTick = now;
        if (resolved.load()) {
            std::shared_lock read{ lodsMutex };
            bool stale = false;
            for (PackLod* lod : lods) {
                stale |= !models::StillValid(lod->target);
            }
            if (!stale) {
                return;
            }
        }
        if (resolved.exchange(false)) {
            std::unique_lock write{ lodsMutex };
            for (PackLod* lod : lods) {
                lod->active = false;
                lod->patched.store(false);
            }
            lods.clear();
            Log("# swap: models unloaded or moved, resolving again\n");
        }

        std::vector<uint64_t> hashes;
        for (const Pack& pack : packs) {
            hashes.push_back(pack.modelHash);
        }
        models::LodTarget found[16 * models::MAX_LODS]{};
        int count{};
        if (!models::Resolve(hashes.data(), (int)hashes.size(), found, &count)) {
            return;
        }
        std::unique_lock write{ lodsMutex };
        for (Pack& pack : packs) {
            for (PackLod* lod : pack.lods) {
                lod->active = false;
                lod->borrowed = nullptr;
                for (int i = 0; i < count; i++) {
                    if (found[i].modelHash == pack.modelHash && found[i].lod == (int)lod->lod) {
                        lod->target = found[i];
                        lod->active = Validate(lod);
                    }
                }
            }
            for (PackLod* lod : pack.lods) {
                // Only the full mesh is shown. The reduced LODs read as boxes from a distance.
                if (lod->lod != TOP_STREAMED_LOD) {
                    lod->active = false;
                }
            }
            for (PackLod* lod : pack.lods) {
                if (!lod->active || lod->block.size() <= lod->expectStreamSize) {
                    continue;
                }
                bool borrowed = false;
                BorrowBuffer(lod, found, count, &borrowed);
                if (!borrowed) {
                    Log("# swap: %s lod %u block 0x%zx exceeds stream 0x%x, left alone\n", pack.file.c_str(),
                        lod->lod, lod->block.size(), lod->expectStreamSize);
                    lod->active = false;
                }
            }
            ExtendReach(pack);
            for (PackLod* lod : pack.lods) {
                Log("# swap: %s lod %u active=%d\n", pack.file.c_str(), lod->lod, lod->active);
                if (lod->active) {
                    lods.push_back(lod);
                }
            }
        }
        resolved.store(true);
    }

    void* Lookup(const uint8_t* dst, uint32_t* offset) {
        if (!resolved.load()) {
            return nullptr;
        }
        std::shared_lock read{ lodsMutex };
        for (PackLod* lod : lods) {
            uint8_t* buffer{};
            uint32_t size{};
            if (models::StagingBuffer(lod->target, &buffer, &size) && dst >= buffer && dst < buffer + size) {
                *offset = (uint32_t)(dst - buffer);
                return lod;
            }
        }
        return nullptr;
    }

    void Apply(void* handle, uint32_t offset, uint8_t* dst, uint32_t len) {
        PackLod* lod = (PackLod*)handle;
        if (offset == 0 && !lod->patched.exchange(true)) {
            PatchMetadata(lod);
        }
        uint32_t blockSize = (uint32_t)lod->block.size();
        uint32_t copy = offset < blockSize ? min(len, blockSize - offset) : 0;
        if (copy) {
            std::memcpy(dst, lod->block.data() + offset, copy);
        }
        if (copy < len) {
            std::memset(dst + copy, 0, len - copy);
        }
        uint32_t end = offset + len;
        if (lod->borrowed && end >= lod->expectStreamSize && end < blockSize) {
            // The streamer only committed this LOD's own stream size; commit the rest of the block ourselves.
            uint8_t* tail = lod->borrowed + end;
            if (VirtualAlloc(tail, blockSize - end, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
                Log("# swap: lod %u commit of tail 0x%p failed (%lu)\n", lod->lod, tail, GetLastError());
            } else {
                std::memcpy(tail, lod->block.data() + end, blockSize - end);
                copy += blockSize - end;
            }
        }
        Log("# swap: lod %u chunk offset=0x%x len=0x%x copied=0x%x\n", lod->lod, offset, len, copy);
    }
} // namespace swap
