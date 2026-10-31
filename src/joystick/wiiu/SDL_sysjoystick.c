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

#ifdef SDL_JOYSTICK_WIIU

/* This is the Wii U implementation of the SDL joystick API.

   Players and sources
   -------------------
   There are four players, and SDL sees one joystick per player. Player n is
   what the Wii U calls player n + 1: the player index is the WPAD/KPAD channel
   the system reports, and this driver does not assign or move it. A Wii
   Remote that is moved to another player in the Wii U menu simply starts
   reporting on that channel.

   A player has up to two sources, whose input is coalesced into the one
   joystick:

     player 0    the GamePad (VPAD channel 0) and Wii Remote channel 0
     player 1..3 Wii Remote channel n

   A Wii Remote channel can hold a Wii Remote alone, with a Nunchuk, with a
   Classic Controller (any of them with or without MotionPlus), or a Wii U Pro
   Controller. The player's joystick exists while any of its sources does, and
   keeps the same instance ID when a source comes or goes, including when a
   Wii Remote's extension changes.

   Coalescing: a button is down if it is down on any source, the D-pad is the
   union of the sources' D-pads, and an axis takes the value of largest
   magnitude.

   Polling
   -------
   The hardware is polled on the calling thread, from Update() and Detect(),
   at most once per 2 ms so that all joysticks in one SDL_UpdateJoysticks()
   cycle share a poll. No thread is used.

   Layout
   ------
   Every player has the same layout, see WIIU_BTN_*: 17 buttons named for the
   Wii U Pro Controller positions, hat 0 for the D-pad and axes 0 to 3 (left X,
   left Y, right X, right Y; Y is positive down). There are no analog trigger
   axes: ZL/ZR and L/R are buttons. The sources that are not already laid out
   this way (a Wii Remote alone or with a Nunchuk) are translated into it when
   they are read, see WIIU_DecodeWPAD(). The GamePad sensors belong to player 0.

   Hint
   ----
   SDL_WIIU_WIIMOTE_ORIENTATION ("sideways", the default, or "vertical") says
   how a Wii Remote without an extension is held. Sideways, 2 is the primary
   (A) button and 1 the secondary (B) button, and the D-pad is rotated to match.
*/

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>

#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../usb_ids.h"
#include "../../SDL_utils_c.h"

#include "SDL_wiiujoystick_c.h"
#ifdef SDL_VIDEO_DRIVER_WIIU
#include "../../video/wiiu/SDL_wiiuvideo.h" // WIIU_AddForegroundListener()
#endif

// ---------------------------------------------------------------------------
// Constants

#define WIIU_NUM_PLAYERS 4 // one per Wii Remote channel; the GamePad shares player 0
#define WIIU_NUM_WPADS WIIU_NUM_PLAYERS

/* Hardware sources, in a fixed order: the GamePad, then the Wii Remote
   channels. A source is not a player, see WIIU_PlayerSources(). */
#define WIIU_NUM_SOURCES (1 + WIIU_NUM_WPADS)
#define WIIU_SRC_GAMEPAD 0
#define WIIU_SRC_FOR_WPAD(chan) (1 + (chan))
#define WIIU_WPAD_FOR_SRC(src) ((src) - 1)

// Hint that says how a Wii Remote without an extension is held
#define WIIU_HINT_WIIMOTE_ORIENTATION "SDL_WIIU_WIIMOTE_ORIENTATION"

// One GUID for every player, so a mapping made for one applies to all
#define WIIU_GUID_NAME    "Wii U Controller"
#define WIIU_GUID_PRODUCT 0x0300 // invented, see WIIU_JoystickGetDeviceGUID()

// A change that happens after the first scan (a controller connecting, or its
// extension changing) must be seen by this many consecutive Detect() calls
// before it is reported. A Wii Remote can report its extension a moment after
// it connects, and this stops it showing up as two different devices.
#define WIIU_STABLE_DETECTS 3

// Updates closer together than this share one poll of the hardware
#define WIIU_POLL_WINDOW_MS 2

// Stops a motor after about 70 frames of continuous rumble
#define WIIU_MOTOR_MAX_ON_MS 1160
#define WIIU_MOTOR_REST_MS   85

// VPAD plays a rumble pattern of up to 120 bits at 120 Hz (1 s); refresh well within that
#define WIIU_VPAD_RUMBLE_REFRESH_MS 500
#define WIIU_VPAD_PATTERN_BITS      120

// Nominal rate reported for the GamePad sensors; the real rate is the polling rate
#define WIIU_SENSOR_RATE 100.0f

typedef enum
{
    WIIU_PAD_NONE = 0,
    WIIU_PAD_GAMEPAD,
    WIIU_PAD_WIIMOTE, // Wii Remote alone, or with an extension we do not use
    WIIU_PAD_NUNCHUK, // Wii Remote + Nunchuk
    WIIU_PAD_CLASSIC, // Wii Remote + Classic Controller (or Pro)
    WIIU_PAD_PRO,     // Wii U Pro Controller
    WIIU_PAD_COUNT
} WIIU_PadType;

// Raw button numbers. Same order as the libogc driver for 0..14.
enum
{
    WIIU_BTN_A = 0,
    WIIU_BTN_B,
    WIIU_BTN_ONE,
    WIIU_BTN_TWO,
    WIIU_BTN_MINUS,
    WIIU_BTN_PLUS,
    WIIU_BTN_HOME,
    WIIU_BTN_Z, // Nunchuk
    WIIU_BTN_C, // Nunchuk
    WIIU_BTN_X,
    WIIU_BTN_Y,
    WIIU_BTN_L,
    WIIU_BTN_R,
    WIIU_BTN_ZL,
    WIIU_BTN_ZR,
    WIIU_BTN_L3,
    WIIU_BTN_R3,
    WIIU_BTN_COUNT
};

// ---------------------------------------------------------------------------
// State

typedef struct WIIU_Source
{
    bool present;      // a usable device, debounced; its input is part of its player
    WIIU_PadType type; // what the hardware showed when it was last accepted

    // Last hardware poll
    bool online;                // the hardware answers
    WIIU_PadType detected_type; // type the hardware shows now, WIIU_PAD_NONE = no change / unknown
    WPADExtensionType ext;      // extension of the last good probe (Wii Remote channels)

    // Debounce for connections and extension changes
    WIIU_PadType pending_type;
    int pending_count;

    // Latest input state, in the layout of WIIU_BTN_*
    bool have_state;
    Uint32 buttons; // bit n = raw button n
    Uint8 hat;
    Sint16 axes[4];
    float accel[3]; // m/s^2, GamePad only
    float gyro[3];  // rad/s, GamePad only
    Uint32 sample_serial; // counts fresh samples, for the sensors
    SDL_PowerState power_state;
    int power_percent;

    // Rumble
    Uint16 rumble_strength; // what the player's joystick asked for, 0 = off
    bool motor_on;
    Uint64 motor_on_since_ns;
    Uint64 motor_rest_until_ns;
    Uint64 motor_refresh_ns;
} WIIU_Source;

