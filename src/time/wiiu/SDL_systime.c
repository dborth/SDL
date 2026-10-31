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

#ifdef SDL_TIME_WIIU

#include "../SDL_time_c.h"

#include <time.h>

/* Built on newlib's clock_gettime(), localtime_r() and gmtime_r(). wut maps
   CLOCK_REALTIME to OSGetTime() shifted from the Wii U epoch (2000) to the
   Unix epoch, so SDL_Time is in the Unix epoch.

   Not done: the Wii U system settings (language, region, 12/24 hour,
   time zone) are not read. Local time is whatever newlib's TZ gives, which is
   UTC unless the app sets it. */

void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
    if (df) {
        *df = SDL_DATE_FORMAT_DDMMYYYY;
    }
    if (tf) {
        *tf = SDL_TIME_FORMAT_24HR;
    }
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    CHECK_PARAM(!ticks) {
        return SDL_InvalidParamError("ticks");
    }

    struct timespec tp;
    if (clock_gettime(CLOCK_REALTIME, &tp) != 0) {
        return SDL_SetError("Failed to get the current time");
    }

    *ticks = SDL_SECONDS_TO_NS((Sint64)tp.tv_sec) + tp.tv_nsec;
    return true;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    CHECK_PARAM(!dt) {
        return SDL_InvalidParamError("dt");
    }

    // Round towards negative infinity so times before 1970 keep a positive nanosecond part.
    Sint64 secs = ticks / SDL_NS_PER_SECOND;
    Sint64 nanos = ticks % SDL_NS_PER_SECOND;
    if (nanos < 0) {
        nanos += SDL_NS_PER_SECOND;
        secs--;
    }

    struct tm tm_storage;
    const time_t tval = (time_t)secs;
    if ((Sint64)tval != secs) {
        return SDL_SetError("Time is out of range");
    }
    const struct tm *tm = localTime ? localtime_r(&tval, &tm_storage) : gmtime_r(&tval, &tm_storage);

    if (!tm) {
        return SDL_SetError("Time conversion failed");
    }

    dt->year = tm->tm_year + 1900;
    dt->month = tm->tm_mon + 1;
    dt->day = tm->tm_mday;
    dt->hour = tm->tm_hour;
    dt->minute = tm->tm_min;
    dt->second = tm->tm_sec;
    dt->nanosecond = (int)nanos;
    dt->day_of_week = tm->tm_wday;
    dt->utc_offset = 0; // no timezone support is set up for Wii U yet

    return true;
}

#endif // SDL_TIME_WIIU
