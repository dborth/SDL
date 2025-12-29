/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2023 Sam Lantinga <slouken@libsdl.org>

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

#include "../../SDL_internal.h"

#ifdef __gamecube__

// SDL_RunApp() code for GameCube based on SDL_gamecube_main.c, leap123@canaglie.org

#include "../../video/ogc/SDL_ogcevents_c.h"

/* Standard includes */
#include <stdio.h>

/* OGC includes */
#include <fat.h>
#include <ogcsys.h>

static void ResetCB()
{
    OGC_ResetRequested = true;
}

/* Do initialisation which has to be done first for the console to work */
/* Entry point */
int SDL_RunApp(int argc, char* argv[], SDL_main_func mainFunction, void * reserved)
{
    (void)reserved;

    SYS_SetResetCallback(ResetCB);
    fatInitDefault();

    /* Call the user's main function. Make sure that argv contains at least one
     * element. */
    if (!argv || argv[0] == NULL) {
        static const char *dummy_argv[2] = { "default.dol", NULL };
        argc = 1;
        argv = (char **)dummy_argv;
    }
    return (mainFunction(argc, argv));
}

#endif /* __gamecube__ */

/* vi: set sts=4 ts=4 sw=4 expandtab: */
