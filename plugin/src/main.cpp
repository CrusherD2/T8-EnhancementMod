// bo3port Shield plugin.
// Replaces donor models' streamed mesh blocks with converted BO3 geometry as OodleLZ_Decompress
// unpacks them (see swap.cpp), points their surfaces at BO3 materials (see materials.cpp), and logs
// every decompress call to project-bo4/bo3port-oodle.csv.
#include <windows.h>
#include <share.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "detours.h"
#include "materials.hpp"
#include "swap.hpp"

namespace {
    using OodleDecompressFn = int64_t (*)(const void* src, uint64_t srcLen, void* dst, uint64_t dstLen,
                                          uint64_t fuzzSafe, uint64_t checkCrc, uint64_t verbosity,
                                          void* decBufBase, uint64_t decBufSize, void* callback,
                                          void* callbackUser, void* decoderMemory, uint64_t decoderMemorySize,
                                          uint64_t threadPhase);

    OodleDecompressFn realDecompress{};
    std::mutex logMutex{};
    FILE* logFile{};
    std::atomic<uint64_t> callCount{};
    constexpr uint64_t MAX_LOGGED_CALLS = 400000;
    constexpr DWORD MATERIAL_POLL_MS = 30;

    void Log(const char* format, ...) {
        std::lock_guard lock{ logMutex };
        if (!logFile) {
            return;
        }
        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fflush(logFile);
    }

    int64_t DecompressHook(const void* src, uint64_t srcLen, void* dst, uint64_t dstLen, uint64_t fuzzSafe,
                           uint64_t checkCrc, uint64_t verbosity, void* decBufBase, uint64_t decBufSize,
                           void* callback, void* callbackUser, void* decoderMemory, uint64_t decoderMemorySize,
                           uint64_t threadPhase) {
        swap::EnsureResolved();
        materials::Update();
        uint32_t offset{};
        void* donor = swap::Lookup((const uint8_t*)dst, &offset);

        int64_t result = realDecompress(src, srcLen, dst, dstLen, fuzzSafe, checkCrc, verbosity, decBufBase,
                                        decBufSize, callback, callbackUser, decoderMemory, decoderMemorySize,
                                        threadPhase);

        if (donor && result > 0) {
            swap::Apply(donor, offset, (uint8_t*)dst, (uint32_t)result);
        }

        uint64_t index = callCount.fetch_add(1);
        if (index < MAX_LOGGED_CALLS) {
            // Oodle's length arguments are 32-bit in this build; the upper register bits are not meaningful.
            Log("%llu,%llu,%lu,0x%p,%u,%u,%lld,%d\n", index, GetTickCount64(), GetCurrentThreadId(), dst,
                (uint32_t)dstLen, (uint32_t)srcLen, result, donor ? 1 : 0);
        }
        return result;
    }

    // Conditional material swaps (perk-drink labels) must follow their dvar within a frame or two, and the
    // decompress hook only runs while something streams.
    DWORD WINAPI PollMaterials(LPVOID) {
        for (;;) {
            Sleep(MATERIAL_POLL_MS);
            materials::Update();
        }
    }

    void InstallHooks() {
        static std::once_flag once{};
        std::call_once(once, [] {
            logFile = _fsopen("project-bo4/bo3port-oodle.csv", "w", _SH_DENYNO);
            Log("index,tick_ms,thread,dst,dst_len,src_len,result,donor\n");
            int packs = swap::LoadPacks(Log);
            Log("# packs loaded: %d\n", packs);
            Log("# material manifests loaded: %d\n", materials::LoadManifests(Log));
            CloseHandle(CreateThread(nullptr, 0, PollMaterials, nullptr, 0, nullptr));

            HMODULE oodle = LoadLibraryA("oo2core_6_win64.dll");
            if (!oodle) {
                Log("# error: oo2core_6_win64.dll not loaded (%lu)\n", GetLastError());
                return;
            }
            realDecompress = (OodleDecompressFn)GetProcAddress(oodle, "OodleLZ_Decompress");
            if (!realDecompress) {
                Log("# error: OodleLZ_Decompress export missing\n");
                return;
            }

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)realDecompress, (PVOID)DecompressHook);
            LONG status = DetourTransactionCommit();
            Log("# hook OodleLZ_Decompress status=%ld module=0x%p\n", status, oodle);
        });
    }
} // namespace

extern "C" __declspec(dllexport) const char* PBO4_GetPluginName() { return "EnhancementModT8"; }

extern "C" __declspec(dllexport) void PBO4_PostUnpack() { InstallHooks(); }

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && logFile) {
        fclose(logFile);
        logFile = nullptr;
    }
    return TRUE;
}