typedef struct WIIU_Player
{
    bool present; // listed to SDL
    SDL_JoystickID instance_id;
    Uint16 rumble_strength; // what SDL asked for, 0 = off
} WIIU_Player;

struct joystick_hwdata
{
    int player;
    bool sensors_enabled;
    Uint32 sensor_serial; // last sample serial sent
};

static WIIU_Source WIIU_sources[WIIU_NUM_SOURCES];
static WIIU_Player WIIU_players[WIIU_NUM_PLAYERS];
static Uint64 WIIU_last_poll_ns; // when the hardware was last polled
static bool WIIU_have_poll;

/* HOME menu policy (see WIIU_JoystickForeground). While the foreground is
   gone the hardware is not polled. Going out sends one neutral sample to every
   open joystick, so no button stays down. Coming back, a source that does not
   answer for the first few Detect() calls is not removed: VPAD and KPAD may
   need a moment, and a removed/added pair would hand out a new instance ID and
   player index. Nothing else changes: sources, players, instance IDs and
   player indexes stay as they were. */
#define WIIU_RESUME_GRACE_DETECTS 30
static bool WIIU_suspended;
static int WIIU_resume_grace;
static bool WIIU_initial_scan; // first scan, from Init(): report devices at once
static bool WIIU_initialized;
static bool WIIU_wiimote_sideways = true;
static VPADStatus WIIU_last_vpad;
static bool WIIU_have_vpad;

// ---------------------------------------------------------------------------
// Translation tables

typedef struct WIIU_ButtonMap
{
    Uint32 mask;
    Uint8 button;
} WIIU_ButtonMap;

#define WIIU_BUTTON_MAP_COUNT(map) ((int)SDL_arraysize(map))

static const WIIU_ButtonMap WIIU_vpad_map[] = {
    { VPAD_BUTTON_A, WIIU_BTN_A },
    { VPAD_BUTTON_B, WIIU_BTN_B },
    { VPAD_BUTTON_X, WIIU_BTN_X },
    { VPAD_BUTTON_Y, WIIU_BTN_Y },
    { VPAD_BUTTON_MINUS, WIIU_BTN_MINUS },
    { VPAD_BUTTON_PLUS, WIIU_BTN_PLUS },
    { VPAD_BUTTON_HOME, WIIU_BTN_HOME },
    { VPAD_BUTTON_L, WIIU_BTN_L },
    { VPAD_BUTTON_R, WIIU_BTN_R },
    { VPAD_BUTTON_ZL, WIIU_BTN_ZL },
    { VPAD_BUTTON_ZR, WIIU_BTN_ZR },
    { VPAD_BUTTON_STICK_L, WIIU_BTN_L3 },
    { VPAD_BUTTON_STICK_R, WIIU_BTN_R3 },
};

// Buttons on the Wii Remote itself (KPADStatus.hold)
static const WIIU_ButtonMap WIIU_wiimote_map[] = {
    { WPAD_BUTTON_A, WIIU_BTN_A },
    { WPAD_BUTTON_B, WIIU_BTN_B },
    { WPAD_BUTTON_1, WIIU_BTN_ONE },
    { WPAD_BUTTON_2, WIIU_BTN_TWO },
    { WPAD_BUTTON_MINUS, WIIU_BTN_MINUS },
    { WPAD_BUTTON_PLUS, WIIU_BTN_PLUS },
    { WPAD_BUTTON_HOME, WIIU_BTN_HOME },
};

// KPADStatus.nunchuk.hold
static const WIIU_ButtonMap WIIU_nunchuk_map[] = {
    { WPAD_NUNCHUK_BUTTON_Z, WIIU_BTN_Z },
    { WPAD_NUNCHUK_BUTTON_C, WIIU_BTN_C },
};

// KPADStatus.classic.hold
static const WIIU_ButtonMap WIIU_classic_map[] = {
    { WPAD_CLASSIC_BUTTON_A, WIIU_BTN_A },
    { WPAD_CLASSIC_BUTTON_B, WIIU_BTN_B },
    { WPAD_CLASSIC_BUTTON_X, WIIU_BTN_X },
    { WPAD_CLASSIC_BUTTON_Y, WIIU_BTN_Y },
    { WPAD_CLASSIC_BUTTON_MINUS, WIIU_BTN_MINUS },
    { WPAD_CLASSIC_BUTTON_PLUS, WIIU_BTN_PLUS },
    { WPAD_CLASSIC_BUTTON_HOME, WIIU_BTN_HOME },
    { WPAD_CLASSIC_BUTTON_L, WIIU_BTN_L },
    { WPAD_CLASSIC_BUTTON_R, WIIU_BTN_R },
    { WPAD_CLASSIC_BUTTON_ZL, WIIU_BTN_ZL },
    { WPAD_CLASSIC_BUTTON_ZR, WIIU_BTN_ZR },
};

// KPADStatus.pro.hold
static const WIIU_ButtonMap WIIU_pro_map[] = {
    { WPAD_PRO_BUTTON_A, WIIU_BTN_A },
    { WPAD_PRO_BUTTON_B, WIIU_BTN_B },
    { WPAD_PRO_BUTTON_X, WIIU_BTN_X },
    { WPAD_PRO_BUTTON_Y, WIIU_BTN_Y },
    { WPAD_PRO_BUTTON_MINUS, WIIU_BTN_MINUS },
    { WPAD_PRO_BUTTON_PLUS, WIIU_BTN_PLUS },
    { WPAD_PRO_BUTTON_HOME, WIIU_BTN_HOME },
    { WPAD_PRO_TRIGGER_L, WIIU_BTN_L },
    { WPAD_PRO_TRIGGER_R, WIIU_BTN_R },
    { WPAD_PRO_TRIGGER_ZL, WIIU_BTN_ZL },
    { WPAD_PRO_TRIGGER_ZR, WIIU_BTN_ZR },
    { WPAD_PRO_BUTTON_STICK_L, WIIU_BTN_L3 },
    { WPAD_PRO_BUTTON_STICK_R, WIIU_BTN_R3 },
};

