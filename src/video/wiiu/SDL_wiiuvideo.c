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

#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"

#include "SDL_wiiuvideo.h"
#include "SDL_wiiu_gfx_heap.h"
#include "SDL_wiiuinput.h"
#include "SDL_wiiuswkbd.h"

#include <malloc.h>

#include <coreinit/dynload.h>
#include <coreinit/foreground.h>
#include <coreinit/systeminfo.h>
#include <coreinit/title.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2r/mem.h>

#define WIIUVID_DRIVER_NAME "wiiu"

#define DRC_SCREEN_WIDTH  854
#define DRC_SCREEN_HEIGHT 480

// The scan buffers, color buffers and the window framebuffer all use this GX2
// format. In memory it is the bytes R, G, B, A, which on this big-endian CPU is
// SDL_PIXELFORMAT_RGBA8888, so the software renderer's pixels need no conversion.
#define WIIU_SURFACE_FORMAT GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8
#define WIIU_PIXEL_FORMAT   SDL_PIXELFORMAT_RGBA8888

// ProcUI callbacks cannot be unregistered and cannot be given a pointer that may
// be freed later, so they find the video data through this.
static SDL_VideoData *s_videodata;
static bool s_callbacks_registered;

/* Process-wide ProcUI state. It outlives SDL_Quit() because the process does.
 *  s_procui_owned   SDL called ProcUIInitEx() and has not called ProcUIShutdown()
 *  s_procui_exited  SDL walked ProcUI to its end (or ProcUI said EXITING): the
 *                   process is leaving, SDL cannot be initialized again.
 *                   Same handling as WHBProcInit/WHBProcShutdown: no HOME menu, HOME
 *                   means "exit", and the end is SYSRelaunchTitle()
 *  s_home_denied    HOME was pressed while the HOME menu was disabled (HBL only)
 *  s_relaunched     SYSRelaunchTitle() was called */
static bool s_procui_owned;
static bool s_procui_exited;
static bool s_from_hbl;
static volatile bool s_home_denied;
static bool s_relaunched;
static bool s_atexit_registered;

#define WIIU_HBL_TITLE_ID           0x0005000013374842ull
#define WIIU_MII_MAKER_JPN_TITLE_ID 0x000500101004A000ull
#define WIIU_MII_MAKER_USA_TITLE_ID 0x000500101004A100ull
#define WIIU_MII_MAKER_EUR_TITLE_ID 0x000500101004A200ull

// Cemu does not answer SYSLaunchMenu() with EXITING, so we must not wait for
// it there. Cemu hands out the address 0xFFFFFFFF for coreinit data exports 
// it does not implement.
static bool WIIU_IsCemu(void)
{
    static int cemu = -1;
    static const char *syms[] = { "OSDynLoad_gLoaderLock", "__OSPlatformInfo", "__OSSchedulerLock", "__OSCurrentThread" };
    OSDynLoad_Module coreinit;
    bool found = false;

    if (cemu >= 0) {
        return cemu != 0;
    }
    if (OSDynLoad_Acquire("coreinit", &coreinit) == OS_DYNLOAD_OK) {
        for (size_t i = 0; i < SDL_arraysize(syms) && !found; ++i) {
            void *addr = NULL;
            OSDynLoad_Error err = OSDynLoad_FindExport(coreinit, OS_DYNLOAD_EXPORT_DATA, syms[i], &addr);
            found = (err == OS_DYNLOAD_OK && addr == (void *)0xFFFFFFFF);
        }
        OSDynLoad_Release(coreinit);
    }
    cemu = found ? 1 : 0;
    return found;
}

static bool WIIU_IsHomebrewLauncher(void)
{
    uint64_t id = OSGetTitleID();
    return id == WIIU_HBL_TITLE_ID || id == WIIU_MII_MAKER_JPN_TITLE_ID ||
           id == WIIU_MII_MAKER_USA_TITLE_ID || id == WIIU_MII_MAKER_EUR_TITLE_ID;
}

static void NotifyForegroundListeners(bool acquired);

/* ---------------------------------------------------------------------- */
// Foreground handling (ProcUI acquire and release)

static void ReleaseForeground(SDL_VideoData *videodata)
{
    if (!videodata->has_foreground) {
        return;
    }

    // Listeners go first: they still have a valid foreground to clean up with
    NotifyForegroundListeners(false);

    // Make sure the GPU is no longer using what we are about to free
    GX2DrawDone();

    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        WIIU_Output *output = &videodata->outputs[i];
        if (output->scan_buffer) {
            WIIU_GfxHeap_ForegroundFree(output->scan_buffer);
            output->scan_buffer = NULL;
        }
        if (output->color_buffer.surface.image) {
            WIIU_GfxHeap_MEM1Free(output->color_buffer.surface.image);
            output->color_buffer.surface.image = NULL;
        }
    }

    WIIU_GfxHeap_MEM1Destroy();
    WIIU_GfxHeap_ForegroundDestroy();

    // Avoids a black frame when the foreground is released
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);

    videodata->has_foreground = false;
    videodata->swap_pending = false;
}

