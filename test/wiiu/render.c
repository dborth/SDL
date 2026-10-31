/*
  GX2 renderer test for the SDL3 Wii U port

  Phases of about 5 seconds each, every frame drawn with a 640x480 logical
  presentation (so the renderer scales it to the screen, which is the point of
  the GX2 renderer) and a mix of draw calls: clear, border, 120 sprites, a
  streaming texture updated every frame, an alpha blended rectangle, a render
  target copied back to the screen, and a bouncing box. The frame rate is logged
  (SDL_Log goes to OSReport on Wii U):

    1. gx2 renderer, TV window
    2. gx2 renderer, GamePad window
    3. gx2 renderer, TV window routed to both screens (scaled to the GamePad)
    4. gx2 renderer, two windows, TV one with prevent_swap, flipping together
    5. software renderer, TV window, same load: what the GX2 renderer replaces

  Before each GX2 phase a check frame draws known colors with the logical
  presentation off and reads them back with SDL_RenderReadPixels(): window
  target, render target, alpha blending and a texture. PASS/FAIL lines are those
  checks; "INFO" lines are for the reader. The window read back goes through
  GX2CopySurface (tiled color buffer to linear), so a FAIL there may also mean
  that assumption does not hold. Returns 0 if every check passed. Look at the
  screens too: the checks cannot see the GamePad.

  The property and hint names are the literal strings of the Wii U video driver.
*/
#include <SDL3/SDL.h>

#define PROP_TARGET       "SDL.window.create.wiiu.target"
#define PROP_PREVENT_SWAP "SDL.window.create.wiiu.prevent_swap"

#define PHASE_MS 5000
#define NUM_SPRITES 120
#define LOGICAL_W 640
#define LOGICAL_H 480

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

typedef struct
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *sprite;
    SDL_Texture *stream;
    SDL_Texture *target;
    int w, h;
    float x, y, dx, dy;
    Uint8 r, g, b;
} Win;

