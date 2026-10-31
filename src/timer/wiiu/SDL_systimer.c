/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_TIMER_WIIU

#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

/* The Wii U timebase (OSGetSystemTime) is a 64-bit counter that runs at
   OSTimerClockSpeed, a quarter of the bus clock (about 62.16 MHz on retail
   hardware). Unlike OSGetTime() it is not adjusted when the console clock is
   changed, so it is monotonic. wut's CLOCK_MONOTONIC uses the same source. */

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)OSGetSystemTime();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return (Uint64)OSTimerClockSpeed;
}

void SDL_SYS_DelayNS(Uint64 ns)
{
    if (ns == 0) {
        /* SDL_Delay(0) is used by the spinlock slow path to give up the
           timeslice. OSYieldThread() only yields to threads of equal priority,
           so also sleep for one tick, which lets lower-priority threads run. */
        OSYieldThread();
        OSSleepTicks(1);
        return;
    }

    // Split into whole seconds and a remainder so the tick conversion
    // can't overflow 64 bits for long delays.
    const Uint64 freq = (Uint64)OSTimerClockSpeed;
    const Uint64 secs = ns / SDL_NS_PER_SECOND;
    const Uint64 rem_ns = ns % SDL_NS_PER_SECOND;
    Uint64 ticks = secs * freq + (rem_ns * freq) / SDL_NS_PER_SECOND;

    if (rem_ns != 0 && (rem_ns * freq) % SDL_NS_PER_SECOND != 0) {
        ticks++; // never sleep for less than requested
    }
    OSSleepTicks((OSTime)ticks);
}

#endif // SDL_TIMER_WIIU