static bool AcquireForeground(SDL_VideoData *videodata)
{
    if (!WIIU_GfxHeap_MEM1Init() || !WIIU_GfxHeap_ForegroundInit()) {
        goto fail;
    }

    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        WIIU_Output *output = &videodata->outputs[i];

        output->scan_buffer = WIIU_GfxHeap_ForegroundAlloc(output->scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
        if (!output->scan_buffer) {
            SDL_SetError("Could not allocate 0x%x bytes for a scan buffer", output->scan_buffer_size);
            goto fail;
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU, output->scan_buffer, output->scan_buffer_size);
        if (i == WIIU_OUTPUT_TV) {
            GX2SetTVBuffer(output->scan_buffer, output->scan_buffer_size, output->tv_render_mode,
                           WIIU_SURFACE_FORMAT, GX2_BUFFERING_MODE_DOUBLE);
        } else {
            GX2SetDRCBuffer(output->scan_buffer, output->scan_buffer_size, output->drc_render_mode,
                            WIIU_SURFACE_FORMAT, GX2_BUFFERING_MODE_DOUBLE);
        }

        output->color_buffer.surface.image = WIIU_GfxHeap_MEM1Alloc(output->color_buffer.surface.imageSize,
                                                                    output->color_buffer.surface.alignment);
        if (!output->color_buffer.surface.image) {
            SDL_SetError("Could not allocate 0x%x bytes of MEM1 for a color buffer", output->color_buffer.surface.imageSize);
            goto fail;
        }
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU, output->color_buffer.surface.image, output->color_buffer.surface.imageSize);

        if (!output->context) {
            output->context = (GX2ContextState *)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState)); // This should NOT be SDL_memalign()
            if (!output->context) {
                SDL_OutOfMemory();
                goto fail;
            }
            GX2SetupContextStateEx(output->context, TRUE);
        }

        // The context keeps the address of the color buffer, so bind again after every acquire
        GX2SetContextState(output->context);
        GX2SetColorBuffer(&output->color_buffer, GX2_RENDER_TARGET_0);
        GX2SetViewport(0.0f, 0.0f, (float)output->width, (float)output->height, 0.0f, 1.0f);
        GX2SetScissor(0, 0, output->width, output->height);
        if (i == WIIU_OUTPUT_TV) {
            GX2SetTVScale(output->width, output->height);
        } else {
            GX2SetDRCScale(output->width, output->height);
        }
        GX2ClearColor(&output->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    GX2SetSwapInterval(videodata->swap_interval);
    videodata->has_foreground = true;
    NotifyForegroundListeners(true);
    return true;

fail:
    videodata->has_foreground = true; // so that ReleaseForeground() cleans up what we did get
    ReleaseForeground(videodata);
    return false;
}

/* The callbacks send the lifecycle events when the foreground really changes
 * hands. When SDL runs ProcUI, WIIU_PumpEvents() has already sent
 * SDL_EVENT_WILL_ENTER_BACKGROUND (the application gets one more frame to
 * react); when the application runs ProcUI itself, the release callback sends
 * it. The SDL_OnApplication*() helpers also do the window side: windows are
 * minimized and the keyboard focus is dropped (so no key stays held) on the way
 * out, and restored on the way in. Windows are not hidden: they still exist and
 * keep their size; this is what the Android and iOS drivers do. */
static uint32_t WIIU_AcquireCallback(void *context)
{
    (void)context;
    if (s_videodata && !s_videodata->has_foreground) {
        const bool events = !s_videodata->quitting;
        if (events) {
            SDL_OnApplicationWillEnterForeground();
        }
        // Tells the foreground listeners (audio, joystick) after the GX2 side is back.
        if (!AcquireForeground(s_videodata)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not get the Wii U foreground back: %s", SDL_GetError());
        }
        if (events) {
            SDL_OnApplicationDidEnterForeground();
        }
    }
    return 0;
}

static uint32_t WIIU_ReleaseCallback(void *context)
{
    (void)context;
    if (s_videodata && s_videodata->has_foreground) {
        SDL_VideoData *videodata = s_videodata;
        const bool events = !videodata->quitting;
        if (events && !videodata->bg_will_sent) {
            SDL_OnApplicationWillEnterBackground();
        }
        videodata->bg_will_sent = false;
        if (WIIU_SWKBD_IsActive()) {
            // The keyboard has GX2 resources of its own, made with the heap state of its time.
            // Take it down; the application can show it again with SDL_StartTextInput().
            WIIU_SWKBD_Shutdown();
        }
        ReleaseForeground(videodata); // listeners first: audio pauses, controllers go neutral
        if (events) {
            SDL_OnApplicationDidEnterBackground();
        }
    }
    return 0;
}