static bool open_win(Win *win, SDL_DisplayID display, const char *title, const char *renderer_name,
                     const char *target, bool prevent_swap, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Surface *pattern;

    SDL_zerop(win);
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_UNDEFINED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_UNDEFINED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 640);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 480);
    if (target) {
        SDL_SetStringProperty(props, PROP_TARGET, target);
    }
    if (prevent_swap) {
        SDL_SetBooleanProperty(props, PROP_PREVENT_SWAP, true);
    }
    win->window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!win->window) {
        SDL_Log("FAIL: SDL_CreateWindowWithProperties(%s): %s", title, SDL_GetError());
        return false;
    }

    win->renderer = SDL_CreateRenderer(win->window, renderer_name);
    if (!win->renderer) {
        SDL_Log("FAIL: SDL_CreateRenderer(%s) for %s: %s", renderer_name ? renderer_name : "default", title, SDL_GetError());
        return false;
    }
    SDL_GetWindowSizeInPixels(win->window, &win->w, &win->h);
    SDL_Log("INFO %s: window %dx%d, renderer \"%s\"", title, win->w, win->h, SDL_GetRendererName(win->renderer));
    SDL_SetRenderVSync(win->renderer, 1);

    // A 16x16 sprite with a visible pattern
    pattern = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_RGBA8888);
    if (pattern) {
        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 16; px++) {
                const bool on = ((px / 4) + (py / 4)) & 1;
                SDL_WriteSurfacePixel(pattern, px, py, on ? 255 : 40, on ? 255 : 40, 255, 255);
            }
        }
        win->sprite = SDL_CreateTextureFromSurface(win->renderer, pattern);
        SDL_DestroySurface(pattern);
    }
    win->stream = SDL_CreateTexture(win->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 64, 64);
    win->target = SDL_CreateTexture(win->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 128, 128);
    if (!win->sprite || !win->stream || !win->target) {
        SDL_Log("FAIL: creating textures for %s: %s", title, SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(win->sprite, SDL_SCALEMODE_NEAREST);

    win->x = 20.0f;
    win->y = 20.0f;
    win->dx = 5.0f;
    win->dy = 3.0f;
    win->r = r;
    win->g = g;
    win->b = b;
    return true;
}

static void close_win(Win *win)
{
    if (win->sprite) {
        SDL_DestroyTexture(win->sprite);
    }
    if (win->stream) {
        SDL_DestroyTexture(win->stream);
    }
    if (win->target) {
        SDL_DestroyTexture(win->target);
    }
    if (win->renderer) {
        SDL_DestroyRenderer(win->renderer);
    }
    if (win->window) {
        SDL_DestroyWindow(win->window);
    }
    SDL_zerop(win);
}

static bool near_color(SDL_Surface *surface, int x, int y, int r, int g, int b, int tolerance)
{
    Uint8 pr = 0, pg = 0, pb = 0, pa = 0;
    if (!SDL_ReadSurfacePixel(surface, x, y, &pr, &pg, &pb, &pa)) {
        return false;
    }
    SDL_Log("INFO   pixel (%d,%d) = %d,%d,%d,%d, wanted about %d,%d,%d", x, y, pr, pg, pb, pa, r, g, b);
    return SDL_abs(pr - r) <= tolerance && SDL_abs(pg - g) <= tolerance && SDL_abs(pb - b) <= tolerance;
}

// Known colors, logical presentation off, read back and compared
static void check_frame(Win *win, const char *name)
{
    SDL_Renderer *renderer = win->renderer;
    SDL_FRect box;
    SDL_Surface *surface;
    char text[128];

    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    // Window: blue background, opaque red box, half transparent white box over blue
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    box.x = 100.0f;
    box.y = 100.0f;
    box.w = 50.0f;
    box.h = 50.0f;
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 128);
    box.x = 200.0f;
    SDL_RenderFillRect(renderer, &box);

    surface = SDL_RenderReadPixels(renderer, NULL);
    SDL_snprintf(text, sizeof(text), "%s: ReadPixels of the window works", name);
    check(text, surface != NULL);
    if (surface) {
        SDL_snprintf(text, sizeof(text), "%s: clear color", name);
        check(text, near_color(surface, 10, 10, 0, 0, 255, 3));
        SDL_snprintf(text, sizeof(text), "%s: opaque fill", name);
        check(text, near_color(surface, 125, 125, 255, 0, 0, 3));
        SDL_snprintf(text, sizeof(text), "%s: alpha blend (white 50%% over blue)", name);
        check(text, near_color(surface, 225, 125, 128, 128, 255, 4));
        SDL_DestroySurface(surface);
    }

    // Render target: clear it green, read the target back
    SDL_SetRenderTarget(renderer, win->target);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    SDL_RenderClear(renderer);
    surface = SDL_RenderReadPixels(renderer, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_snprintf(text, sizeof(text), "%s: render target readback", name);
    check(text, surface && near_color(surface, 64, 64, 0, 255, 0, 3));
    if (surface) {
        SDL_DestroySurface(surface);
    }

    // The target as a texture, drawn into the window, then the window read back
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    box.x = 300.0f;
    box.y = 300.0f;
    box.w = 128.0f;
    box.h = 128.0f;
    SDL_RenderTexture(renderer, win->target, NULL, &box);
    surface = SDL_RenderReadPixels(renderer, NULL);
    SDL_snprintf(text, sizeof(text), "%s: render target drawn to the window", name);
    check(text, surface && near_color(surface, 360, 360, 0, 255, 0, 3) && near_color(surface, 10, 10, 0, 0, 0, 3));
    if (surface) {
        SDL_DestroySurface(surface);
    }

    // Not shown: the next frame will draw over it
    SDL_SetRenderLogicalPresentation(renderer, LOGICAL_W, LOGICAL_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static void draw_win(Win *win, Uint64 frame)
{
    SDL_Renderer *renderer = win->renderer;
    SDL_FRect box;
    void *pixels;
    int pitch;

    // Streaming texture: a moving gradient
    if (SDL_LockTexture(win->stream, NULL, &pixels, &pitch)) {
        for (int py = 0; py < 64; py++) {
            Uint32 *row = (Uint32 *)((Uint8 *)pixels + (size_t)py * pitch);
            for (int px = 0; px < 64; px++) {
                const Uint32 r = (Uint32)((px * 4 + frame * 3) & 255);
                const Uint32 g = (Uint32)(py * 4);
                row[px] = (r << 24) | (g << 16) | (128u << 8) | 255u; // RGBA8888
            }
        }
        SDL_UnlockTexture(win->stream);
    }

    // Render target: a small scene drawn into it each frame
    SDL_SetRenderTarget(renderer, win->target);
    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255);
    box.x = (float)((frame * 2) % 100);
    box.y = 20.0f;
    box.w = 28.0f;
    box.h = 28.0f;
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderTarget(renderer, NULL);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, win->r, win->g, win->b, 255);
    SDL_RenderClear(renderer);

    // Frame border in logical coordinates
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    box.x = 0.0f;
    box.y = 0.0f;
    box.w = (float)LOGICAL_W;
    box.h = 4.0f;
    SDL_RenderFillRect(renderer, &box);
    box.y = (float)LOGICAL_H - 4.0f;
    SDL_RenderFillRect(renderer, &box);
    box.y = 0.0f;
    box.w = 4.0f;
    box.h = (float)LOGICAL_H;
    SDL_RenderFillRect(renderer, &box);
    box.x = (float)LOGICAL_W - 4.0f;
    SDL_RenderFillRect(renderer, &box);

    // Sprites
    for (int i = 0; i < NUM_SPRITES; i++) {
        SDL_FRect dst;
        dst.x = (float)((i * 37 + (int)frame * 2) % (LOGICAL_W - 16));
        dst.y = (float)((i * 53 + (int)frame) % (LOGICAL_H - 16));
        dst.w = 16.0f;
        dst.h = 16.0f;
        SDL_RenderTexture(renderer, win->sprite, NULL, &dst);
    }

    // The streaming texture and the render target
    box.x = 400.0f;
    box.y = 40.0f;
    box.w = 128.0f;
    box.h = 128.0f;
    SDL_RenderTexture(renderer, win->stream, NULL, &box);
    box.x = 40.0f;
    box.y = 300.0f;
    box.w = 128.0f;
    box.h = 128.0f;
    SDL_RenderTexture(renderer, win->target, NULL, &box);

    // Alpha blending, a line and a point
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 128);
    box.x = 180.0f;
    box.y = 180.0f;
    box.w = 280.0f;
    box.h = 120.0f;
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 0, 255, 255, 255);
    SDL_RenderLine(renderer, 190.0f, 190.0f, 450.0f, 290.0f);
    SDL_RenderPoint(renderer, 320.0f, 240.0f);

    // The bouncing box
    win->x += win->dx;
    win->y += win->dy;
    if (win->x < 0.0f || win->x > (float)LOGICAL_W - 64.0f) {
        win->dx = -win->dx;
        win->x += win->dx;
    }
    if (win->y < 0.0f || win->y > (float)LOGICAL_H - 64.0f) {
        win->dy = -win->dy;
        win->y += win->dy;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 255, 220, 0, 255);
    box.x = win->x;
    box.y = win->y;
    box.w = 64.0f;
    box.h = 64.0f;
    SDL_RenderFillRect(renderer, &box);
}

