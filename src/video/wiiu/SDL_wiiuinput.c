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

#ifdef SDL_VIDEO_DRIVER_WIIU

/* GamePad touch, and text input through the system software keyboard.

   Touch: the GamePad's touch panel is an SDL touch device (direct). SDL's own
   touch to mouse conversion (SDL_HINT_TOUCH_MOUSE_EVENTS, on by default) turns
   it into mouse events, so this file sends none. The finger goes to a window
   that is shown on the GamePad; see FindTouchWindow().

   The VPAD data: VPADRead() consumes samples, so two readers in one frame would
   starve each other. When the joystick subsystem is initialized its driver is the
   reader, and the last sample it read is used (it is up to one pump old). When it
   is not, this file reads VPAD itself.

   Text input: the system keyboard (SDL_wiiuswkbd.cpp). It is not modal: the
   application keeps running, and must keep presenting frames, because the
   keyboard is drawn over them in the video driver's present path
   (WIIU_SWKBD_Draw()). The confirmed text arrives as SDL_EVENT_TEXT_INPUT.
*/

#include <vpad/input.h>

#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_touch_c.h"
#include "../SDL_sysvideo.h"

#include "SDL_wiiuinput.h"
#include "SDL_wiiuswkbd.h"
#include "SDL_wiiuusb.h"
#include "SDL_wiiuvideo.h"

#ifdef SDL_JOYSTICK_WIIU
#include "../../joystick/wiiu/SDL_wiiujoystick_c.h"
#endif

// The calibrated touch point is in this space (1280x720), whatever the screen
#define WIIU_TOUCH_SPACE_WIDTH  1280.0f
#define WIIU_TOUCH_SPACE_HEIGHT 720.0f

#define WIIU_TOUCH_ID  ((SDL_TouchID)1)
#define WIIU_FINGER_ID ((SDL_FingerID)1) // one finger; SDL requires a non-zero ID

typedef struct WIIU_InputData
{
    bool touch_registered;
    bool vpad_initialized;

    // The latest GamePad sample, with the touch point calibrated
    bool have_sample;
    bool sample_is_new; // differs from the one used on the previous pump
    VPADStatus sample;

    // Touch
    bool finger_down;
    SDL_Window *finger_window;
    float finger_x; // GamePad pixels
    float finger_y;
    bool finger_has_position;
    float sent_x; // last normalized position sent to SDL
    float sent_y;

    bool keyboard_shown; // we told SDL the screen keyboard is shown
} WIIU_InputData;

static WIIU_InputData WIIU_input;

/* ---------------------------------------------------------------------- */
// The GamePad sample

static bool ReadRawSample(VPADStatus *out, bool *is_new)
{
#ifdef SDL_JOYSTICK_WIIU
    if (SDL_WasInit(SDL_INIT_JOYSTICK)) {
        // The joystick driver reads VPAD; take its copy. Same data twice is not new.
        if (!WIIU_GetGamePadSample(out)) {
            return false;
        }
        *is_new = !WIIU_input.have_sample || SDL_memcmp(out, &WIIU_input.sample, sizeof(*out)) != 0;
        return true;
    }
#endif

    {
        VPADReadError err = VPAD_READ_SUCCESS;
        VPADStatus status;
        int32_t count;

        if (!WIIU_input.vpad_initialized) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            VPADInit();
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
            WIIU_input.vpad_initialized = true;
        }

        SDL_zero(status);
        count = VPADRead(VPAD_CHAN_0, &status, 1, &err);
        switch (err) {
        case VPAD_READ_SUCCESS:
            if (count > 0) {
                *out = status;
                *is_new = true;
                return true;
            }
            break;
        case VPAD_READ_NO_SAMPLES:
        case VPAD_READ_BUSY:
            // Nothing new: keep what we have
            *is_new = false;
            if (WIIU_input.have_sample) {
                *out = WIIU_input.sample;
                return true;
            }
            return false;
        default:
            // VPAD_READ_INVALID_CONTROLLER: no GamePad
            break;
        }
        return false;
    }
}

static void UpdateSample(void)
{
    VPADStatus raw;
    VPADTouchData calibrated;
    bool is_new = false;

    if (!ReadRawSample(&raw, &is_new)) {
        WIIU_input.have_sample = false;
        return;
    }

    // The calibrated point is what nn::swkbd and the touch events want
    SDL_zero(calibrated);
    VPADGetTPCalibratedPoint(VPAD_CHAN_0, &calibrated, &raw.tpNormal);
    raw.tpNormal = calibrated;

    if (!is_new) {
        // Same packet again: its button edges were seen already
        raw.trigger = 0;
        raw.release = 0;
    }
    WIIU_input.sample = raw;
    WIIU_input.sample_is_new = is_new;
    WIIU_input.have_sample = true;
}

