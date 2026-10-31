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

#ifndef SDL_wiiuvideo_h_
#define SDL_wiiuvideo_h_

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "../SDL_sysvideo.h"

#include <gx2/context.h>
#include <gx2/surface.h>

/* Wii U specific window creation properties and hint.
 *
 * Application code uses the literal strings (this header is private).
 *
 * SDL_PROP_WINDOW_CREATE_WIIU_TARGET_STRING
 *   Which screen(s) receive the window's pixels: "tv", "gamepad" or "both".
 *   When absent, SDL_HINT_WIIU_WINDOW_TARGET is consulted, then the screen of the
 *   display the window is on. The window's size always comes from its display,
 *   so a window routed to the other screen is cropped (or padded with black).
 *
 * SDL_PROP_WINDOW_CREATE_WIIU_PREVENT_SWAP_BOOLEAN
 *   true: updating this window's surface copies it to the scan buffers but does
 *   not swap them or wait. The swap happens on the next update of a window that
 *   does not set this. Use it for a TV window and a GamePad window that should
 *   flip together.
 */
#define SDL_PROP_WINDOW_CREATE_WIIU_TARGET_STRING       "SDL.window.create.wiiu.target"
#define SDL_PROP_WINDOW_CREATE_WIIU_PREVENT_SWAP_BOOLEAN "SDL.window.create.wiiu.prevent_swap"
#define SDL_HINT_WIIU_WINDOW_TARGET                     "SDL_WIIU_WINDOW_TARGET"

// Output indexes: they are also the display order (the TV is the primary display)
#define WIIU_OUTPUT_TV  0
#define WIIU_OUTPUT_DRC 1
#define WIIU_NUM_OUTPUTS 2

// Bit mask of outputs, see WIIU_OUTPUT_*
#define WIIU_TARGET_TV   (1u << WIIU_OUTPUT_TV)
#define WIIU_TARGET_DRC  (1u << WIIU_OUTPUT_DRC)
#define WIIU_TARGET_BOTH (WIIU_TARGET_TV | WIIU_TARGET_DRC)

// One screen. Native size, scan buffer and the GX2 render target in front of it.
typedef struct WIIU_Output
{
    uint32_t width;
    uint32_t height;
    float refresh_rate;
    GX2ScanTarget scan_target;

    void *scan_buffer;            // foreground bucket, owned between acquire and release
    uint32_t scan_buffer_size;

    GX2ColorBuffer color_buffer;  // MEM1, same size as the screen; image is NULL in background
    GX2ContextState *context;     // MEM2, GX2_CONTEXT_STATE_ALIGNMENT, color_buffer bound as target 0

    // TV only
    GX2TVRenderMode tv_render_mode;
    // GamePad only
    GX2DrcRenderMode drc_render_mode;
} WIIU_Output;

struct SDL_VideoData
{
    bool handle_procui;      // SDL called ProcUIInit and so owns the ProcUI state machine
    bool has_foreground;
    bool entering_background; // RELEASE_FOREGROUND was seen; ProcUIDrawDoneRelease() is due on the next pump
    bool exiting_procui;     // ProcUI said EXITING (or we walked it there)
    bool procui_fresh;       // this init called ProcUIInitEx() (not adopted from an earlier init)
    bool bg_will_sent;       // SDL_OnApplicationWillEnterBackground() already sent for this release
    bool quit_sent;          // SDL_EVENT_QUIT was sent because of HOME (Homebrew Launcher)
    bool quitting;           // WIIU_VideoQuit() is running: no lifecycle events any more
    bool gx2_initialized;
    bool swap_pending;       // a GX2SwapScanBuffers() was issued that may not have flipped yet

    void *command_buffer_pool;
    uint32_t swap_interval;

    WIIU_Output outputs[WIIU_NUM_OUTPUTS];
};

struct SDL_DisplayData
{
    int output; // WIIU_OUTPUT_*
};

struct SDL_WindowData
{
    uint32_t target;        // WIIU_TARGET_* mask
    bool prevent_swap;

    GX2Surface framebuffer; // linear, MEM2, the pixels the software renderer draws into
    bool has_framebuffer;
};

/* Services for the GX2 renderer (src/render/wiiu/) */

/* Called with acquired == false just before the driver frees the scan buffers,
 * the color buffers and the heaps when the application loses the foreground
 * (HOME), and with acquired == true after they were recreated. The color buffers'
 * surface.image addresses are different after an acquire. Called from the thread
 * that runs ProcUI (normally the one that pumps events), never from an OS callback. */
typedef void (*WIIU_ForegroundListener)(bool acquired, void *userdata);
extern bool WIIU_AddForegroundListener(WIIU_ForegroundListener callback, void *userdata);
extern void WIIU_RemoveForegroundListener(WIIU_ForegroundListener callback, void *userdata);

// Copy both color buffers to their scan buffers and swap. Does nothing in the background.
extern void WIIU_PresentOutputs(SDL_VideoData *videodata);

// GX2 swap interval (0 = no vsync, n = every n-th refresh). Negative values are unsupported.
extern bool WIIU_SetSwapInterval(SDL_VideoData *videodata, int vsync);

#endif // SDL_VIDEO_DRIVER_WIIU

#endif // SDL_wiiuvideo_h_