// Homebrew Launcher only: HOME is denied by the OS and means "leave".
static uint32_t WIIU_HomeDeniedCallback(void *context)
{
    (void)context;
    s_home_denied = true;
    return 0;
}

static uint32_t WIIU_SaveCallback(void *context)
{
    (void)context;
    OSSavesDone_ReadyToRelease();
    return 0;
}

/* ---------------------------------------------------------------------- */
// Foreground listeners (used by the GX2 renderer)

#define WIIU_MAX_FOREGROUND_LISTENERS 8

static struct
{
    WIIU_ForegroundListener callback;
    void *userdata;
} s_foreground_listeners[WIIU_MAX_FOREGROUND_LISTENERS];

bool WIIU_AddForegroundListener(WIIU_ForegroundListener callback, void *userdata)
{
    for (int i = 0; i < WIIU_MAX_FOREGROUND_LISTENERS; i++) {
        if (!s_foreground_listeners[i].callback) {
            s_foreground_listeners[i].callback = callback;
            s_foreground_listeners[i].userdata = userdata;
            return true;
        }
    }
    return SDL_SetError("Too many Wii U foreground listeners");
}

void WIIU_RemoveForegroundListener(WIIU_ForegroundListener callback, void *userdata)
{
    for (int i = 0; i < WIIU_MAX_FOREGROUND_LISTENERS; i++) {
        if (s_foreground_listeners[i].callback == callback && s_foreground_listeners[i].userdata == userdata) {
            s_foreground_listeners[i].callback = NULL;
            s_foreground_listeners[i].userdata = NULL;
        }
    }
}

static void NotifyForegroundListeners(bool acquired)
{
    for (int i = 0; i < WIIU_MAX_FOREGROUND_LISTENERS; i++) {
        if (s_foreground_listeners[i].callback) {
            s_foreground_listeners[i].callback(acquired, s_foreground_listeners[i].userdata);
        }
    }
}

/* ---------------------------------------------------------------------- */
// Presenting

// Wait until the swap issued by the previous frame has been flipped, so that the
// scan buffers are free to be written. Gives up after 10 vertical syncs rather
// than hanging if the GPU is stuck (the same limit as wut).
static void WaitForPreviousSwap(SDL_VideoData *videodata)
{
    uint32_t swap_count, flip_count;
    OSTime last_flip, last_vsync;
    int waits = 0;

    if (!videodata->swap_pending) {
        return;
    }
    for (;;) {
        GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
        if (flip_count >= swap_count) {
            break;
        }
        if (++waits > 10) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Wii U: timed out waiting for the previous swap");
            break;
        }
        GX2WaitForVsync();
    }
    videodata->swap_pending = false;
}

bool WIIU_SetSwapInterval(SDL_VideoData *videodata, int vsync)
{
    // The display refreshes every 1/60 s (1/50 s on a PAL 576i TV); GX2's swap interval counts refreshes.
    if (vsync < 0) {
        return SDL_Unsupported(); // no adaptive vsync
    }
    videodata->swap_interval = (uint32_t)vsync;
    if (videodata->has_foreground) {
        GX2SetSwapInterval(videodata->swap_interval);
    }
    return true;
}

static bool WIIU_SetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int vsync)
{
    return WIIU_SetSwapInterval(_this->internal, vsync);
}

static bool WIIU_GetWindowFramebufferVSync(SDL_VideoDevice *_this, SDL_Window *window, int *vsync)
{
    SDL_VideoData *videodata = _this->internal;
    *vsync = (int)videodata->swap_interval;
    return true;
}

static void CopyWindowToOutput(SDL_Window *window, WIIU_Output *output)
{
    SDL_WindowData *windowdata = window->internal;
    const GX2Surface *src = &windowdata->framebuffer;
    GX2Surface *dst = &output->color_buffer.surface;

    GX2SetContextState(output->context);
    if (src->width == dst->width && src->height == dst->height) {
        // The window is as big as the screen: this is the normal case
        GX2CopySurface(src, 0, 0, dst, 0, 0);
    } else {
        // The window is routed to a screen of another size: copy the top left part
        GX2Rect rect;
        GX2Point point;
        rect.left = 0;
        rect.top = 0;
        rect.right = (int32_t)SDL_min(src->width, dst->width);
        rect.bottom = (int32_t)SDL_min(src->height, dst->height);
        point.x = 0;
        point.y = 0;
        GX2CopySurfaceEx(src, 0, 0, dst, 0, 0, 1, &rect, &point);
    }
}