static Uint32 WIIU_TranslateButtons(Uint32 hold, const WIIU_ButtonMap *map, int count)
{
    Uint32 buttons = 0;
    int i;

    for (i = 0; i < count; ++i) {
        if (hold & map[i].mask) {
            buttons |= (1u << map[i].button);
        }
    }
    return buttons;
}

static Uint8 WIIU_HatFromBits(Uint32 hold, Uint32 up, Uint32 right, Uint32 down, Uint32 left)
{
    Uint8 hat = SDL_HAT_CENTERED;

    if (hold & up) {
        hat |= SDL_HAT_UP;
    }
    if (hold & right) {
        hat |= SDL_HAT_RIGHT;
    }
    if (hold & down) {
        hat |= SDL_HAT_DOWN;
    }
    if (hold & left) {
        hat |= SDL_HAT_LEFT;
    }
    return hat;
}

// Stick floats are -1.0 to 1.0, positive up; SDL wants positive down
static Sint16 WIIU_StickToAxis(float value, bool invert)
{
    if (!(value == value)) { // NaN
        return 0;
    }
    if (value > 1.0f) {
        value = 1.0f;
    } else if (value < -1.0f) {
        value = -1.0f;
    }
    if (invert) {
        value = -value;
    }
    return (Sint16)(value * 32767.0f);
}

static void WIIU_ClearState(WIIU_Source *slot)
{
    slot->have_state = false;
    slot->buttons = 0;
    slot->hat = SDL_HAT_CENTERED;
    SDL_zeroa(slot->axes);
    SDL_zeroa(slot->accel);
    SDL_zeroa(slot->gyro);
    slot->power_state = SDL_POWERSTATE_UNKNOWN;
    slot->power_percent = -1;
}

// ---------------------------------------------------------------------------
// Polling

static void WIIU_DecodeGamePad(WIIU_Source *slot, const VPADStatus *st)
{
    int raw_battery;
    bool charging;

    slot->buttons = WIIU_TranslateButtons(st->hold, WIIU_vpad_map, WIIU_BUTTON_MAP_COUNT(WIIU_vpad_map));
    slot->hat = WIIU_HatFromBits(st->hold, VPAD_BUTTON_UP, VPAD_BUTTON_RIGHT, VPAD_BUTTON_DOWN, VPAD_BUTTON_LEFT);
    slot->axes[0] = WIIU_StickToAxis(st->leftStick.x, false);
    slot->axes[1] = WIIU_StickToAxis(st->leftStick.y, true);
    slot->axes[2] = WIIU_StickToAxis(st->rightStick.x, false);
    slot->axes[3] = WIIU_StickToAxis(st->rightStick.y, true);

    /* Sensors. VPAD gives acceleration in g and angular velocity in turns per second. */
    slot->accel[0] = st->accelorometer.acc.x * SDL_STANDARD_GRAVITY;
    slot->accel[1] = st->accelorometer.acc.y * SDL_STANDARD_GRAVITY;
    slot->accel[2] = st->accelorometer.acc.z * SDL_STANDARD_GRAVITY;
    slot->gyro[0] = st->gyro.x * (2.0f * SDL_PI_F);
    slot->gyro[1] = st->gyro.y * (2.0f * SDL_PI_F);
    slot->gyro[2] = st->gyro.z * (2.0f * SDL_PI_F);

    /* Battery: 0 means charging, 4 and up is full, 1 to 3 are quarters. */
    raw_battery = st->battery;
    charging = (raw_battery == 0);
    if (charging) {
        slot->power_state = SDL_POWERSTATE_CHARGING;
        slot->power_percent = -1;
    } else {
        slot->power_state = SDL_POWERSTATE_ON_BATTERY;
        slot->power_percent = (raw_battery >= 4) ? 100 : (raw_battery * 25);
    }

    slot->have_state = true;
    ++slot->sample_serial;
}

static void WIIU_PollGamePad(WIIU_Source *slot)
{
    VPADStatus st;
    VPADReadError err = VPAD_READ_SUCCESS;
    int32_t count;

    SDL_zero(st);
    count = VPADRead(VPAD_CHAN_0, &st, 1, &err);

    switch (err) {
    case VPAD_READ_SUCCESS:
        slot->online = true;
        if (count > 0) {
            WIIU_DecodeGamePad(slot, &st);
            WIIU_last_vpad = st;
            WIIU_have_vpad = true;
        }
        break;
    case VPAD_READ_NO_SAMPLES:
        // No new packet, but the GamePad is there: keep the last state
        slot->online = true;
        break;
    case VPAD_READ_BUSY:
        // Cannot tell; change nothing
        break;
    default:
        // VPAD_READ_INVALID_CONTROLLER (no GamePad), VPAD_READ_UNINITIALIZED
        slot->online = false;
        WIIU_ClearState(slot);
        break;
    }
    slot->detected_type = slot->online ? WIIU_PAD_GAMEPAD : WIIU_PAD_NONE;
}

static WIIU_PadType WIIU_TypeFromExtension(WPADExtensionType ext, bool *transitional)
{
    *transitional = false;
    switch (ext) {
    case WPAD_EXT_CORE:
    case WPAD_EXT_MPLUS:
        return WIIU_PAD_WIIMOTE;
    case WPAD_EXT_NUNCHUK:
    case WPAD_EXT_MPLUS_NUNCHUK:
        return WIIU_PAD_NUNCHUK;
    case WPAD_EXT_CLASSIC:
    case WPAD_EXT_MPLUS_CLASSIC:
        return WIIU_PAD_CLASSIC;
    case WPAD_EXT_PRO_CONTROLLER:
        return WIIU_PAD_PRO;
    case WPAD_EXT_BALANCE_BOARD:
        return WIIU_PAD_NONE; // not a controller
    case WPAD_EXT_DEV_NOT_FOUND:
    case WPAD_EXT_UNKNOWN:
        // The extension is being identified; do not change anything yet
        *transitional = true;
        return WIIU_PAD_NONE;
    default:
        // Guitar, drums and so on: use the Wii Remote part only
        return WIIU_PAD_WIIMOTE;
    }
}

static bool WIIU_ExtensionIsNunchuk(uint8_t ext)
{
    return ext == WPAD_EXT_NUNCHUK || ext == WPAD_EXT_MPLUS_NUNCHUK;
}

static bool WIIU_ExtensionIsClassic(uint8_t ext)
{
    return ext == WPAD_EXT_CLASSIC || ext == WPAD_EXT_MPLUS_CLASSIC;
}

