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

#ifndef SDL_wiiuswkbd_h_
#define SDL_wiiuswkbd_h_

#ifdef SDL_VIDEO_DRIVER_WIIU

#include <vpad/input.h>

/* The system software keyboard (nn::swkbd), behind a plain C interface so the
   rest of the video driver does not need C++. See SDL_wiiuswkbd.cpp. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WIIU_SWKBDOptions
{
    bool numpad;            // number pad instead of the full keyboard
    bool password_hidden;   // hide the characters as they are typed
    bool disable_newline;   // no new line key
    bool word_suggestions;
    int max_length;         // in characters; < 0 for no limit
    const char *initial_text; // UTF-8, may be NULL
    const char *hint_text;    // UTF-8, may be NULL
    const char *ok_label;     // UTF-8, may be NULL for the default
} WIIU_SWKBDOptions;

typedef enum WIIU_SWKBDResult
{
    WIIU_SWKBD_RESULT_NONE = 0,
    WIIU_SWKBD_RESULT_OK,    // the user confirmed; the text is returned
    WIIU_SWKBD_RESULT_CANCEL
} WIIU_SWKBDResult;

// Creates the keyboard on first use and shows it. The window is the caller's concern.
extern bool WIIU_SWKBD_Show(const WIIU_SWKBDOptions *options);

// Starts the fade out. The keyboard is still drawn until WIIU_SWKBD_IsActive() is false.
extern void WIIU_SWKBD_Hide(void);

// True from Show until the keyboard has finished fading out
extern bool WIIU_SWKBD_IsActive(void);

/* Run one frame of the keyboard. vpad is the GamePad sample with the touch point
   already calibrated (VPADGetTPCalibratedPoint), or NULL if there is none.
   Returns OK once, with *text set to a string from SDL_malloc() (UTF-8) that the
   caller frees, or CANCEL once. After either, the keyboard is fading out. */
extern WIIU_SWKBDResult WIIU_SWKBD_Update(VPADStatus *vpad, char **text);

/* Draw the keyboard over what is in the color buffer of one screen. The caller
   has bound that screen's GX2 context. output is 0 for the TV, 1 for the GamePad. */
extern void WIIU_SWKBD_Draw(int output);

// Destroy the keyboard and release its memory. Safe to call at any time.
extern void WIIU_SWKBD_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // SDL_VIDEO_DRIVER_WIIU

#endif // SDL_wiiuswkbd_h_