// Color buffers -> scan buffers on both screens, swap. Shared by the window
// framebuffer path and the GX2 renderer. The caller has already drawn the frame
// into the color buffers; the GPU executes in order, so nothing needs waiting for
// here except the previous swap. Both screens are copied every time: the scan
// buffers are double buffered, so a screen that is skipped would show an older frame.
void WIIU_PresentOutputs(SDL_VideoData *videodata)
{
    if (!videodata->has_foreground) {
        return;
    }
    WaitForPreviousSwap(videodata);
    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        WIIU_Output *output = &videodata->outputs[i];
        GX2SetContextState(output->context);
        WIIU_SWKBD_Draw(i); // the software keyboard, if it is up, over the window's pixels
        GX2CopyColorBufferToScanBuffer(&output->color_buffer, output->scan_target);
    }
    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    videodata->swap_pending = true;
}

static bool WIIU_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *videodata = _this->internal;
    SDL_WindowData *windowdata = window->internal;
    uint8_t *image;
    uint32_t row_bytes, y0, y1;

    if (!windowdata || !windowdata->has_framebuffer) {
        return SDL_SetError("The window has no surface");
    }
    if (!videodata->has_foreground) {
        // In the background there are no scan buffers. The frame is dropped; the
        // window shows its last content when the foreground is acquired again.
        return true;
    }

    // Flush what the CPU wrote so the GPU reads it. Only the rows that changed.
    image = (uint8_t *)windowdata->framebuffer.image;
    row_bytes = windowdata->framebuffer.pitch * 4;
    y0 = 0;
    y1 = windowdata->framebuffer.height;
    if (numrects > 0) {
        int top = (int)y1, bottom = 0;
        for (int i = 0; i < numrects; i++) {
            top = SDL_min(top, rects[i].y);
            bottom = SDL_max(bottom, rects[i].y + rects[i].h);
        }
        y0 = (uint32_t)SDL_clamp(top, 0, (int)y1);
        y1 = (uint32_t)SDL_clamp(bottom, (int)y0, (int)y1);
    }
    if (y1 > y0) {
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, image + (size_t)y0 * row_bytes, (y1 - y0) * row_bytes);
    }

    // Window framebuffer -> color buffer of each target screen
    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        if (windowdata->target & (1u << i)) {
            CopyWindowToOutput(window, &videodata->outputs[i]);
        }
    }

    // The CPU is going to draw into the framebuffer again as soon as we return,
    // so the GPU has to be finished reading it first. This copy is short. (This
    // also has to happen for prevent_swap windows.)
    GX2Flush();
    GX2WaitTimeStamp(GX2GetLastSubmittedTimeStamp());

    if (windowdata->prevent_swap) {
        return true; // the next window update that swaps will show this too
    }

    WIIU_PresentOutputs(videodata);
    return true;
}

static void DestroyFramebuffer(SDL_WindowData *windowdata)
{
    if (windowdata->has_framebuffer) {
        // The GPU may still be reading it
        GX2DrawDone();
        free(windowdata->framebuffer.image); // This should NOT be SDL_free()
        SDL_zero(windowdata->framebuffer);
        windowdata->has_framebuffer = false;
    }
}

static bool WIIU_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    SDL_WindowData *windowdata = window->internal;
    GX2Surface *surface;
    int w, h;

    DestroyFramebuffer(windowdata);

    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) {
        return SDL_SetError("Invalid window size");
    }

    // Linear and 32 bit, so the CPU can draw into it directly. The GPU converts
    // it to the tiled color buffer in WIIU_UpdateWindowFramebuffer().
    surface = &windowdata->framebuffer;
    SDL_zerop(surface);
    surface->dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface->width = (uint32_t)w;
    surface->height = (uint32_t)h;
    surface->depth = 1;
    surface->mipLevels = 1;
    surface->format = WIIU_SURFACE_FORMAT;
    surface->aa = GX2_AA_MODE1X;
    surface->use = GX2_SURFACE_USE_TEXTURE;
    surface->tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(surface);

    surface->image = memalign(surface->alignment, surface->imageSize); // This should NOT be SDL_memalign()
    if (!surface->image) {
        SDL_zerop(surface);
        return SDL_OutOfMemory();
    }
    SDL_memset(surface->image, 0, surface->imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, surface->image, surface->imageSize);
    windowdata->has_framebuffer = true;

    *format = WIIU_PIXEL_FORMAT;
    *pixels = surface->image;
    *pitch = (int)(surface->pitch * 4);
    return true;
}

