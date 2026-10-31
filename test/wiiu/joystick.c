/*
  joystick and gamepad driver tests for the SDL3 Wii U port

  Shows what SDL reports, live, on the TV and the GamePad (console.c). For each
  player there are three lines:

    P1 id=1 "Wii U Controller 1" gamepad
      raw: A X  hat=01  axes=0,-120,0,0     <- SDL_Joystick: raw button names, hat, axes
      pad: A(east) X(north) dpup lefty=-120 <- SDL_Gamepad: label(position) of the mapped buttons, axes

  Press a button on a controller and see which raw button and which SDL gamepad
  button answers.

  Players: one joystick per player. Player 1 is the GamePad coalesced with the
  Wii Remote on channel 0 (their buttons are ORed, the larger axis wins), players
  2 to 4 are the Wii Remotes on channels 1 to 3. The player number shown is
  SDL_GetJoystickPlayerIndexForID() + 1 and is the number the Wii U menu
  gives the controller.

  Raw buttons (the names come from WIIU_BTN_* in src/joystick/wiiu/
  SDL_sysjoystick.c and must be kept in step with it):
    A B X Y L R ZL ZR - + HOME L3 R3 1 2 Z C
  SDL gamepad: positions are fixed by SDL, south = Wii U B, east = Wii U A,
  west = Wii U Y, north = Wii U X. The test shows the Nintendo label first and the
  SDL position in brackets (SDL_GetGamepadStringForButton names the position, so
  a Wii U A reads "b" there, as it does on a Switch Pro Controller).

  Also: press A (east) on a controller to rumble it for 300 ms. The first line of
  the status panel shows the GamePad sensors (accelerometer in m/s^2, gyro in
  rad/s) when there is one.
  Hold MINUS and PLUS together on any controller to stop the test, HOME exits.
*/
#include <SDL3/SDL.h>

#include "console.h"

#define RUN_SECONDS 600
#define MAX_PLAYERS 4
#define STATUS_ROWS (2 + 3 * MAX_PLAYERS)

// Same order as WIIU_BTN_* in the driver
static const char *const raw_names[] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "-", "+", "HOME", "L3", "R3", "1", "2", "Z", "C"
};

static void describe(SDL_JoystickID id)
{
    char guid[33];
    const char *name = SDL_GetJoystickNameForID(id);
    const char *mapping;

    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    SDL_Log("ADDED id=%u player=%d \"%s\" gamepad=%s", (unsigned)id,
            SDL_GetJoystickPlayerIndexForID(id) + 1, name ? name : "?", SDL_IsGamepad(id) ? "yes" : "NO");
    SDL_Log("  guid=%s", guid);
    mapping = SDL_GetGamepadMappingForID(id);
    SDL_Log("  map: %s", mapping ? mapping : "(none)");
}

static void open_device(SDL_JoystickID id)
{
    SDL_Gamepad *gamepad;
    SDL_Joystick *joystick;

    if (SDL_IsGamepad(id)) {
        gamepad = SDL_OpenGamepad(id);
        if (!gamepad) {
            SDL_Log("  could not open as gamepad: %s", SDL_GetError());
            return;
        }
        joystick = SDL_GetGamepadJoystick(gamepad);
        if (SDL_GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL)) {
            SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true);
            SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true);
        }
    } else {
        joystick = SDL_OpenJoystick(id);
        if (!joystick) {
            SDL_Log("  could not open as joystick: %s", SDL_GetError());
            return;
        }
    }
    SDL_Log("  opened: %d buttons, %d axes, %d hats, rumble=%s", SDL_GetNumJoystickButtons(joystick),
            SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickHats(joystick),
            SDL_GetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
}