#define WIIU_BIT(btn) (1u << (btn))

// Wii Remote held sideways: the D-pad's up is the player's left, its right is up
static Uint8 WIIU_RotateHatSideways(Uint8 hat)
{
    Uint8 out = SDL_HAT_CENTERED;

    if (hat & SDL_HAT_RIGHT) {
        out |= SDL_HAT_UP;
    }
    if (hat & SDL_HAT_LEFT) {
        out |= SDL_HAT_DOWN;
    }
    if (hat & SDL_HAT_UP) {
        out |= SDL_HAT_LEFT;
    }
    if (hat & SDL_HAT_DOWN) {
        out |= SDL_HAT_RIGHT;
    }
    return out;
}

// Decode a sample according to the type the source was accepted as. The
// result is in the one layout every player has (WIIU_BTN_*).
static void WIIU_DecodeWPAD(WIIU_Source *slot, int chan, const KPADStatus *st)
{
    Uint32 phys;

    slot->buttons = 0;
    slot->hat = SDL_HAT_CENTERED;
    SDL_zeroa(slot->axes);

    switch (slot->type) {
    case WIIU_PAD_WIIMOTE:
        phys = WIIU_TranslateButtons(st->hold, WIIU_wiimote_map, WIIU_BUTTON_MAP_COUNT(WIIU_wiimote_map));
        slot->hat = WIIU_HatFromBits(st->hold, WPAD_BUTTON_UP, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT);
        slot->buttons = phys & (WIIU_BIT(WIIU_BTN_MINUS) | WIIU_BIT(WIIU_BTN_PLUS) | WIIU_BIT(WIIU_BTN_HOME));
        if (WIIU_wiimote_sideways) {
            // 2 is under the right thumb: primary (A, the east button), 1 is secondary (B, south)
            if (phys & WIIU_BIT(WIIU_BTN_TWO)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_A);
            }
            if (phys & WIIU_BIT(WIIU_BTN_ONE)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_B);
            }
            if (phys & WIIU_BIT(WIIU_BTN_A)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_Y);
            }
            if (phys & WIIU_BIT(WIIU_BTN_B)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_X);
            }
            slot->hat = WIIU_RotateHatSideways(slot->hat);
        } else {
            slot->buttons |= phys & (WIIU_BIT(WIIU_BTN_A) | WIIU_BIT(WIIU_BTN_B) | WIIU_BIT(WIIU_BTN_ONE) | WIIU_BIT(WIIU_BTN_TWO));
        }
        break;

    case WIIU_PAD_NUNCHUK:
        // Nunchuk in the left hand, Wii Remote pointed at the screen in the right
        phys = WIIU_TranslateButtons(st->hold, WIIU_wiimote_map, WIIU_BUTTON_MAP_COUNT(WIIU_wiimote_map));
        slot->hat = WIIU_HatFromBits(st->hold, WPAD_BUTTON_UP, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT);
        slot->buttons = phys & (WIIU_BIT(WIIU_BTN_A) | WIIU_BIT(WIIU_BTN_B) | WIIU_BIT(WIIU_BTN_MINUS) | WIIU_BIT(WIIU_BTN_PLUS) | WIIU_BIT(WIIU_BTN_HOME));
        if (phys & WIIU_BIT(WIIU_BTN_ONE)) {
            slot->buttons |= WIIU_BIT(WIIU_BTN_L);
        }
        if (phys & WIIU_BIT(WIIU_BTN_TWO)) {
            slot->buttons |= WIIU_BIT(WIIU_BTN_R);
        }
        // The Nunchuk fields are only valid while the Nunchuk is the extension
        if (WIIU_ExtensionIsNunchuk(st->extensionType)) {
            phys = WIIU_TranslateButtons(st->nunchuk.hold, WIIU_nunchuk_map, WIIU_BUTTON_MAP_COUNT(WIIU_nunchuk_map));
            if (phys & WIIU_BIT(WIIU_BTN_Z)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_Y);
            }
            if (phys & WIIU_BIT(WIIU_BTN_C)) {
                slot->buttons |= WIIU_BIT(WIIU_BTN_X);
            }
            slot->axes[0] = WIIU_StickToAxis(st->nunchuk.stick.x, false);
            slot->axes[1] = WIIU_StickToAxis(st->nunchuk.stick.y, true);
        }
        break;

    case WIIU_PAD_CLASSIC:
        // Only the Classic Controller's own buttons: its D-pad is not the Wii Remote's
        if (WIIU_ExtensionIsClassic(st->extensionType)) {
            slot->buttons = WIIU_TranslateButtons(st->classic.hold, WIIU_classic_map, WIIU_BUTTON_MAP_COUNT(WIIU_classic_map));
            slot->hat = WIIU_HatFromBits(st->classic.hold, WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_RIGHT,
                                         WPAD_CLASSIC_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_LEFT);
            slot->axes[0] = WIIU_StickToAxis(st->classic.leftStick.x, false);
            slot->axes[1] = WIIU_StickToAxis(st->classic.leftStick.y, true);
            slot->axes[2] = WIIU_StickToAxis(st->classic.rightStick.x, false);
            slot->axes[3] = WIIU_StickToAxis(st->classic.rightStick.y, true);
        }
        break;

    case WIIU_PAD_PRO:
        if (st->extensionType == WPAD_EXT_PRO_CONTROLLER) {
            slot->buttons = WIIU_TranslateButtons(st->pro.hold, WIIU_pro_map, WIIU_BUTTON_MAP_COUNT(WIIU_pro_map));
            slot->hat = WIIU_HatFromBits(st->pro.hold, WPAD_PRO_BUTTON_UP, WPAD_PRO_BUTTON_RIGHT,
                                         WPAD_PRO_BUTTON_DOWN, WPAD_PRO_BUTTON_LEFT);
            slot->axes[0] = WIIU_StickToAxis(st->pro.leftStick.x, false);
            slot->axes[1] = WIIU_StickToAxis(st->pro.leftStick.y, true);
            slot->axes[2] = WIIU_StickToAxis(st->pro.rightStick.x, false);
            slot->axes[3] = WIIU_StickToAxis(st->pro.rightStick.y, true);
            if (st->pro.charging) {
                slot->power_state = SDL_POWERSTATE_CHARGING;
                slot->power_percent = -1;
                slot->have_state = true;
                ++slot->sample_serial;
                return;
            }
        }
        break;

    default:
        return;
    }

    // WPADGetBatteryLevel() is 0 (empty) to 4 (full)
    slot->power_state = SDL_POWERSTATE_ON_BATTERY;
    slot->power_percent = WPADGetBatteryLevel((WPADChan)chan) * 25;
    if (slot->power_percent > 100) {
        slot->power_percent = 100;
    }
    slot->have_state = true;
    ++slot->sample_serial;
}