static void WIIU_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windowdata = window->internal;
    if (windowdata) {
        DestroyFramebuffer(windowdata);
    }
}

/* ---------------------------------------------------------------------- */
// Displays

static bool WIIU_GetDisplayBounds(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_Rect *rect)
{
    SDL_VideoData *videodata = _this->internal;
    SDL_DisplayData *displaydata = display->internal;

    if (!displaydata) {
        return false;
    }
    // The GamePad sits to the right of the TV
    rect->x = (displaydata->output == WIIU_OUTPUT_DRC) ? (int)videodata->outputs[WIIU_OUTPUT_TV].width : 0;
    rect->y = 0;
    rect->w = display->current_mode->w;
    rect->h = display->current_mode->h;
    return true;
}

static bool WIIU_GetDisplayModes(SDL_VideoDevice *_this, SDL_VideoDisplay *display)
{
    // One mode per screen: the system's. Scaling to other sizes is done by the
    // GX2 renderer, not by changing the mode.
    return SDL_AddFullscreenDisplayMode(display, &display->desktop_mode);
}

static bool WIIU_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    return true;
}

static SDL_FullscreenResult WIIU_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
    // Windows are always fullscreen and always cover their screen
    return (fullscreen == SDL_FULLSCREEN_OP_LEAVE) ? SDL_FULLSCREEN_FAILED : SDL_FULLSCREEN_SUCCEEDED;
}

static uint32_t ParseTarget(const char *value, uint32_t fallback)
{
    if (!value || !*value) {
        return fallback;
    }
    if (SDL_strcasecmp(value, "tv") == 0) {
        return WIIU_TARGET_TV;
    }
    if (SDL_strcasecmp(value, "gamepad") == 0 || SDL_strcasecmp(value, "drc") == 0) {
        return WIIU_TARGET_DRC;
    }
    if (SDL_strcasecmp(value, "both") == 0) {
        return WIIU_TARGET_BOTH;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Unknown Wii U window target \"%s\" (use tv, gamepad or both)", value);
    return fallback;
}

static bool WIIU_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_DisplayData *displaydata = SDL_GetDisplayDriverDataForWindow(window);
    SDL_WindowData *windowdata;
    uint32_t target;

    windowdata = (SDL_WindowData *)SDL_calloc(1, sizeof(*windowdata));
    if (!windowdata) {
        return false;
    }

    // Window property, then hint, then the screen the window is on
    target = displaydata ? (1u << displaydata->output) : WIIU_TARGET_TV;
    target = ParseTarget(SDL_GetHint(SDL_HINT_WIIU_WINDOW_TARGET), target);
    target = ParseTarget(SDL_GetStringProperty(create_props, SDL_PROP_WINDOW_CREATE_WIIU_TARGET_STRING, NULL), target);
    windowdata->target = target;
    windowdata->prevent_swap = SDL_GetBooleanProperty(create_props, SDL_PROP_WINDOW_CREATE_WIIU_PREVENT_SWAP_BOOLEAN, false);

    window->internal = windowdata;
    SDL_SetKeyboardFocus(window);
    SDL_SetMouseFocus(window);
    return true;
}

static void WIIU_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *windowdata = window->internal;
    if (windowdata) {
        DestroyFramebuffer(windowdata);
        SDL_free(windowdata);
        window->internal = NULL;
    }
}

/* ---------------------------------------------------------------------- */
// Events