// Run for PHASE_MS, or until quit. Windows are presented in array order.
static bool run_phase(const char *name, Win **wins, int count)
{
    Uint64 start = SDL_GetTicksNS();
    Uint64 frames = 0;
    Uint64 worst = 0;
    bool quit = false;

    SDL_Log("INFO phase \"%s\" for %d seconds", name, PHASE_MS / 1000);
    while (!quit && (SDL_GetTicksNS() - start) < (Uint64)PHASE_MS * SDL_NS_PER_MS) {
        SDL_Event event;
        Uint64 frame_start = SDL_GetTicksNS();

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            }
        }
        for (int i = 0; i < count; i++) {
            draw_win(wins[i], frames);
            SDL_RenderPresent(wins[i]->renderer);
        }
        frames++;
        worst = SDL_max(worst, SDL_GetTicksNS() - frame_start);
    }

    {
        const Uint64 elapsed_ms = (SDL_GetTicksNS() - start) / SDL_NS_PER_MS;
        SDL_Log("INFO %s: %llu frames in %llu ms = %.1f fps, worst frame %.1f ms",
                name, (unsigned long long)frames, (unsigned long long)elapsed_ms,
                elapsed_ms ? (double)frames * 1000.0 / (double)elapsed_ms : 0.0,
                (double)worst / (double)SDL_NS_PER_MS);
        check(name, frames > 10);
    }
    return !quit;
}