static void WIIU_PollWPAD(WIIU_Source *slot, int chan)
{
    WPADExtensionType ext = WPAD_EXT_UNKNOWN;
    WPADError probe = WPADProbe((WPADChan)chan, &ext);

    if (probe == WPAD_ERROR_NONE) {
        bool transitional;
        WIIU_PadType type = WIIU_TypeFromExtension(ext, &transitional);

        slot->online = true;
        slot->ext = ext;
        if (!transitional) {
            slot->detected_type = type;
        }

        if (slot->present) {
            KPADStatus st;
            KPADError err = KPAD_ERROR_OK;
            uint32_t count;

            SDL_zero(st);
            count = KPADReadEx((KPADChan)chan, &st, 1, &err);
            /* The error code, not the count, says whether the sample is new:
               a read with no new packet can still return a count. */
            if (count > 0 && err == KPAD_ERROR_OK) {
                WIIU_DecodeWPAD(slot, chan, &st);
            }
        }
    } else if (probe == WPAD_ERROR_NO_CONTROLLER) {
        slot->online = false;
        slot->detected_type = WIIU_PAD_NONE;
        WIIU_ClearState(slot);
    }
    // WPAD_ERROR_NOT_READY (busy or pairing): change nothing
}

static void WIIU_PollPads(void)
{
    int chan;

    /* Several joysticks update in one SDL_UpdateJoysticks() cycle, and
       SDL_OpenJoystick() calls Update() outside one, so a flag that Detect()
       clears is not reliable. Poll again unless the last poll was just now. */
    Uint64 now = SDL_GetTicksNS();

    if (WIIU_suspended) {
        return;
    }
    if (WIIU_have_poll && (now - WIIU_last_poll_ns) < SDL_MS_TO_NS(WIIU_POLL_WINDOW_MS)) {
        return;
    }
    WIIU_have_poll = true;
    WIIU_last_poll_ns = now;

    WIIU_PollGamePad(&WIIU_sources[WIIU_SRC_GAMEPAD]);
    for (chan = 0; chan < WIIU_NUM_WPADS; ++chan) {
        WIIU_PollWPAD(&WIIU_sources[WIIU_SRC_FOR_WPAD(chan)], chan);
    }
}

bool WIIU_GetGamePadSample(VPADStatus *status)
{
    if (!WIIU_initialized || !WIIU_have_vpad || !status) {
        return false;
    }
    *status = WIIU_last_vpad;
    return true;
}

// ---------------------------------------------------------------------------
// Rumble

