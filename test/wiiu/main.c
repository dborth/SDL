/*
  smoke test for the SDL3 Wii U port. All output goes through SDL_Log,
  which the Wii U build routes to OSReport. Returns 0 if every check passed.
*/
#include <SDL3/SDL.h>

#include "console.h"

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

int tool_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    const int v = SDL_GetVersion();
    SDL_Log("SDL %d.%d.%d (%s)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
            SDL_VERSIONNUM_MICRO(v), SDL_GetRevision());
    SDL_Log("Platform: %s", SDL_GetPlatform());
    SDL_Log("Byte order: %s", SDL_BYTEORDER == SDL_BIG_ENDIAN ? "big-endian" : "little-endian");

    check("platform is \"Wii U\"", SDL_strcmp(SDL_GetPlatform(), "Wii U") == 0);
    check("SDL_BYTEORDER is big-endian", SDL_BYTEORDER == SDL_BIG_ENDIAN);
    check("SDL_Init(0)", SDL_Init(0));

    // Atomics (coreinit path)
    SDL_AtomicInt a;
    SDL_SetAtomicInt(&a, 5);
    check("SDL_AddAtomicInt returns previous value", SDL_AddAtomicInt(&a, 3) == 5);
    check("SDL_GetAtomicInt", SDL_GetAtomicInt(&a) == 8);
    check("SDL_SetAtomicInt returns previous value", SDL_SetAtomicInt(&a, 1) == 8);
    check("SDL_CompareAndSwapAtomicInt succeeds", SDL_CompareAndSwapAtomicInt(&a, 1, 2));
    check("SDL_CompareAndSwapAtomicInt fails on mismatch", !SDL_CompareAndSwapAtomicInt(&a, 1, 3));

    int value = 0;
    void *ptr = &value;
    check("SDL_CompareAndSwapAtomicPointer", SDL_CompareAndSwapAtomicPointer(&ptr, &value, NULL) && ptr == NULL);

    SDL_SpinLock lock = 0;
    check("spinlock try-lock", SDL_TryLockSpinlock(&lock));
    check("spinlock is held", !SDL_TryLockSpinlock(&lock));
    SDL_UnlockSpinlock(&lock);
    check("spinlock re-lock after unlock", SDL_TryLockSpinlock(&lock));
    SDL_UnlockSpinlock(&lock);

    // Timers
    const Uint64 t0 = SDL_GetTicksNS();
    SDL_Delay(100);
    const Uint64 t1 = SDL_GetTicksNS();
    SDL_Log("SDL_Delay(100) took %llu ms", (unsigned long long)((t1 - t0) / SDL_NS_PER_MS));
    check("SDL_Delay(100) lasted at least 100 ms", (t1 - t0) >= 100 * SDL_NS_PER_MS);
    check("SDL_Delay(100) lasted under 250 ms", (t1 - t0) < 250 * SDL_NS_PER_MS);
    check("performance frequency is non-zero", SDL_GetPerformanceFrequency() != 0);

    // Wall-clock time
    SDL_Time now;
    SDL_DateTime dt;
    check("SDL_GetCurrentTime", SDL_GetCurrentTime(&now));
    if (SDL_TimeToDateTime(now, &dt, false)) {
        SDL_Log("UTC now: %04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
        check("UTC year is plausible (>= 2024)", dt.year >= 2024);
    } else {
        check("SDL_TimeToDateTime", false);
    }

    SDL_Quit();
    SDL_Log("Done: %d failure(s)", failures);
    return failures ? 1 : 0;
}