// Append text to a status line, never past its end
static void append(char *line, size_t size, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(3);
static void append(char *line, size_t size, const char *fmt, ...)
{
    size_t used = SDL_strlen(line);
    va_list ap;

    if (used + 1 >= size) {
        return;
    }
    va_start(ap, fmt);
    SDL_vsnprintf(line + used, size - used, fmt, ap);
    va_end(ap);
}

// "A(east)": the label on the controller, then the position SDL names it by
static const char *pad_button_text(SDL_GamepadButton button, SDL_Gamepad *gamepad)
{
    static char text[32];
    const char *position = SDL_GetGamepadStringForButton(button);
    const char *label = NULL;

    switch (SDL_GetGamepadButtonLabel(gamepad, button)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: label = "A"; break;
    case SDL_GAMEPAD_BUTTON_LABEL_B: label = "B"; break;
    case SDL_GAMEPAD_BUTTON_LABEL_X: label = "X"; break;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: label = "Y"; break;
    default: break;
    }
    if (!label) {
        return position ? position : "?";
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: position = "south"; break;
    case SDL_GAMEPAD_BUTTON_EAST:  position = "east"; break;
    case SDL_GAMEPAD_BUTTON_WEST:  position = "west"; break;
    case SDL_GAMEPAD_BUTTON_NORTH: position = "north"; break;
    default: break;
    }
    SDL_snprintf(text, sizeof(text), "%s(%s)", label, position);
    return text;
}

static void show_player(int slot, SDL_JoystickID id)
{
    char line[160];
    SDL_Joystick *joystick = SDL_GetJoystickFromID(id);
    SDL_Gamepad *gamepad = SDL_GetGamepadFromID(id);
    const char *name = SDL_GetJoystickNameForID(id);
    int row = 2 + slot * 3;
    int i;

    SDL_snprintf(line, sizeof(line), "P%d id=%u \"%s\" %s", SDL_GetJoystickPlayerIndexForID(id) + 1,
                 (unsigned)id, name ? name : "?", gamepad ? "gamepad" : "JOYSTICK ONLY");
    Console_SetStatus(row, "%s", line);

    if (!joystick) {
        Console_SetStatus(row + 1, "  (not open)");
        Console_SetStatus(row + 2, "%s", "");
        return;
    }

    line[0] = '\0';
    append(line, sizeof(line), "  raw:");
    for (i = 0; i < SDL_GetNumJoystickButtons(joystick); ++i) {
        if (SDL_GetJoystickButton(joystick, i)) {
            if (i < (int)SDL_arraysize(raw_names)) {
                append(line, sizeof(line), " %s", raw_names[i]);
            } else {
                append(line, sizeof(line), " #%d", i);
            }
        }
    }
    append(line, sizeof(line), "  hat=%02x axes=", SDL_GetNumJoystickHats(joystick) ? SDL_GetJoystickHat(joystick, 0) : 0);
    for (i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i) {
        append(line, sizeof(line), "%s%d", i ? "," : "", SDL_GetJoystickAxis(joystick, i));
    }
    Console_SetStatus(row + 1, "%s", line);

    line[0] = '\0';
    append(line, sizeof(line), "  pad:");
    if (gamepad) {
        for (i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
            if (SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)i)) {
                append(line, sizeof(line), " %s", pad_button_text((SDL_GamepadButton)i, gamepad));
            }
        }
        for (i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
            Sint16 value = SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)i);
            if (value > 8000 || value < -8000) {
                append(line, sizeof(line), " %s=%d", SDL_GetGamepadStringForAxis((SDL_GamepadAxis)i), value);
            }
        }
    } else {
        append(line, sizeof(line), " (no mapping)");
    }
    Console_SetStatus(row + 2, "%s", line);
}

static void show_status(void)
{
    int count = 0, i, shown = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    float accel[3], gyro[3];
    bool have_sensor = false;

    Console_SetStatus(0, "SDL3 Wii U joystick test: %d device(s)   MINUS+PLUS stops", count);

    for (i = 0; i < count && shown < MAX_PLAYERS; ++i, ++shown) {
        show_player(shown, ids[i]);
        if (!have_sensor) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
            have_sensor = pad && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                          SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3) &&
                          SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3);
        }
    }
    for (; shown < MAX_PLAYERS; ++shown) {
        Console_SetStatus(2 + shown * 3, "%s", "");
        Console_SetStatus(3 + shown * 3, "%s", "");
        Console_SetStatus(4 + shown * 3, "%s", "");
    }
    SDL_free(ids);

    if (have_sensor) {
        Console_SetStatus(1, "accel %.1f %.1f %.1f  gyro %.2f %.2f %.2f", accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]);
    } else {
        Console_SetStatus(1, "no sensors");
    }
}

int tool_main(int argc, char *argv[])
{
    Uint64 start;
    bool done = false;
    SDL_Event event;
    int count = 0;
    SDL_JoystickID *ids;
    int i;

    Console_SetStatusRows(STATUS_ROWS);

    SDL_Log("SDL %d.%d.%d on %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetPlatform());
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        SDL_Log("FAIL: SDL_Init: %s", SDL_GetError());
        return 1;
    }

    ids = SDL_GetJoysticks(&count);
    SDL_Log("%d device(s) at start", count);
    for (i = 0; i < count; ++i) {
        describe(ids[i]);
        open_device(ids[i]);
    }
    SDL_free(ids);

    start = SDL_GetTicks();
    while (!done && Console_Update() && (SDL_GetTicks() - start) < (Uint64)RUN_SECONDS * 1000) {
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_JOYSTICK_ADDED:
                describe(event.jdevice.which);
                open_device(event.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                SDL_Log("REMOVED id=%u", (unsigned)event.jdevice.which);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                SDL_Log("joy %u button %d %s", (unsigned)event.jbutton.which, event.jbutton.button,
                        event.jbutton.down ? "down" : "up");
                break;
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                SDL_Log("joy %u hat %d = 0x%02x", (unsigned)event.jhat.which, event.jhat.hat, event.jhat.value);
                break;
            case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
                SDL_Log("joy %u battery state=%d percent=%d", (unsigned)event.jbattery.which,
                        (int)event.jbattery.state, event.jbattery.percent);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                    SDL_Gamepad *pad = SDL_GetGamepadFromID(event.gbutton.which);
                    if (pad) {
                        SDL_RumbleGamepad(pad, 0xFFFF, 0xFFFF, 300);
                    }
                }
                break;
            case SDL_EVENT_QUIT:
                done = true;
                break;
            default:
                break;
            }
        }

        // MINUS (back) and PLUS (start) together on any gamepad stop the test
        ids = SDL_GetGamepads(&count);
        for (i = 0; i < count; ++i) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
            if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)) {
                done = true;
            }
        }
        SDL_free(ids);

        show_status();
        SDL_Delay(8);
    }

    SDL_Log("done");
    SDL_Quit();
    return 0;
}