static void WIIU_MakeVPADPattern(Uint16 strength, uint8_t pattern[15])
{
    /* VPAD only has motor on and off, at 120 Hz, so the strength is the share
       of bits that are on, spread evenly (an error-diffusion pattern). */
    Uint32 on_bits = ((Uint32)strength * WIIU_VPAD_PATTERN_BITS + 32767) / 65535;
    Uint32 acc = 0;
    int i;

    SDL_memset(pattern, 0, 15);
    for (i = 0; i < WIIU_VPAD_PATTERN_BITS; ++i) {
        acc += on_bits;
        if (acc >= WIIU_VPAD_PATTERN_BITS) {
            acc -= WIIU_VPAD_PATTERN_BITS;
            pattern[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        }
    }
}

static void WIIU_MotorStart(int index, Uint16 strength)
{
    if (index == WIIU_SRC_GAMEPAD) {
        uint8_t pattern[15];
        WIIU_MakeVPADPattern(strength, pattern);
        VPADControlMotor(VPAD_CHAN_0, pattern, WIIU_VPAD_PATTERN_BITS);
    } else {
        WPADControlMotor((WPADChan)WIIU_WPAD_FOR_SRC(index), TRUE);
    }
}

static void WIIU_MotorStop(int index)
{
    if (index == WIIU_SRC_GAMEPAD) {
        VPADStopMotor(VPAD_CHAN_0);
    } else {
        WPADControlMotor((WPADChan)WIIU_WPAD_FOR_SRC(index), FALSE);
    }
}

// Start, refresh or stop the motor to match what SDL asked for, within the safety limits
static void WIIU_ServiceRumble(int index)
{
    WIIU_Source *slot = &WIIU_sources[index];
    Uint64 now = SDL_GetTicksNS();

    if (slot->rumble_strength == 0 || !slot->present) {
        if (slot->motor_on) {
            WIIU_MotorStop(index);
            slot->motor_on = false;
        }
        return;
    }

    if (slot->motor_on) {
        if (now - slot->motor_on_since_ns >= SDL_MS_TO_NS(WIIU_MOTOR_MAX_ON_MS)) {
            WIIU_MotorStop(index);
            slot->motor_on = false;
            slot->motor_rest_until_ns = now + SDL_MS_TO_NS(WIIU_MOTOR_REST_MS);
        } else if (index == WIIU_SRC_GAMEPAD && now >= slot->motor_refresh_ns) {
            // The pattern is queued for at most a second; queue it again
            WIIU_MotorStart(index, slot->rumble_strength);
            slot->motor_refresh_ns = now + SDL_MS_TO_NS(WIIU_VPAD_RUMBLE_REFRESH_MS);
        }
    } else if (now >= slot->motor_rest_until_ns) {
        WIIU_MotorStart(index, slot->rumble_strength);
        slot->motor_on = true;
        slot->motor_on_since_ns = now;
        slot->motor_refresh_ns = now + SDL_MS_TO_NS(WIIU_VPAD_RUMBLE_REFRESH_MS);
    }
}

static void WIIU_StopRumble(int index)
{
    WIIU_Source *slot = &WIIU_sources[index];

    slot->rumble_strength = 0;
    if (slot->motor_on) {
        WIIU_MotorStop(index);
        slot->motor_on = false;
    }
    slot->motor_rest_until_ns = 0;
}

// ---------------------------------------------------------------------------
// Sources, players and detection

// The sources that feed a player: the GamePad is part of player 0
static int WIIU_PlayerSources(int player, int sources[2])
{
    int count = 0;

    if (player == 0) {
        sources[count++] = WIIU_SRC_GAMEPAD;
    }
    sources[count++] = WIIU_SRC_FOR_WPAD(player);
    return count;
}

static void WIIU_ResetPending(WIIU_Source *slot)
{
    slot->pending_type = WIIU_PAD_NONE;
    slot->pending_count = 0;
}

static bool WIIU_IsStable(WIIU_Source *slot, WIIU_PadType type)
{
    if (slot->pending_type != type) {
        slot->pending_type = type;
        slot->pending_count = 1;
    } else {
        ++slot->pending_count;
    }
    return slot->pending_count >= WIIU_STABLE_DETECTS;
}

/* Decide what a source is, from the last poll. Nothing is reported to SDL
   from here: the player's joystick lives as long as any of its sources. */
static void WIIU_SyncSource(int index)
{
    WIIU_Source *slot = &WIIU_sources[index];

    if (!slot->online) {
        WIIU_ResetPending(slot);
        if (slot->present && (WIIU_suspended || WIIU_resume_grace > 0)) {
            return; // just back from the HOME menu: wait before believing it
        }
        if (slot->present) {
            WIIU_StopRumble(index);
            slot->present = false;
            slot->type = WIIU_PAD_NONE;
        }
        WIIU_ClearState(slot);
        return;
    }

    if (slot->detected_type == WIIU_PAD_NONE) {
        return; // online, but nothing usable to report yet
    }

    if (!slot->present) {
        if (WIIU_initial_scan || WIIU_IsStable(slot, slot->detected_type)) {
            slot->present = true;
            slot->type = slot->detected_type;
            slot->sample_serial = 0;
            WIIU_ResetPending(slot);
        }
    } else if (slot->detected_type != slot->type) {
        // Different extension: the player's joystick stays, the source is read differently
        if (WIIU_IsStable(slot, slot->detected_type)) {
            slot->type = slot->detected_type;
            WIIU_ResetPending(slot);
            WIIU_ClearState(slot);
        }
    } else {
        WIIU_ResetPending(slot);
    }
}

static void WIIU_StopPlayerRumble(int player)
{
    int sources[2];
    int count = WIIU_PlayerSources(player, sources);
    int i;

    WIIU_players[player].rumble_strength = 0;
    for (i = 0; i < count; ++i) {
        WIIU_StopRumble(sources[i]);
    }
}

// Run the motors of every source of the player, within the safety limits
static void WIIU_ServicePlayerRumble(int player)
{
    int sources[2];
    int count = WIIU_PlayerSources(player, sources);
    int i;

    for (i = 0; i < count; ++i) {
        WIIU_sources[sources[i]].rumble_strength = WIIU_players[player].rumble_strength;
        WIIU_ServiceRumble(sources[i]);
    }
}

static bool WIIU_PlayerWanted(int player)
{
    int sources[2];
    int count = WIIU_PlayerSources(player, sources);
    int i;

    for (i = 0; i < count; ++i) {
        if (WIIU_sources[sources[i]].present) {
            return true;
        }
    }
    return false;
}

static void WIIU_SyncPlayer(int player)
{
    WIIU_Player *pl = &WIIU_players[player];
    bool wanted = WIIU_PlayerWanted(player);

    if (wanted && !pl->present) {
        pl->present = true;
        pl->instance_id = SDL_GetNextObjectID();
        // SDL looks the device up by instance ID, so it has to be listed first
        SDL_PrivateJoystickAdded(pl->instance_id);
    } else if (!wanted && pl->present) {
        // SDL looks the device up by instance ID here too, so it is unlisted afterwards
        SDL_PrivateJoystickRemoved(pl->instance_id);
        WIIU_StopPlayerRumble(player);
        pl->present = false;
        pl->instance_id = 0;
    }
}

static void WIIU_SyncAll(void)
{
    int i;

    for (i = 0; i < WIIU_NUM_SOURCES; ++i) {
        WIIU_SyncSource(i);
    }
    for (i = 0; i < WIIU_NUM_PLAYERS; ++i) {
        WIIU_SyncPlayer(i);
    }
}

static int WIIU_PlayerForDeviceIndex(int device_index)
{
    int i;

    for (i = 0; i < WIIU_NUM_PLAYERS; ++i) {
        if (WIIU_players[i].present) {
            if (device_index == 0) {
                return i;
            }
            --device_index;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Driver functions

static void SDLCALL WIIU_WiimoteOrientationChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    WIIU_wiimote_sideways = !(hint && SDL_strcasecmp(hint, "vertical") == 0);
}

#ifdef SDL_VIDEO_DRIVER_WIIU
// Called by the video driver when the foreground is released (acquired == false)
// and when it is back, on the thread that runs ProcUI.
static void WIIU_JoystickForeground(bool acquired, void *userdata)
{
    int i;

    (void)userdata;
    if (!WIIU_initialized) {
        return;
    }
    if (acquired) {
        WIIU_suspended = false;
        WIIU_resume_grace = WIIU_RESUME_GRACE_DETECTS;
        WIIU_have_poll = false; // poll at once: the first sample is the real state
        return;
    }

    WIIU_suspended = true;
    for (i = 0; i < WIIU_NUM_PLAYERS; ++i) {
        WIIU_StopPlayerRumble(i);
    }
    for (i = 0; i < WIIU_NUM_SOURCES; ++i) {
        WIIU_Source *slot = &WIIU_sources[i];

        WIIU_ClearState(slot);
        slot->have_state = slot->present; // neutral, not "unknown": it is sent below
        slot->online = slot->present;     // a stale answer must not remove the source
    }
    // Deliver the neutral state to the open joysticks now; later is too late.
    SDL_UpdateJoysticks();
}
#endif

static bool WIIU_JoystickInit(void)
{
    int i;

    SDL_zeroa(WIIU_sources);
    SDL_zeroa(WIIU_players);
    for (i = 0; i < WIIU_NUM_SOURCES; ++i) {
        WIIU_ClearState(&WIIU_sources[i]);
    }
    WIIU_have_vpad = false;
    WIIU_have_poll = false;
    WIIU_suspended = false;
    WIIU_resume_grace = 0;

    SDL_AddHintCallback(WIIU_HINT_WIIMOTE_ORIENTATION, WIIU_WiimoteOrientationChanged, NULL);

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    VPADInit();
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    KPADInit();
    WPADEnableURCC(TRUE);
    WPADEnableWiiRemote(TRUE);

    WIIU_initialized = true;
#ifdef SDL_VIDEO_DRIVER_WIIU
    WIIU_AddForegroundListener(WIIU_JoystickForeground, NULL);
#endif

    // Report what is connected now, without waiting for the debounce
    WIIU_initial_scan = true;
    WIIU_PollPads();
    WIIU_SyncAll();
    WIIU_initial_scan = false;
    WIIU_have_poll = false;

    return true;
}

static int WIIU_JoystickGetCount(void)
{
    int i, count = 0;

    for (i = 0; i < WIIU_NUM_PLAYERS; ++i) {
        if (WIIU_players[i].present) {
            ++count;
        }
    }
    return count;
}

static void WIIU_JoystickDetect(void)
{
    if (WIIU_resume_grace > 0 && !WIIU_suspended) {
        --WIIU_resume_grace;
    }
    WIIU_PollPads();
    WIIU_SyncAll();
}

static bool WIIU_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    // Nothing else reports these devices, so there is nothing to hide
    return false;
}

static const char *WIIU_JoystickGetDeviceName(int device_index)
{
    static char name[32];
    int player = WIIU_PlayerForDeviceIndex(device_index);

    if (player < 0) {
        return NULL;
    }
    SDL_snprintf(name, sizeof(name), "Wii U Controller %d", player + 1);
    return name;
}

static const char *WIIU_JoystickGetDevicePath(int device_index)
{
    return NULL;
}

static int WIIU_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    return -1;
}

static int WIIU_JoystickGetDevicePlayerIndex(int device_index)
{
    /* The player the Wii U reports: the Wii Remote channel, and the GamePad
       is part of player 0. It is never a position among the connected
       controllers, and it does not depend on which other controllers exist. */
    return WIIU_PlayerForDeviceIndex(device_index);
}

static void WIIU_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
    // The player is what the Wii U says; SDL keeps its own record of any change
}