static void WIIU_PumpEvents(SDL_VideoDevice *_this)
{
    SDL_VideoData *videodata = _this->internal;

    // Touch and the software keyboard. Before the ProcUI handling below, which may return early
    // or block, and which is not changed by this.
    WIIU_PumpInput(_this);

    // If the application runs ProcUI itself, it is its job to call
    // ProcUIProcessMessages(); our callbacks still get called from there.
    if (!videodata->handle_procui) {
        return;
    }

    if (videodata->entering_background) {
        // The previous call saw RELEASE_FOREGROUND and sent WILL_ENTER_BACKGROUND.
        // The application has had a frame to process it; now give the foreground
        // up. This runs WIIU_ReleaseCallback(), which sends DID_ENTER_BACKGROUND.
        videodata->entering_background = false;
        ProcUIDrawDoneRelease();
    }

    if (s_home_denied) {
        // Homebrew Launcher: no HOME menu. Treat it as a request to quit.
        s_home_denied = false;
        if (!videodata->quit_sent) {
            videodata->quit_sent = true;
            SDL_SendQuit();
        }
    }

    if (s_procui_owned && ProcUIIsRunning() && !videodata->exiting_procui) {
        // TRUE: while in the background this blocks until the foreground is back
        // (and runs WIIU_AcquireCallback() when it is).
        switch (ProcUIProcessMessages(TRUE)) {
        case PROCUI_STATUS_RELEASE_FOREGROUND:
            // HOME pressed (or another system request)
            if (videodata->has_foreground) {
                SDL_OnApplicationWillEnterBackground();
                videodata->bg_will_sent = true;
                videodata->entering_background = true;
            }
            break;
        case PROCUI_STATUS_EXITING:
            // Same order as wut's WHBProcIsRunning(): ProcUIShutdown() as soon as
            // EXITING is seen, then the application cleans up and returns.
            videodata->exiting_procui = true;
            ProcUIShutdown();
            s_procui_owned = false;
            s_procui_exited = true;
            if (!videodata->quit_sent) {
                videodata->quit_sent = true;
                SDL_SendQuit();
            }
            break;
        case PROCUI_STATUS_IN_FOREGROUND:
        case PROCUI_STATUS_IN_BACKGROUND:
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------------- */
// Init and quit

static void InitOutputTV(WIIU_Output *output)
{
    uint32_t unused;

    switch (GX2GetSystemTVScanMode()) {
    case GX2_TV_SCAN_MODE_480I:
    case GX2_TV_SCAN_MODE_480P:
        if (GX2GetSystemTVAspectRatio() == GX2_ASPECT_RATIO_16_9) {
            output->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
            output->width = 854;
        } else {
            output->tv_render_mode = GX2_TV_RENDER_MODE_STANDARD_480P;
            output->width = 640;
        }
        output->height = 480;
        break;
    case GX2_TV_SCAN_MODE_1080I:
    case GX2_TV_SCAN_MODE_1080P:
        output->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_1080P;
        output->width = 1920;
        output->height = 1080;
        break;
    case GX2_TV_SCAN_MODE_576I:
        // PAL 50 Hz. wut renders this as 720p as well.
        output->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
        output->width = 1280;
        output->height = 720;
        break;
    case GX2_TV_SCAN_MODE_720P:
    default:
        output->tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
        output->width = 1280;
        output->height = 720;
        break;
    }
    output->refresh_rate = (GX2GetSystemTVScanMode() == GX2_TV_SCAN_MODE_576I) ? 50.0f : 60.0f;
    output->scan_target = GX2_SCAN_TARGET_TV;
    GX2CalcTVSize(output->tv_render_mode, WIIU_SURFACE_FORMAT, GX2_BUFFERING_MODE_DOUBLE,
                  &output->scan_buffer_size, &unused);
}

static void InitOutputDRC(WIIU_Output *output)
{
    uint32_t unused;

    output->drc_render_mode = GX2GetSystemDRCMode();
    output->width = DRC_SCREEN_WIDTH;
    output->height = DRC_SCREEN_HEIGHT;
    output->refresh_rate = 60.0f;
    output->scan_target = GX2_SCAN_TARGET_DRC;
    GX2CalcDRCSize(output->drc_render_mode, WIIU_SURFACE_FORMAT, GX2_BUFFERING_MODE_DOUBLE,
                   &output->scan_buffer_size, &unused);
}

// Describe the color buffer. Its memory comes later, in AcquireForeground().
static void InitColorBuffer(WIIU_Output *output)
{
    GX2ColorBuffer *cb = &output->color_buffer;

    SDL_zerop(cb);
    cb->surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    cb->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    cb->surface.width = output->width;
    cb->surface.height = output->height;
    cb->surface.depth = 1;
    cb->surface.mipLevels = 1;
    cb->surface.format = WIIU_SURFACE_FORMAT;
    cb->surface.aa = GX2_AA_MODE1X;
    cb->surface.tileMode = GX2_TILE_MODE_DEFAULT;
    cb->viewNumSlices = 1;
    GX2CalcSurfaceSizeAndAlignment(&cb->surface);
    GX2InitColorBufferRegs(cb);
}

static bool AddWiiUDisplay(SDL_VideoData *videodata, int index)
{
    WIIU_Output *output = &videodata->outputs[index];
    SDL_DisplayData *displaydata = (SDL_DisplayData *)SDL_calloc(1, sizeof(*displaydata));
    SDL_VideoDisplay display;
    SDL_DisplayMode mode;

    if (!displaydata) {
        return false;
    }
    displaydata->output = index;

    SDL_zero(mode);
    mode.format = WIIU_PIXEL_FORMAT;
    mode.w = (int)output->width;
    mode.h = (int)output->height;
    mode.refresh_rate = output->refresh_rate;

    SDL_zero(display);
    display.name = (index == WIIU_OUTPUT_TV) ? (char *)"Wii U TV" : (char *)"Wii U GamePad";
    display.desktop_mode = mode;
    display.internal = displaydata;

    if (!SDL_AddVideoDisplay(&display, false)) {
        SDL_free(displaydata);
        return false;
    }
    return true;
}

// Free everything VideoInit() set up. Safe to call after a partial init.
static void CleanupVideo(SDL_VideoData *videodata, bool shutdown_procui)
{
    if (videodata->gx2_initialized) {
        ReleaseForeground(videodata);
        GX2RSetAllocator(NULL, NULL);
        GX2Shutdown();
        videodata->gx2_initialized = false;
    }

    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        free(videodata->outputs[i].context); // This should NOT be SDL_free()
        videodata->outputs[i].context = NULL;
    }

    free(videodata->command_buffer_pool); // This should NOT be SDL_free()
    videodata->command_buffer_pool = NULL;

    if (videodata->handle_procui) {
        // A quit leaves ProcUI as it is (see WIIU_VideoQuit); only a failed init takes it down again
        if (shutdown_procui && s_procui_owned && videodata->procui_fresh) {
            ProcUIShutdown();
            s_procui_owned = false;
            if (s_from_hbl) {
                OSEnableHomeButtonMenu(TRUE);
            }
        }
        videodata->handle_procui = false;
    }

    s_videodata = NULL;
}

/* Walk ProcUI to its end: ask the HOME menu to take over, answer ProcUI until it
 * says EXITING, then ProcUIShutdown(). Without this the process can return to
 * a loading screen that never ends. Used by SDL_Quit() and, if the application
 * never got there, by the atexit() handler. */
static void WIIU_WalkProcUIToExit(void)
{
    if (!s_procui_owned || !ProcUIIsRunning()) {
        return;
    }
    if (!WIIU_IsCemu()) {
        bool running = true;
        SYSLaunchMenu();
        while (running) {
            switch (ProcUIProcessMessages(TRUE)) {
            case PROCUI_STATUS_RELEASE_FOREGROUND:
                ProcUIDrawDoneRelease();
                break;
            case PROCUI_STATUS_EXITING:
                running = false;
                break;
            default:
                break;
            }
        }
    }
    ProcUIShutdown();
    s_procui_owned = false;
    s_procui_exited = true;
}

// Homebrew Launcher: go back to it (wut's WHBProcShutdown does the same). Does not return.
static void WIIU_RelaunchIfHomebrew(void)
{
    if (s_from_hbl && s_procui_exited && !s_relaunched) {
        s_relaunched = true;
        SYSRelaunchTitle(0, NULL);
    }
}

static void WIIU_AtExit(void)
{
    /* The process is ending (main() returned or exit() was called). If SDL owns
       ProcUI and ProcUI has not said EXITING yet, walk it there: the HOME menu
       takes over, ProcUI is shut down, and with the Homebrew Launcher the title
       is relaunched. Skipping this leaves the console at a loading screen.
       Does nothing if the application runs ProcUI itself or already got there. */
    if (s_videodata) {
        s_videodata->quitting = true; // no lifecycle events while the process is ending
    }
    WIIU_WalkProcUIToExit();
    WIIU_RelaunchIfHomebrew();
}

/* SDL_Quit() and SDL_QuitSubSystem(SDL_INIT_VIDEO) free what this driver made
 * (GX2, scan buffers, windows) and nothing more. ProcUI stays initialized, so
 * SDL can be initialized again, and the process is not ended here: ending it
 * means walking ProcUI to EXITING, which is WIIU_AtExit()'s job, at the moment
 * the process really ends (the application returning from main(), calling
 * exit(), or ProcUI having said EXITING already). A program that goes on running
 * after SDL_Quit() without calling ProcUIProcessMessages() or SDL_Init() again
 * does not answer the HOME button. */
static void WIIU_VideoQuit(SDL_VideoDevice *_this)
{
    SDL_VideoData *videodata = _this->internal;

    videodata->quitting = true;
    WIIU_QuitInput(_this); // before GX2 is shut down
    CleanupVideo(videodata, false);
}

static bool WIIU_VideoInit(SDL_VideoDevice *_this)
{
    SDL_VideoData *videodata = _this->internal;
    uint32_t init_attribs[] = {
        GX2_INIT_CMD_BUF_BASE, 0,
        GX2_INIT_CMD_BUF_POOL_SIZE, GX2_COMMAND_BUFFER_SIZE,
        GX2_INIT_ARGC, 0,
        GX2_INIT_ARGV, 0,
        GX2_INIT_END
    };

    if (s_procui_exited) {
        // ProcUI was walked to its end: the process is on its way out.
        return SDL_SetError("The Wii U application is exiting; SDL cannot be initialized again");
    }

    s_videodata = videodata;
    videodata->swap_interval = 1;

    if (s_procui_owned && ProcUIIsRunning()) {
        // SDL started ProcUI in an earlier SDL_Init() and SDL_Quit() left it running
        videodata->handle_procui = true;
    } else if (!ProcUIIsRunning()) {
        // If the application already runs ProcUI, leave it to the application.
        s_from_hbl = WIIU_IsHomebrewLauncher();
        if (s_from_hbl) {
            // Like wut's WHBProcInit(): the Homebrew Launcher's host title cannot
            // use the HOME menu. This must come before ProcUIInitEx().
            OSEnableHomeButtonMenu(FALSE);
        }
        ProcUIInitEx(WIIU_SaveCallback, NULL);
        if (s_from_hbl) {
            ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, WIIU_HomeDeniedCallback, NULL, 100);
        }
        s_procui_owned = true;
        videodata->handle_procui = true;
        videodata->procui_fresh = true;
        if (!s_atexit_registered) {
            s_atexit_registered = true;
            atexit(WIIU_AtExit);
        }
    }

    videodata->command_buffer_pool = memalign(GX2_COMMAND_BUFFER_ALIGNMENT, GX2_COMMAND_BUFFER_SIZE); // This should NOT be SDL_memalign()
    if (!videodata->command_buffer_pool) {
        SDL_OutOfMemory();
        goto fail;
    }
    init_attribs[1] = (uint32_t)(uintptr_t)videodata->command_buffer_pool;
    GX2Init(init_attribs);
    videodata->gx2_initialized = true;

    InitOutputTV(&videodata->outputs[WIIU_OUTPUT_TV]);
    InitOutputDRC(&videodata->outputs[WIIU_OUTPUT_DRC]);
    InitColorBuffer(&videodata->outputs[WIIU_OUTPUT_TV]);
    InitColorBuffer(&videodata->outputs[WIIU_OUTPUT_DRC]);

    GX2RSetAllocator(&WIIU_GfxHeap_GX2RAlloc, &WIIU_GfxHeap_GX2RFree);

    if (!s_callbacks_registered) {
        ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, WIIU_AcquireCallback, NULL, 100);
        ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, WIIU_ReleaseCallback, NULL, 100);
        s_callbacks_registered = true;
    }

    // We are running, so we have the foreground
    if (!AcquireForeground(videodata)) {
        goto fail;
    }

    if (!AddWiiUDisplay(videodata, WIIU_OUTPUT_TV) || !AddWiiUDisplay(videodata, WIIU_OUTPUT_DRC)) {
        goto fail;
    }
    WIIU_InitInput(_this);
    return true;

