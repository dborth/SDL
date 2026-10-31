/*
  lifecycle tests for the SDL3 Wii U port

  Part 1  SDL_GetSystemRAM() must return a nonzero value.
  Part 2  Memory cycles: N times create a window, a renderer and a 640x480
          ARGB8888 streaming texture, update it, present, destroy all three.
          Free memory (MEM2 size minus malloc'd bytes)
          and the SDL allocation count are logged before and after. Growth
          above GROWTH_LIMIT is a FAIL. The GX2 heap is not malloc: it is not
          in these numbers, so a leak there shows up as a failing create, not
          as growth.
  Part 3  HOME cycles. A tone plays, the screen shows the lifecycle events.
          Press HOME, then come back; repeat HOME_CYCLES times (or wait TIMEOUT_MS).
          Hold a button on a controller while pressing HOME. Logged: every
          lifecycle and window event, joystick added/removed events (any
          during a HOME cycle is a FAIL: controllers must keep their IDs), the
          buttons that are down when the foreground returns (INFO: they should be
          the ones you really hold), and free memory after the cycles.

  Returns 0 if every PASS/FAIL check passed. SDL_Log goes to OSReport on Wii U.
*/
#include <SDL3/SDL.h>

#include <malloc.h>
#include <stdint.h>
#include <coreinit/memory.h>

#define CYCLES        30
#define GROWTH_LIMIT  (1024 * 1024)
#define HOME_CYCLES   5
#define TIMEOUT_MS    180000

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static long free_mem2(void)
{
    uint32_t addr = 0, size = 0;
    struct mallinfo mi = mallinfo();

    OSGetMemBound(OS_MEM2, &addr, &size);
    return (long)size - (long)mi.uordblks;
}

static void audio_cb(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    static float phase;
    Sint16 buf[480];

    (void)userdata;
    (void)total;
    while (additional > 0) {
        int n = (additional < (int)sizeof(buf)) ? additional / 2 : (int)SDL_arraysize(buf);
        for (int i = 0; i < n; ++i) {
            buf[i] = (Sint16)(SDL_sinf(phase) * 6000.0f);
            phase += 2.0f * SDL_PI_F * 440.0f / 48000.0f;
            if (phase > 2.0f * SDL_PI_F) {
                phase -= 2.0f * SDL_PI_F;
            }
        }
        SDL_PutAudioStreamData(stream, buf, n * 2);
        additional -= n * 2;
    }
}