static SDL_GUID WIIU_JoystickGetDeviceGUID(int device_index)
{
    int player = WIIU_PlayerForDeviceIndex(device_index);

    if (player < 0) {
        SDL_GUID guid;
        SDL_zero(guid);
        return guid;
    }

    /* One GUID for every player, so a mapping made for one applies to all
       (SDL caches the built-in mapping per GUID). The vendor is Nintendo; the
       product ID is invented (0x0300 is not used by any of the controllers
       SDL knows). driver_data stays 0 because SDL reads it as the Switch
       controller type for Nintendo devices. */
    return SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_UNKNOWN, USB_VENDOR_NINTENDO, WIIU_GUID_PRODUCT, 1,
                                  NULL, WIIU_GUID_NAME, 0, 0);
}

static SDL_JoystickID WIIU_JoystickGetDeviceInstanceID(int device_index)
{
    int player = WIIU_PlayerForDeviceIndex(device_index);

    return (player >= 0) ? WIIU_players[player].instance_id : 0;
}

static bool WIIU_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    int player = WIIU_PlayerForDeviceIndex(device_index);

    if (player < 0) {
        return SDL_SetError("Wii U joystick %d is not connected", device_index);
    }

    joystick->hwdata = (struct joystick_hwdata *)SDL_calloc(1, sizeof(*joystick->hwdata));
    if (!joystick->hwdata) {
        return false;
    }
    joystick->hwdata->player = player;
    joystick->hwdata->sensor_serial = WIIU_sources[WIIU_SRC_GAMEPAD].sample_serial;

    joystick->nbuttons = WIIU_BTN_COUNT;
    joystick->naxes = 4;
    joystick->nhats = 1;

    // Every source has a rumble motor (the Classic Controller uses the Wii Remote's)
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, true);

    if (player == 0) {
        // The GamePad's; they report nothing while there is no GamePad
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, WIIU_SENSOR_RATE);
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, WIIU_SENSOR_RATE);
    }
    return true;
}

static bool WIIU_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    int player;
    WIIU_Player *pl;

    if (!joystick->hwdata) {
        return SDL_SetError("Joystick is not open");
    }
    player = joystick->hwdata->player;
    pl = &WIIU_players[player];
    if (!pl->present || pl->instance_id != joystick->instance_id) {
        return SDL_SetError("Joystick is not connected");
    }

    // One motor per source: use the stronger of the two
    pl->rumble_strength = SDL_max(low_frequency_rumble, high_frequency_rumble);
    WIIU_ServicePlayerRumble(player);
    return true;
}

static bool WIIU_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool WIIU_JoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    if (!joystick->hwdata || joystick->hwdata->player != 0) {
        return SDL_Unsupported();
    }
    joystick->hwdata->sensors_enabled = enabled;
    return true;
}

static void WIIU_JoystickUpdate(SDL_Joystick *joystick)
{
    Uint64 timestamp;
    int player, i, count;
    int sources[2];
    WIIU_Player *pl;
    WIIU_Source *gamepad;
    Uint32 buttons = 0;
    Uint8 hat = SDL_HAT_CENTERED;
    Sint16 axes[4] = { 0, 0, 0, 0 };
    bool have_state = false;
    const WIIU_Source *power = NULL;

    if (!joystick->hwdata) {
        return;
    }
    player = joystick->hwdata->player;
    pl = &WIIU_players[player];

    WIIU_PollPads();

    // Gone, or replaced by a new connection (Detect() reports it)
    if (!pl->present || pl->instance_id != joystick->instance_id) {
        return;
    }

    /* Coalesce the player's sources: a button is down if it is down on any
       of them, the D-pad is their union, an axis takes the largest value. */
    count = WIIU_PlayerSources(player, sources);
    for (i = 0; i < count; ++i) {
        const WIIU_Source *slot = &WIIU_sources[sources[i]];
        int a;

        if (!slot->present || !slot->have_state) {
            continue;
        }
        have_state = true;
        buttons |= slot->buttons;
        hat |= slot->hat;
        for (a = 0; a < 4; ++a) {
            if (SDL_abs(slot->axes[a]) > SDL_abs(axes[a])) {
                axes[a] = slot->axes[a];
            }
        }
        if (!power && slot->power_state != SDL_POWERSTATE_UNKNOWN) {
            power = slot;
        }
    }

    timestamp = SDL_GetTicksNS();

    if (have_state) {
        for (i = 0; i < joystick->nbuttons; ++i) {
            SDL_SendJoystickButton(timestamp, joystick, (Uint8)i, (buttons & (1u << i)) != 0);
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
        for (i = 0; i < joystick->naxes; ++i) {
            SDL_SendJoystickAxis(timestamp, joystick, (Uint8)i, axes[i]);
        }
        if (power) {
            SDL_SendJoystickPowerInfo(joystick, power->power_state, power->power_percent);
        }
    }

    // One sensor sample per fresh GamePad sample
    gamepad = &WIIU_sources[WIIU_SRC_GAMEPAD];
    if (player == 0 && joystick->hwdata->sensors_enabled && gamepad->present && gamepad->have_state &&
        joystick->hwdata->sensor_serial != gamepad->sample_serial) {
        joystick->hwdata->sensor_serial = gamepad->sample_serial;
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, timestamp, gamepad->accel, 3);
        SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, timestamp, gamepad->gyro, 3);
    }

    // Keep a running rumble within the motor limits
    WIIU_ServicePlayerRumble(player);
}

