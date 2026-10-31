/*
  USB keyboard and USB mouse tests for the SDL3 Wii U port

  It opens a window on the TV and draws with the window surface only. Nothing is
  written on screen as text; the log (SDL_Log, OSReport on Wii U) tells you what
  happened. Start it with the devices already plugged in, then also try
  unplugging and plugging them in while it runs.

    - Keyboard: every key logs a KEY_DOWN / KEY_UP line with the key name and
      scancode; a held key logs more KEY_DOWN lines with repeat=1. Printable keys
      also log TEXT_INPUT. Check shifted and AltGr characters on your layout.
      Hold Shift, Ctrl and Alt and check the modifier bits in the log.
      Ctrl+letter and Alt+letter must log no TEXT_INPUT.
    - Mouse: a square follows the pointer, which starts in the middle of the
      screen and stops at the edges. It is white, red while the left button is
      down, blue for right and green for middle. The log shows MOUSE_MOTION
      (position and relative motion), MOUSE_BUTTON and MOUSE_WHEEL (turn the
      wheel both ways: up must be positive y).
    - MOUSE_ADDED / MOUSE_REMOVED / KEYBOARD_ADDED / KEYBOARD_REMOVED log on
      hot-plug. A key or button held while the device is unplugged must be
      released (the log shows the KEY_UP or BUTTON_UP).
    - Press HOME and come back: nothing may be stuck down, and what was typed
      in the HOME menu must not show up.
    - Escape quits (so does the HOME menu's exit).

  The on-screen keyboard is switched off here (SDL_HINT_ENABLE_SCREEN_KEYBOARD),
  so text input starts whether or not a USB keyboard is attached. Without the
  hint, SDL starts the on-screen keyboard when there is no keyboard.
*/
#include <SDL3/SDL.h>

#define SQUARE 24

static void log_mods(SDL_Keymod mod, char *out, size_t size)
{
    SDL_snprintf(out, size, "%s%s%s%s%s%s",
                 (mod & SDL_KMOD_SHIFT) ? "shift " : "",
                 (mod & SDL_KMOD_CTRL) ? "ctrl " : "",
                 (mod & SDL_KMOD_LALT) ? "lalt " : "",
                 (mod & SDL_KMOD_RALT) ? "ralt " : "",
                 (mod & SDL_KMOD_GUI) ? "gui " : "",
                 (mod & SDL_KMOD_CAPS) ? "caps " : "");
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Surface *surface;
    bool running = true;
    int quit_code = 0;

    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("usb", 1280, 720, 0);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_StartTextInput(window);
    SDL_Log("keyboard present: %d, mouse present: %d", SDL_HasKeyboard(), SDL_HasMouse());

    while (running) {
        SDL_Event event;
        float mx, my;
        SDL_MouseButtonFlags buttons;
        char mods[48];
        SDL_Rect square;
        Uint8 r = 255, g = 255, b = 255;

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEYBOARD_ADDED:
                SDL_Log("KEYBOARD_ADDED id=%u \"%s\"", (unsigned)event.kdevice.which, SDL_GetKeyboardNameForID(event.kdevice.which));
                break;
            case SDL_EVENT_KEYBOARD_REMOVED:
                SDL_Log("KEYBOARD_REMOVED id=%u", (unsigned)event.kdevice.which);
                break;
            case SDL_EVENT_MOUSE_ADDED:
                SDL_Log("MOUSE_ADDED id=%u \"%s\"", (unsigned)event.mdevice.which, SDL_GetMouseNameForID(event.mdevice.which));
                break;
            case SDL_EVENT_MOUSE_REMOVED:
                SDL_Log("MOUSE_REMOVED id=%u", (unsigned)event.mdevice.which);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                log_mods(event.key.mod, mods, sizeof(mods));
                SDL_Log("KEY_%s %s scancode=%d raw=%d repeat=%d mods=[%s]",
                        event.key.down ? "DOWN" : "UP", SDL_GetKeyName(event.key.key),
                        (int)event.key.scancode, (int)event.key.raw, event.key.repeat, mods);
                if (event.key.down && event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                break;
            case SDL_EVENT_TEXT_INPUT:
                SDL_Log("TEXT_INPUT \"%s\"", event.text.text);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                SDL_Log("MOUSE_MOTION x=%.0f y=%.0f xrel=%.0f yrel=%.0f", event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                SDL_Log("MOUSE_BUTTON_%s button=%d clicks=%d at %.0f,%.0f", event.button.down ? "DOWN" : "UP",
                        event.button.button, event.button.clicks, event.button.x, event.button.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                SDL_Log("MOUSE_WHEEL x=%.1f y=%.1f", event.wheel.x, event.wheel.y);
                break;
            default:
                break;
            }
        }

        buttons = SDL_GetMouseState(&mx, &my);
        if (buttons & SDL_BUTTON_LMASK) {
            r = 255; g = 60; b = 60;
        } else if (buttons & SDL_BUTTON_RMASK) {
            r = 60; g = 60; b = 255;
        } else if (buttons & SDL_BUTTON_MMASK) {
            r = 60; g = 220; b = 60;
        }

        surface = SDL_GetWindowSurface(window);
        if (surface) {
            square.x = (int)mx - SQUARE / 2;
            square.y = (int)my - SQUARE / 2;
            square.w = SQUARE;
            square.h = SQUARE;
            SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 20, 20, 30));
            SDL_FillSurfaceRect(surface, &square, SDL_MapSurfaceRGB(surface, r, g, b));
            SDL_UpdateWindowSurface(window);
        } else {
            SDL_Delay(16);
        }
    }

    SDL_StopTextInput(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return quit_code;
}
