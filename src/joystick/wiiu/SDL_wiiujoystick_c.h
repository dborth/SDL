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

#ifndef SDL_wiiujoystick_c_h_
#define SDL_wiiujoystick_c_h_

#ifdef SDL_JOYSTICK_WIIU

#include <vpad/input.h>

/* Copy of the most recent GamePad sample the joystick driver read
   The sample is refreshed by SDL_UpdateJoysticks(). Returns false if the joystick
   driver is not initialized or no GamePad sample has been read yet; a caller
   that has to work without the joystick subsystem must call VPADRead itself.
   Call with the joystick lock held, or accept that the sample can be one
   update old. */
extern bool WIIU_GetGamePadSample(VPADStatus *status);

#endif // SDL_JOYSTICK_WIIU

#endif // SDL_wiiujoystick_c_h_