static void WIIU_JoystickClose(SDL_Joystick *joystick)
{
    if (joystick->hwdata) {
        // Do not leave a motor running for a joystick nobody has open
        if (WIIU_players[joystick->hwdata->player].rumble_strength != 0) {
            WIIU_StopPlayerRumble(joystick->hwdata->player);
        }
        SDL_free(joystick->hwdata);
        joystick->hwdata = NULL;
    }
}

static void WIIU_JoystickQuit(void)
{
    int i;

#ifdef SDL_VIDEO_DRIVER_WIIU
    WIIU_RemoveForegroundListener(WIIU_JoystickForeground, NULL);
#endif
    for (i = 0; i < WIIU_NUM_SOURCES; ++i) {
        WIIU_StopRumble(i);
    }
    SDL_RemoveHintCallback(WIIU_HINT_WIIMOTE_ORIENTATION, WIIU_WiimoteOrientationChanged, NULL);
    SDL_zeroa(WIIU_sources);
    SDL_zeroa(WIIU_players);
    WIIU_have_vpad = false;
    WIIU_have_poll = false;
    WIIU_initialized = false;
}

// ---------------------------------------------------------------------------
// Built-in gamepad mapping

static SDL_InputMapping WIIU_MapNone(void)
{
    SDL_InputMapping m;
    SDL_zero(m);
    m.kind = EMappingKind_None;
    m.target = 255;
    return m;
}

static SDL_InputMapping WIIU_MapButton(int button)
{
    SDL_InputMapping m = WIIU_MapNone();
    m.kind = EMappingKind_Button;
    m.target = (Uint8)button;
    return m;
}

static SDL_InputMapping WIIU_MapAxis(int axis)
{
    SDL_InputMapping m = WIIU_MapNone();
    m.kind = EMappingKind_Axis;
    m.target = (Uint8)axis;
    return m;
}

// Hat 0, target is (hat << 4) | direction
static SDL_InputMapping WIIU_MapHat(Uint8 direction)
{
    SDL_InputMapping m = WIIU_MapNone();
    m.kind = EMappingKind_Hat;
    m.target = direction;
    return m;
}

static bool WIIU_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    if (WIIU_PlayerForDeviceIndex(device_index) < 0) {
        return false;
    }

    SDL_zerop(out);
    out->a = out->b = out->x = out->y = WIIU_MapNone();
    out->back = out->guide = out->start = WIIU_MapNone();
    out->leftstick = out->rightstick = out->leftshoulder = out->rightshoulder = WIIU_MapNone();
    out->dpup = out->dpdown = out->dpleft = out->dpright = WIIU_MapNone();
    out->misc1 = out->misc2 = out->misc3 = out->misc4 = out->misc5 = out->misc6 = WIIU_MapNone();
    out->right_paddle1 = out->left_paddle1 = out->right_paddle2 = out->left_paddle2 = WIIU_MapNone();
    out->leftx = out->lefty = out->rightx = out->righty = WIIU_MapNone();
    out->lefttrigger = out->righttrigger = out->touchpad = WIIU_MapNone();

    /* One mapping for every player, whatever its sources are: they are all
       in the layout of WIIU_BTN_* by the time SDL sees them. SDL's a/b/x/y are
       positions (a is the bottom button), and Nintendo's A is on the right:
       a = B, b = A, x = Y, y = X. */
    out->a = WIIU_MapButton(WIIU_BTN_B);
    out->b = WIIU_MapButton(WIIU_BTN_A);
    out->x = WIIU_MapButton(WIIU_BTN_Y);
    out->y = WIIU_MapButton(WIIU_BTN_X);
    out->back = WIIU_MapButton(WIIU_BTN_MINUS);
    out->start = WIIU_MapButton(WIIU_BTN_PLUS);
    out->guide = WIIU_MapButton(WIIU_BTN_HOME);
    out->leftshoulder = WIIU_MapButton(WIIU_BTN_L);
    out->rightshoulder = WIIU_MapButton(WIIU_BTN_R);
    out->lefttrigger = WIIU_MapButton(WIIU_BTN_ZL);
    out->righttrigger = WIIU_MapButton(WIIU_BTN_ZR);
    out->leftstick = WIIU_MapButton(WIIU_BTN_L3);
    out->rightstick = WIIU_MapButton(WIIU_BTN_R3);
    out->leftx = WIIU_MapAxis(0);
    out->lefty = WIIU_MapAxis(1);
    out->rightx = WIIU_MapAxis(2);
    out->righty = WIIU_MapAxis(3);
    out->dpup = WIIU_MapHat(SDL_HAT_UP);
    out->dpdown = WIIU_MapHat(SDL_HAT_DOWN);
    out->dpleft = WIIU_MapHat(SDL_HAT_LEFT);
    out->dpright = WIIU_MapHat(SDL_HAT_RIGHT);
    return true;
}

SDL_JoystickDriver SDL_WIIU_JoystickDriver = {
    WIIU_JoystickInit,
    WIIU_JoystickGetCount,
    WIIU_JoystickDetect,
    WIIU_JoystickIsDevicePresent,
    WIIU_JoystickGetDeviceName,
    WIIU_JoystickGetDevicePath,
    WIIU_JoystickGetDeviceSteamVirtualGamepadSlot,
    WIIU_JoystickGetDevicePlayerIndex,
    WIIU_JoystickSetDevicePlayerIndex,
    WIIU_JoystickGetDeviceGUID,
    WIIU_JoystickGetDeviceInstanceID,
    WIIU_JoystickOpen,
    WIIU_JoystickRumble,
    WIIU_JoystickRumbleTriggers,
    WIIU_JoystickSetLED,
    WIIU_JoystickSendEffect,
    WIIU_JoystickSetSensorsEnabled,
    WIIU_JoystickUpdate,
    WIIU_JoystickClose,
    WIIU_JoystickQuit,
    WIIU_JoystickGetGamepadMapping
};

#endif // SDL_JOYSTICK_WIIU
