/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

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

#ifdef SDL_TIME_OGC

#include "../SDL_time_c.h"

#include <errno.h>
#include <ogc/system.h>
#ifdef __wii__
#include <ogc/conf.h>
#endif
#include <time.h>

// GameCube/Wii epoch is Jan 1 2000 UTC
#define UNIX_EPOCH_OFFSET_SEC 946684800

void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
#ifdef __wii__
    // Wii has CONF settings for language/region
    const u32 language = CONF_GetLanguage();
    const s32 region = CONF_GetRegion();

    if (df) {
        switch (language) {
        case CONF_LANG_JAPANESE:
        case CONF_LANG_KOREAN:
        case CONF_LANG_SIMP_CHINESE:
        case CONF_LANG_TRAD_CHINESE:
            *df = SDL_DATE_FORMAT_YYYYMMDD;
            break;
        case CONF_LANG_ENGLISH:
            if (region == CONF_REGION_US) {
                *df = SDL_DATE_FORMAT_MMDDYYYY;
            } else {
                *df = SDL_DATE_FORMAT_DDMMYYYY;
            }
            break;
        default:
            *df = SDL_DATE_FORMAT_DDMMYYYY;
            break;
        }
    }

    if (tf) {
        // CONF_GetTimeFormat() doesn't exist in libogc, use default 24-hour
        *tf = SDL_TIME_FORMAT_24HR;
    }
#else
    // GameCube: Use reasonable defaults
    if (df) {
        *df = SDL_DATE_FORMAT_DDMMYYYY;
    }
    if (tf) {
        *tf = SDL_TIME_FORMAT_24HR;
    }
#endif
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    if (!ticks) {
        return SDL_InvalidParamError("ticks");
    }

    time_t sys_time = time(NULL);
    if (sys_time == (time_t)-1) {
        return SDL_SetError("Failed to get current time");
    }

    // Convert to SDL_Time (nanoseconds since Unix epoch)
    *ticks = SDL_SECONDS_TO_NS((SDL_Time)sys_time);

    return true;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    struct tm tm_storage;
    struct tm *tm = NULL;

    if (!dt) {
        return SDL_InvalidParamError("dt");
    }

    const time_t tval = (time_t)SDL_NS_TO_SECONDS(ticks);

    if (localTime) {
        tm = localtime_r(&tval, &tm_storage);
    } else {
        tm = gmtime_r(&tval, &tm_storage);
    }

    if (tm) {
        dt->year = tm->tm_year + 1900;
        dt->month = tm->tm_mon + 1;
        dt->day = tm->tm_mday;
        dt->hour = tm->tm_hour;
        dt->minute = tm->tm_min;
        dt->second = tm->tm_sec;
        dt->nanosecond = ticks % SDL_NS_PER_SECOND;
        dt->day_of_week = tm->tm_wday;

        if (localTime) {
            // OGC/devkitPPC uses tm->tm_gmtoff for timezone offset
#ifdef __USE_MISC
            dt->utc_offset = tm->tm_gmtoff;
#else
            dt->utc_offset = 0;  // Fallback if not available
#endif
        } else {
            dt->utc_offset = 0;
        }

        return true;
    }

    return SDL_SetError("Time conversion failed");
}

#endif // SDL_TIME_OGC