int main(int argc, char *argv[])
{
    SDL_DisplayID *displays;
    int num_displays = 0;
    SDL_DisplayID tv, gamepad;
    Win a = { 0 }, b = { 0 };
    Win *wins[2];

    (void)argc;
    (void)argv;

    SDL_Log("SDL %d.%d.%d on %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetPlatform());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("FAIL: SDL_Init(SDL_INIT_VIDEO): %s", SDL_GetError());
        return 1;
    }
    check("video driver is wiiu", SDL_strcmp(SDL_GetCurrentVideoDriver(), "wiiu") == 0);
    {
        bool have_gx2 = false;
        for (int i = 0; i < SDL_GetNumRenderDrivers(); i++) {
            const char *name = SDL_GetRenderDriver(i);
            SDL_Log("INFO render driver %d: %s", i, name);
            if (SDL_strcmp(name, "gx2") == 0) {
                have_gx2 = true;
            }
        }
        check("gx2 render driver is listed", have_gx2);
    }

    displays = SDL_GetDisplays(&num_displays);
    check("two displays (TV, GamePad)", displays && num_displays == 2);
    if (!displays || num_displays != 2) {
        SDL_Quit();
        return 1;
    }
    tv = displays[0];
    gamepad = displays[1];
    SDL_free(displays);

    // 1. TV
    if (open_win(&a, tv, "tv", NULL, "tv", false, 20, 20, 120)) {
        check("default renderer is gx2", SDL_strcmp(SDL_GetRendererName(a.renderer), "gx2") == 0);
        check_frame(&a, "TV");
        wins[0] = &a;
        if (!run_phase("gx2, TV window", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 2. GamePad
    if (open_win(&a, gamepad, "gamepad", "gx2", "gamepad", false, 20, 100, 20)) {
        check_frame(&a, "GamePad");
        wins[0] = &a;
        if (!run_phase("gx2, GamePad window", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 3. TV window on both screens
    if (open_win(&a, tv, "both", "gx2", "both", false, 100, 20, 20)) {
        wins[0] = &a;
        if (!run_phase("gx2, TV window routed to both", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 4. one window per screen, swapped together
    if (open_win(&a, tv, "tv2", "gx2", "tv", true, 20, 20, 120) &&
        open_win(&b, gamepad, "gamepad2", "gx2", "gamepad", false, 20, 100, 20)) {
        wins[0] = &a;
        wins[1] = &b;
        if (!run_phase("gx2, two windows, prevent_swap on the TV one", wins, 2)) {
            close_win(&a);
            close_win(&b);
            goto done;
        }
    }
    close_win(&a);
    close_win(&b);

    // 5. the same load on the software renderer, for comparison
    if (open_win(&a, tv, "tv-sw", "software", "tv", false, 20, 20, 120)) {
        wins[0] = &a;
        run_phase("software, TV window (same load)", wins, 1);
    }
    close_win(&a);

done:
    close_win(&a);
    close_win(&b);
    SDL_Log("%s (%d failures)", failures ? "FAILED" : "ALL CHECKS PASSED", failures);
    SDL_Quit();
    return failures ? 1 : 0;
}
