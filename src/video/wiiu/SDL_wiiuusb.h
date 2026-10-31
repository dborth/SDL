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

#ifndef SDL_wiiuusb_h_
#define SDL_wiiuusb_h_

#ifdef SDL_VIDEO_DRIVER_WIIU

/* USB keyboard and mouse. Called from SDL_wiiuinput.c, which the video driver
 * calls: init and quit with the video subsystem, and one pump per pump of
 * events. Nothing here fails the video subsystem: with no USB library
 * available the devices are simply absent. */

// Starts the OS keyboard library and a HID client for mice. Always returns true.
extern bool WIIU_USB_Init(void);

// Stops both. SDL has already removed its devices by then (SDL_VideoQuit() does that first).
extern void WIIU_USB_Quit(void);

// Main thread, in the foreground: hot-plug, then every queued key and mouse event.
extern void WIIU_USB_Pump(void);

// Main thread, in the background: drops what is queued and releases held keys and buttons.
extern void WIIU_USB_Suspend(void);

#endif // SDL_VIDEO_DRIVER_WIIU

#endif // SDL_wiiuusb_h_