/* ---------------------------------------------------------------------- */
// Touch

// Is this window one that shows on the GamePad?
static bool WindowIsOnGamePad(SDL_Window *window)
{
    SDL_WindowData *windowdata = window->internal;

    return windowdata && (windowdata->target & WIIU_TARGET_DRC) && !window->is_destroying &&
           !(window->flags & SDL_WINDOW_HIDDEN);
}

/* The window that gets the touch: the keyboard focus window if it is on the
   GamePad, else the first window that is. There is no touch if none is. */
static SDL_Window *FindTouchWindow(SDL_VideoDevice *_this)
{
    SDL_Window *focus = SDL_GetKeyboardFocus();
    SDL_Window *window;

    if (focus && WindowIsOnGamePad(focus)) {
        return focus;
    }
    for (window = _this->windows; window; window = window->next) {
        if (WindowIsOnGamePad(window)) {
            return window;
        }
    }
    return NULL;
}

static bool WindowStillExists(SDL_VideoDevice *_this, SDL_Window *target)
{
    SDL_Window *window;

    for (window = _this->windows; window; window = window->next) {
        if (window == target) {
            return true;
        }
    }
    return false;
}

/* A point in GamePad pixels to SDL's 0..1 window coordinates. A window that is
   shown on the GamePad shows its top left part at 1:1 (P5), and a window on the
   GamePad's own display is exactly as big as the screen, so the same division
   works for both; points beyond the window (a small window on the TV, routed to
   the GamePad) are clamped to its edge. */
static void ToWindowCoordinates(SDL_Window *window, float x, float y, float *out_x, float *out_y)
{
    float nx = window->w > 0 ? x / (float)window->w : 0.0f;
    float ny = window->h > 0 ? y / (float)window->h : 0.0f;

    *out_x = SDL_clamp(nx, 0.0f, 1.0f);
    *out_y = SDL_clamp(ny, 0.0f, 1.0f);
}

static void FingerUp(SDL_VideoDevice *_this)
{
    if (!WIIU_input.finger_down) {
        return;
    }
    if (WindowStillExists(_this, WIIU_input.finger_window)) {
        SDL_SendTouch(0, WIIU_TOUCH_ID, WIIU_FINGER_ID, WIIU_input.finger_window, SDL_EVENT_FINGER_UP,
                      WIIU_input.sent_x, WIIU_input.sent_y, 0.0f);
    }
    WIIU_input.finger_down = false;
    WIIU_input.finger_window = NULL;
    WIIU_input.finger_has_position = false;
}

static void PumpTouch(SDL_VideoDevice *_this)
{
    SDL_VideoData *videodata = _this->internal;
    const WIIU_Output *drc = &videodata->outputs[WIIU_OUTPUT_DRC];
    const VPADTouchData *tp = &WIIU_input.sample.tpNormal;
    bool touched;
    float nx, ny;

    if (!WIIU_input.touch_registered) {
        return;
    }
    if (!WIIU_input.have_sample) {
        FingerUp(_this); // GamePad gone
        return;
    }

    touched = (tp->touched != 0);
    if (!touched) {
        FingerUp(_this);
        return;
    }

    // A sample the panel marks as not accurate keeps the previous position
    if (tp->validity == VPAD_VALID) {
        WIIU_input.finger_x = SDL_clamp((float)tp->x * ((float)drc->width / WIIU_TOUCH_SPACE_WIDTH), 0.0f, (float)drc->width);
        WIIU_input.finger_y = SDL_clamp((float)tp->y * ((float)drc->height / WIIU_TOUCH_SPACE_HEIGHT), 0.0f, (float)drc->height);
        WIIU_input.finger_has_position = true;
    }
    if (!WIIU_input.finger_has_position) {
        return;
    }

    if (!WIIU_input.finger_down) {
        SDL_Window *window = FindTouchWindow(_this);

        if (!window) {
            return; // nothing on the GamePad to touch
        }
        ToWindowCoordinates(window, WIIU_input.finger_x, WIIU_input.finger_y, &nx, &ny);
        WIIU_input.finger_window = window;
        WIIU_input.finger_down = true;
        WIIU_input.sent_x = nx;
        WIIU_input.sent_y = ny;
        SDL_SendTouch(0, WIIU_TOUCH_ID, WIIU_FINGER_ID, window, SDL_EVENT_FINGER_DOWN, nx, ny, 1.0f);
        return;
    }

    if (!WindowStillExists(_this, WIIU_input.finger_window)) {
        // The window was destroyed under the finger: there is nobody to tell
        WIIU_input.finger_down = false;
        WIIU_input.finger_window = NULL;
        return;
    }
    ToWindowCoordinates(WIIU_input.finger_window, WIIU_input.finger_x, WIIU_input.finger_y, &nx, &ny);
    if (nx != WIIU_input.sent_x || ny != WIIU_input.sent_y) {
        WIIU_input.sent_x = nx;
        WIIU_input.sent_y = ny;
        SDL_SendTouchMotion(0, WIIU_TOUCH_ID, WIIU_FINGER_ID, WIIU_input.finger_window, nx, ny, 1.0f);
    }
}