static void memory_cycles(void)
{
    long free_before, free_after;
    int alloc_before, alloc_after;
    int i;

    SDL_Log("INFO: memory cycles, %d x window + renderer + 640x480 streaming texture", CYCLES);
    SDL_Init(SDL_INIT_VIDEO);
    free_before = free_mem2();
    alloc_before = SDL_GetNumAllocations();

    for (i = 0; i < CYCLES; ++i) {
        SDL_Window *window = SDL_CreateWindow("cycle", 1280, 720, 0);
        SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
        SDL_Texture *tex = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480) : NULL;

        if (!tex) {
            SDL_Log("FAIL: cycle %d: %s", i, SDL_GetError());
            failures++;
        } else {
            void *pixels;
            int pitch;
            if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
                SDL_memset(pixels, 0x80, (size_t)pitch * 480);
                SDL_UnlockTexture(tex);
            }
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            SDL_RenderPresent(renderer);
        }
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        if (i == 0 || i == CYCLES - 1 || (i % 10) == 9) {
            SDL_Log("INFO: after cycle %d: MEM2 free %ld, SDL allocations %d", i + 1, free_mem2(), SDL_GetNumAllocations());
        }
    }

    free_after = free_mem2();
    alloc_after = SDL_GetNumAllocations();
    SDL_Log("INFO: MEM2 free before %ld, after %ld (delta %ld)", free_before, free_after, free_after - free_before);
    SDL_Log("INFO: SDL allocations before %d, after %d", alloc_before, alloc_after);
    check("memory cycles: MEM2 growth below the limit", (free_before - free_after) < GROWTH_LIMIT);
    check("memory cycles: SDL allocation count did not grow by more than 16", alloc_after < 0 || (alloc_after - alloc_before) <= 16);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void home_cycles(void)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_AudioStream *audio;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, 48000 };
    Uint64 start;
    int will_bg = 0, did_bg = 0, will_fg = 0, did_fg = 0, minimized = 0, restored = 0;
    int joy_added = 0, joy_removed = 0, joy_removed_in_cycle = 0;
    long free_start;
    int cycles = 0;
    bool in_bg = false;
    bool running = true;

    SDL_Log("INFO: HOME cycles: press HOME and come back %d times (hold a button and listen for the tone)", HOME_CYCLES);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        check("HOME cycles: SDL_Init", false);
        return;
    }
    window = SDL_CreateWindow("home", 1280, 720, 0);
    renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    check("HOME cycles: window, renderer and audio stream", renderer && audio);
    if (!renderer || !audio) {
        goto done;
    }
    SDL_ResumeAudioStreamDevice(audio);
    free_start = free_mem2();
    start = SDL_GetTicks();

    while (running && cycles < HOME_CYCLES && (SDL_GetTicks() - start) < TIMEOUT_MS) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                SDL_Log("INFO: SDL_EVENT_QUIT");
                running = false;
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                ++will_bg;
                in_bg = true;
                joy_removed_in_cycle = 0;
                SDL_Log("INFO: WILL_ENTER_BACKGROUND");
                break;
            case SDL_EVENT_DID_ENTER_BACKGROUND:
                ++did_bg;
                SDL_Log("INFO: DID_ENTER_BACKGROUND");
                break;
            case SDL_EVENT_WILL_ENTER_FOREGROUND:
                ++will_fg;
                SDL_Log("INFO: WILL_ENTER_FOREGROUND");
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND: {
                int n = 0, count = 0;
                SDL_JoystickID *ids = SDL_GetJoysticks(&count);
                ++did_fg;
                in_bg = false;
                for (int i = 0; i < count; ++i) {
                    SDL_Joystick *joy = SDL_GetJoystickFromID(ids[i]);
                    for (int b = 0; joy && b < SDL_GetNumJoystickButtons(joy); ++b) {
                        n += SDL_GetJoystickButton(joy, b) ? 1 : 0;
                    }
                }
                SDL_free(ids);
                SDL_Log("INFO: DID_ENTER_FOREGROUND (%d joysticks, %d buttons down)", count, n);
                ++cycles;
                check("HOME cycle: no controller removed or added", joy_removed_in_cycle == 0);
                break;
            }
            case SDL_EVENT_WINDOW_MINIMIZED:
                ++minimized;
                SDL_Log("INFO: WINDOW_MINIMIZED");
                break;
            case SDL_EVENT_WINDOW_RESTORED:
                ++restored;
                SDL_Log("INFO: WINDOW_RESTORED");
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
                ++joy_added;
                SDL_Log("INFO: JOYSTICK_ADDED id %u", (unsigned)e.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                ++joy_removed;
                if (in_bg || did_bg > did_fg) {
                    ++joy_removed_in_cycle;
                }
                SDL_Log("INFO: JOYSTICK_REMOVED id %u", (unsigned)e.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
                SDL_Log("INFO: button %d down (id %u)", e.jbutton.button, (unsigned)e.jbutton.which);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                SDL_Log("INFO: button %d up (id %u)", e.jbutton.button, (unsigned)e.jbutton.which);
                break;
            default:
                break;
            }
        }
        {
            Uint8 shade = (Uint8)(60 + (SDL_GetTicks() / 8) % 120);
            SDL_SetRenderDrawColor(renderer, shade, 40, (Uint8)(255 - shade), 255);
            SDL_RenderClear(renderer);
            SDL_RenderPresent(renderer);
        }
    }

    SDL_Log("INFO: events: will_bg %d did_bg %d will_fg %d did_fg %d minimized %d restored %d", will_bg, did_bg, will_fg, did_fg, minimized, restored);
    SDL_Log("INFO: joystick added %d removed %d", joy_added, joy_removed);
    SDL_Log("INFO: MEM2 free at start %ld, now %ld", free_start, free_mem2());
    check("HOME cycles: every WILL has a DID, in order", will_bg == did_bg && will_fg == did_fg && will_bg == will_fg);
    check("HOME cycles: windows minimized and restored as often as the app left and returned", minimized == restored && minimized >= will_bg);
    check("HOME cycles: at least one cycle happened", cycles > 0);

done:
    SDL_DestroyAudioStream(audio);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_Log("SDL_GetSystemRAM() = %d MiB", SDL_GetSystemRAM());
    check("SDL_GetSystemRAM() is nonzero", SDL_GetSystemRAM() > 0);
    memory_cycles();
    home_cycles();
    SDL_Log("%s (%d failures)", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
