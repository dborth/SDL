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

#ifndef SDL_wiiuinput_h_
#define SDL_wiiuinput_h_

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "../SDL_sysvideo.h"

/* Wii U specific text input properties, for SDL_StartTextInputWithProperties().
 *
 * Application code uses the literal strings (this header is private). All are
 * optional. The standard properties are used too: SDL_PROP_TEXTINPUT_TYPE_NUMBER
 * (a number type selects the number pad, a password type hides the text),
 * SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN (word suggestions) and
 * SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN (false removes the new line key).
 *
 * SDL_PROP_TEXTINPUT_WIIU_INITIAL_TEXT_STRING   text the input starts with (UTF-8)
 * SDL_PROP_TEXTINPUT_WIIU_HINT_TEXT_STRING      grayed text shown while it is empty
 * SDL_PROP_TEXTINPUT_WIIU_OK_LABEL_STRING       label of the confirm button
 * SDL_PROP_TEXTINPUT_WIIU_MAX_LENGTH_NUMBER     maximum length in characters
 */
#define SDL_PROP_TEXTINPUT_WIIU_INITIAL_TEXT_STRING "SDL.textinput.wiiu.initial_text"
#define SDL_PROP_TEXTINPUT_WIIU_HINT_TEXT_STRING    "SDL.textinput.wiiu.hint_text"
#define SDL_PROP_TEXTINPUT_WIIU_OK_LABEL_STRING     "SDL.textinput.wiiu.ok_label"
#define SDL_PROP_TEXTINPUT_WIIU_MAX_LENGTH_NUMBER   "SDL.textinput.wiiu.max_length"

// Called from the video driver: init and quit, once per pump of events, and the SDL_VideoDevice hooks
extern bool WIIU_InitInput(SDL_VideoDevice *_this);
extern void WIIU_QuitInput(SDL_VideoDevice *_this);
extern void WIIU_PumpInput(SDL_VideoDevice *_this);

extern bool WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this);
extern void WIIU_ShowScreenKeyboard(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props);
extern void WIIU_HideScreenKeyboard(SDL_VideoDevice *_this, SDL_Window *window);

#endif // SDL_VIDEO_DRIVER_WIIU

#endif // SDL_wiiuinput_h_