/* ---------------------------------------------------------------------- */
// Text input

static void PumpKeyboard(void)
{
    VPADStatus vpad;
    char *text = NULL;
    WIIU_SWKBDResult result;

    if (WIIU_SWKBD_IsActive()) {
        vpad = WIIU_input.sample;
        result = WIIU_SWKBD_Update(WIIU_input.have_sample ? &vpad : NULL, &text);
        if (result == WIIU_SWKBD_RESULT_OK && text) {
            SDL_SendKeyboardText(text);
        }
        SDL_free(text);
        if (result != WIIU_SWKBD_RESULT_NONE && WIIU_input.keyboard_shown) {
            // It is fading out; for SDL it is gone
            WIIU_input.keyboard_shown = false;
            SDL_SendScreenKeyboardHidden();
        }
    } else if (WIIU_input.keyboard_shown) {
        WIIU_input.keyboard_shown = false;
        SDL_SendScreenKeyboardHidden();
    }
}

bool WIIU_HasScreenKeyboardSupport(SDL_VideoDevice *_this)
{
    return true;
}

void WIIU_ShowScreenKeyboard(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID props)
{
    WIIU_SWKBDOptions options;
    SDL_TextInputType type = SDL_TEXTINPUT_TYPE_TEXT;

    SDL_zero(options);
    options.word_suggestions = true;
    options.max_length = -1;

    if (props) {
        type = SDL_GetTextInputType(props);
        options.word_suggestions = SDL_GetTextInputAutocorrect(props);
        options.disable_newline = !SDL_GetTextInputMultiline(props);
        options.initial_text = SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_WIIU_INITIAL_TEXT_STRING, NULL);
        options.hint_text = SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_WIIU_HINT_TEXT_STRING, NULL);
        options.ok_label = SDL_GetStringProperty(props, SDL_PROP_TEXTINPUT_WIIU_OK_LABEL_STRING, NULL);
        options.max_length = (int)SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_WIIU_MAX_LENGTH_NUMBER, -1);
    }
    switch (type) {
    case SDL_TEXTINPUT_TYPE_NUMBER:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE:
        options.numpad = true;
        break;
    default:
        break;
    }
    switch (type) {
    case SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN:
    case SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN:
        options.password_hidden = true;
        break;
    default:
        break;
    }

    if (WIIU_SWKBD_IsActive()) {
        return; // already up
    }
    if (!WIIU_SWKBD_Show(&options)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not show the software keyboard: %s", SDL_GetError());
        return;
    }
    WIIU_input.keyboard_shown = true;
    SDL_SendScreenKeyboardShown();
}

void WIIU_HideScreenKeyboard(SDL_VideoDevice *_this, SDL_Window *window)
{
    WIIU_SWKBD_Hide();
    if (WIIU_input.keyboard_shown) {
        WIIU_input.keyboard_shown = false;
        SDL_SendScreenKeyboardHidden();
    }
}

/* ---------------------------------------------------------------------- */
// Init, quit, pump

bool WIIU_InitInput(SDL_VideoDevice *_this)
{
    SDL_zero(WIIU_input);

    if (SDL_AddTouch(WIIU_TOUCH_ID, SDL_TOUCH_DEVICE_DIRECT, "Wii U GamePad Touchscreen") >= 0) {
        WIIU_input.touch_registered = true;
    }
    WIIU_USB_Init(); // USB keyboard and mouse; their absence is not an error
    return true;
}

void WIIU_QuitInput(SDL_VideoDevice *_this)
{
    WIIU_USB_Quit();
    // The keyboard has to go before GX2 does
    WIIU_SWKBD_Shutdown();
    if (WIIU_input.keyboard_shown) {
        SDL_SendScreenKeyboardHidden();
    }
    if (WIIU_input.touch_registered) {
        FingerUp(_this);
        SDL_DelTouch(WIIU_TOUCH_ID);
    }
    SDL_zero(WIIU_input);
}

void WIIU_PumpInput(SDL_VideoDevice *_this)
{
    SDL_VideoData *videodata = _this->internal;

    if (!videodata->has_foreground) {
        // VPAD is not ours in the background, nor are the USB devices
        FingerUp(_this);
        WIIU_USB_Suspend();
        return;
    }

    UpdateSample();
    PumpTouch(_this);
    PumpKeyboard();
    WIIU_USB_Pump();
}

#endif // SDL_VIDEO_DRIVER_WIIU
