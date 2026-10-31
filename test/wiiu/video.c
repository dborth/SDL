/*
  video test for the SDL3 Wii U port

  Runs four phases of about 5 seconds each. Each draws a clear color, a bouncing
  box and a field of small sprites, and logs the frame rate (SDL_Log goes to
  OSReport on Wii U):

    1. window on the TV display
    2. window on the GamePad display
    3. TV window routed to both screens (the GamePad shows the top left part)
    4. two windows, a TV one with prevent_swap and a GamePad one, flipping together

  The property and hint names below are the literal strings of the Wii U driver
  (src/video/wiiu/SDL_wiiuvideo.h is private).

  Lines starting with "INFO" are for the reader. PASS/FAIL lines are checks.
  Returns 0 if every check passed.
*/
#include <SDL3/SDL.h>

#define PROP_TARGET       "SDL.window.create.wiiu.target"
#define PROP_PREVENT_SWAP "SDL.window.create.wiiu.prevent_swap"

#define PHASE_MS 5000
#define NUM_SPRITES 120

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
    int w, h;
    float x, y, dx, dy;
    Uint8 r, g, b;
} Win;

static bool open_win(Win *win, SDL_DisplayID display, const char *title, const char *target, bool prevent_swap, Uint8 r, Uint8 g, Uint8 b)
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

    win->renderer = SDL_CreateRenderer(win->window, "software");
    if (!win->renderer) {
        SDL_Log("FAIL: SDL_CreateRenderer(software) for %s: %s", title, SDL_GetError());
        return false;
    }
    SDL_GetWindowSizeInPixels(win->window, &win->w, &win->h);
    SDL_Log("INFO %s: window %dx%d, renderer \"%s\"", title, win->w, win->h, SDL_GetRendererName(win->renderer));

    // A 16x16 sprite with a visible pattern, as a software texture
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
    if (win->renderer) {
        SDL_DestroyRenderer(win->renderer);
    }
    if (win->window) {
        SDL_DestroyWindow(win->window);
    }
    SDL_zerop(win);
}

static void draw_win(Win *win, Uint64 frame)
{
    SDL_FRect box;

    SDL_SetRenderDrawColor(win->renderer, win->r, win->g, win->b, 255);
    SDL_RenderClear(win->renderer);

    // Frame border: shows whether the whole window reaches the screen
    SDL_SetRenderDrawColor(win->renderer, 255, 255, 255, 255);
    box.x = 0.0f;
    box.y = 0.0f;
    box.w = (float)win->w;
    box.h = 4.0f;
    SDL_RenderFillRect(win->renderer, &box);
    box.y = (float)win->h - 4.0f;
    SDL_RenderFillRect(win->renderer, &box);
    box.y = 0.0f;
    box.w = 4.0f;
    box.h = (float)win->h;
    SDL_RenderFillRect(win->renderer, &box);
    box.x = (float)win->w - 4.0f;
    SDL_RenderFillRect(win->renderer, &box);

    // Sprites, a rough stand-in for a 2D game's blit load
    if (win->sprite) {
        for (int i = 0; i < NUM_SPRITES; i++) {
            SDL_FRect dst;
            dst.x = (float)((i * 37 + (int)frame * 2) % (win->w - 16));
            dst.y = (float)((i * 53 + (int)frame) % (win->h - 16));
            dst.w = 16.0f;
            dst.h = 16.0f;
            SDL_RenderTexture(win->renderer, win->sprite, NULL, &dst);
        }
    }

    // The bouncing box
    win->x += win->dx;
    win->y += win->dy;
    if (win->x < 0.0f || win->x > (float)win->w - 64.0f) {
        win->dx = -win->dx;
        win->x += win->dx;
    }
    if (win->y < 0.0f || win->y > (float)win->h - 64.0f) {
        win->dy = -win->dy;
        win->y += win->dy;
    }
    SDL_SetRenderDrawColor(win->renderer, 255, 220, 0, 255);
    box.x = win->x;
    box.y = win->y;
    box.w = 64.0f;
    box.h = 64.0f;
    SDL_RenderFillRect(win->renderer, &box);
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
    SDL_Log("INFO video driver: %s", SDL_GetCurrentVideoDriver());
    check("video driver is wiiu", SDL_strcmp(SDL_GetCurrentVideoDriver(), "wiiu") == 0);

    displays = SDL_GetDisplays(&num_displays);
    check("two displays (TV, GamePad)", displays && num_displays == 2);
    if (!displays || num_displays != 2) {
        SDL_Quit();
        return 1;
    }
    tv = displays[0];
    gamepad = displays[1];
    SDL_free(displays);

    for (int i = 0; i < 2; i++) {
        const SDL_DisplayID id = i ? gamepad : tv;
        SDL_Rect bounds;
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(id);
        SDL_GetDisplayBounds(id, &bounds);
        SDL_Log("INFO display %d \"%s\": mode %dx%d @ %.1f Hz, bounds %d,%d %dx%d",
                i, SDL_GetDisplayName(id), mode ? mode->w : 0, mode ? mode->h : 0,
                mode ? (double)mode->refresh_rate : 0.0, bounds.x, bounds.y, bounds.w, bounds.h);
    }
    check("primary display is the TV", SDL_GetPrimaryDisplay() == tv);

    // 1. TV
    if (open_win(&a, tv, "tv", "tv", false, 20, 20, 120)) {
        wins[0] = &a;
        if (!run_phase("TV window", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 2. GamePad
    if (open_win(&a, gamepad, "gamepad", "gamepad", false, 20, 100, 20)) {
        wins[0] = &a;
        if (!run_phase("GamePad window", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 3. TV window on both screens
    if (open_win(&a, tv, "both", "both", false, 100, 20, 20)) {
        wins[0] = &a;
        if (!run_phase("TV window routed to both", wins, 1)) {
            goto done;
        }
    }
    close_win(&a);

    // 4. one window per screen, swapped together
    if (open_win(&a, tv, "tv2", "tv", true, 20, 20, 120) &&
        open_win(&b, gamepad, "gamepad2", "gamepad", false, 20, 100, 20)) {
        wins[0] = &a;
        wins[1] = &b;
        run_phase("two windows, prevent_swap on the TV one", wins, 2);
    }
    close_win(&a);
    close_win(&b);

done:
    close_win(&a);
    close_win(&b);
    SDL_Log("%s (%d failures)", failures ? "FAILED" : "ALL CHECKS PASSED", failures);
    SDL_Quit();
    return failures ? 1 : 0;
}