fail:
    // Not the full quit: a failed init must not send the application to the HOME menu
    CleanupVideo(videodata, true);
    return false;
}

static void WIIU_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->internal);
    SDL_free(device);
}

static SDL_VideoDevice *WIIU_CreateDevice(void)
{
    SDL_VideoDevice *device;
    SDL_VideoData *videodata;

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(*device));
    if (!device) {
        return NULL;
    }
    videodata = (SDL_VideoData *)SDL_calloc(1, sizeof(*videodata));
    if (!videodata) {
        SDL_free(device);
        return NULL;
    }
    device->internal = videodata;

    device->VideoInit = WIIU_VideoInit;
    device->VideoQuit = WIIU_VideoQuit;
    device->GetDisplayBounds = WIIU_GetDisplayBounds;
    device->GetDisplayModes = WIIU_GetDisplayModes;
    device->SetDisplayMode = WIIU_SetDisplayMode;
    device->CreateSDLWindow = WIIU_CreateWindow;
    device->DestroyWindow = WIIU_DestroyWindow;
    device->SetWindowFullscreen = WIIU_SetWindowFullscreen;
    device->PumpEvents = WIIU_PumpEvents;
    device->CreateWindowFramebuffer = WIIU_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = WIIU_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = WIIU_DestroyWindowFramebuffer;
    device->SetWindowFramebufferVSync = WIIU_SetWindowFramebufferVSync;
    device->GetWindowFramebufferVSync = WIIU_GetWindowFramebufferVSync;
    device->HasScreenKeyboardSupport = WIIU_HasScreenKeyboardSupport;
    device->ShowScreenKeyboard = WIIU_ShowScreenKeyboard;
    device->HideScreenKeyboard = WIIU_HideScreenKeyboard;
    device->free = WIIU_DeleteDevice;

    device->device_caps = VIDEO_DEVICE_CAPS_FULLSCREEN_ONLY;

    return device;
}

VideoBootStrap WIIU_bootstrap = {
    WIIUVID_DRIVER_NAME, "Wii U video driver", WIIU_CreateDevice, NULL /* no ShowMessageBox implementation */, false
};

#endif // SDL_VIDEO_DRIVER_WIIU
