/*
  GamePad touch tests for the SDL3 Wii U port: mouse events made from it,
  and text input through the system software keyboard.

  It opens a window on the GamePad's display (854x480, so touch is 1:1) and
  draws with the window surface only. Nothing is written on screen as text; the
  log (SDL_Log, OSReport on Wii U) tells you what happened.

    - Touch the screen: a white square follows the finger. The log shows the
      finger events (position 0..1 and in pixels) and the mouse events SDL's own
      touch to mouse conversion makes from them. Both must agree with where you
      touched; try the four corners.
    - Green box (bottom left): text input with the full keyboard, starting text
      "Hello". Type, confirm: the log shows TEXT_INPUT "<what you typed>".
    - Blue box (bottom right): number pad, hidden characters, 4 digits at most.
    - Red box (top right): quit.
    - The keyboard is drawn over this program's frames, so it must keep
      presenting while the keyboard is up: this program does.

  Run it with a window on the TV instead by setting SDL_WIIU_WINDOW_TARGET=both
  and the window on the TV display: a TV window shown on the GamePad shows its
  top left 854x480, and touch is mapped to that part.
*/
#include <SDL3/SDL.h>

#define W 854
#define H 480

typedef struct
{
    SDL_Rect rect;
    Uint8 r, g, b;
} Box;

static const Box KEYBOARD_BOX = { { 40, 340, 360, 100 }, 40, 170, 60 };
static const Box NUMPAD_BOX = { { 454, 340, 360, 100 }, 50, 90, 200 };
static const Box QUIT_BOX = { { 700, 20, 134, 60 }, 200, 50, 50 };

static void fill(SDL_Surface *surface, const SDL_Rect *rect, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_FillSurfaceRect(surface, rect, SDL_MapSurfaceRGB(surface, r, g, b));
}

static bool inside(const Box *box, float x, float y)
{
    return x >= box->rect.x && x < box->rect.x + box->rect.w && y >= box->rect.y && y < box->rect.y + box->rect.h;
}

static void start_text(SDL_Window *window, bool numpad)
{
    SDL_PropertiesID props = SDL_CreateProperties();

    if (numpad) {
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN);
        SDL_SetStringProperty(props, "SDL.textinput.wiiu.hint_text", "PIN");
        SDL_SetNumberProperty(props, "SDL.textinput.wiiu.max_length", 4);
    } else {
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
        SDL_SetStringProperty(props, "SDL.textinput.wiiu.initial_text", "Hello");
        SDL_SetStringProperty(props, "SDL.textinput.wiiu.ok_label", "Done");
    }
    if (!SDL_StartTextInputWithProperties(window, props)) {
        SDL_Log("FAIL: SDL_StartTextInput: %s", SDL_GetError());
    } else {
        SDL_Log("text input started (%s)", numpad ? "number pad" : "keyboard");
    }
    SDL_DestroyProperties(props);
}

int main(int argc, char *argv[])
{
    SDL_Window *window;
    SDL_Surface *surface;
    SDL_PropertiesID props;
    SDL_DisplayID *displays;
    SDL_DisplayID gamepad_display = 0;
    SDL_Event event;
    int count = 0, i;
    float dot_x = -100.0f, dot_y = -100.0f;
    bool done = false;
    Uint64 start;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("FAIL: SDL_Init: %s", SDL_GetError());
        return 1;
    }

    displays = SDL_GetDisplays(&count);
    for (i = 0; i < count; ++i) {
        const char *name = SDL_GetDisplayName(displays[i]);
        SDL_Log("display %d: %s", i, name ? name : "?");
        if (name && SDL_strstr(name, "GamePad")) {
            gamepad_display = displays[i];
        }
    }
    SDL_free(displays);
    if (!gamepad_display) {
        SDL_Log("no GamePad display; using the primary one");
        gamepad_display = SDL_GetPrimaryDisplay();
    }

    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "sdl3-wiiu-touch");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_UNDEFINED_DISPLAY(gamepad_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_UNDEFINED_DISPLAY(gamepad_display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, W);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, H);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window) {
        SDL_Log("FAIL: SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    surface = SDL_GetWindowSurface(window);
    if (!surface) {
        SDL_Log("FAIL: SDL_GetWindowSurface: %s", SDL_GetError());
        return 1;
    }
    SDL_free(SDL_GetTouchDevices(&count));
    SDL_Log("window %dx%d, %d touch device(s), keyboard support: %s", surface->w, surface->h, count,
            SDL_HasScreenKeyboardSupport() ? "yes" : "NO");

    start = SDL_GetTicks();
    while (!done && (SDL_GetTicks() - start) < 10 * 60 * 1000) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP: {
                float px = event.tfinger.x * (float)surface->w, py = event.tfinger.y * (float)surface->h;
                SDL_Log("finger %s (%.3f, %.3f) = pixel (%.0f, %.0f)",
                        event.type == SDL_EVENT_FINGER_DOWN ? "down" : event.type == SDL_EVENT_FINGER_UP ? "up" : "move",
                        event.tfinger.x, event.tfinger.y, px, py);
                dot_x = px;
                dot_y = py;
                if (event.type == SDL_EVENT_FINGER_DOWN) {
                    if (inside(&QUIT_BOX, px, py)) {
                        done = true;
                    } else if (inside(&KEYBOARD_BOX, px, py)) {
                        start_text(window, false);
                    } else if (inside(&NUMPAD_BOX, px, py)) {
                        start_text(window, true);
                    }
                }
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
                SDL_Log("mouse motion (%.0f, %.0f)", event.motion.x, event.motion.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                SDL_Log("mouse button %d %s at (%.0f, %.0f)", event.button.button, event.button.down ? "down" : "up",
                        event.button.x, event.button.y);
                break;
            case SDL_EVENT_TEXT_INPUT:
                SDL_Log("TEXT_INPUT \"%s\"", event.text.text);
                break;
            case SDL_EVENT_SCREEN_KEYBOARD_SHOWN:
                SDL_Log("screen keyboard shown");
                break;
            case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN:
                SDL_Log("screen keyboard hidden");
                break;
            case SDL_EVENT_QUIT:
                done = true;
                break;
            default:
                break;
            }
        }

        // Redraw every frame: the keyboard is drawn over each presented frame
        SDL_FillSurfaceRect(surface, NULL, SDL_MapSurfaceRGB(surface, 30, 30, 30));
        fill(surface, &KEYBOARD_BOX.rect, KEYBOARD_BOX.r, KEYBOARD_BOX.g, KEYBOARD_BOX.b);
        fill(surface, &NUMPAD_BOX.rect, NUMPAD_BOX.r, NUMPAD_BOX.g, NUMPAD_BOX.b);
        fill(surface, &QUIT_BOX.rect, QUIT_BOX.r, QUIT_BOX.g, QUIT_BOX.b);
        {
            SDL_Rect dot = { (int)dot_x - 8, (int)dot_y - 8, 16, 16 };
            fill(surface, &dot, 255, 255, 255);
        }
        SDL_UpdateWindowSurface(window);
    }

    SDL_Log("done");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
