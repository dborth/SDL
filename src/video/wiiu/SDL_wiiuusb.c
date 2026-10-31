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

/* USB keyboard and mouse.

   Keyboard: the OS keyboard library (nsyskbd) reports USB HID usage codes and,
   with each key, the character the key makes in the system's keyboard layout.
   SDL's scancodes 4 to 231 are the HID usages, so the code is passed on as the
   scancode. SDL tracks the modifier state itself. The character becomes
   SDL_EVENT_TEXT_INPUT, which SDL only delivers while text input is active.

   Mouse: the OS has no mouse API, so this is a client of the generic HID
   library (nsyshid). A mouse is switched to the HID boot protocol, whose
   report layout is fixed, so no report descriptor is parsed:
       byte 0  buttons (bit 0 left, bit 1 right, bit 2 middle)
       byte 1  X movement, signed
       byte 2  Y movement, signed
       byte 3  wheel, signed (not part of the boot protocol, but sent by
               practically every wheel mouse, and read by Linux's usbmouse
               driver the same way; ignored when the report has no byte 3)
   Motion is relative; SDL keeps the position and clamps it to the window.
   No cursor is drawn (SDL has no software cursor of its own).

   Threads: the keyboard callbacks and the HID callbacks run in the OS
   libraries' contexts, never on the main thread. They only append to small
   queues under one OSMutex. WIIU_USB_Pump() drains them on the main thread and
   is the only place that calls into SDL's event code.

   All state is static (no allocation a late callback could touch after quit),
   except the mouse read buffers, see WIIU_USB_Quit().
*/

#include <coreinit/mutex.h>
#include <malloc.h>
#include <nsyshid/hid.h>
#include <nsyskbd/nsyskbd.h>

#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../SDL_sysvideo.h"

#include "SDL_wiiuusb.h"

#define KEY_QUEUE_SIZE   128
#define MOUSE_QUEUE_SIZE 64
#define MAX_MICE         4
#define MOUSE_BUFFER_MAX 64 // bytes of one report we read

// KBDKeyEvent::isPressedDown is typed BOOL, but the library reports 1 = pressed, 0 = released and
// 3 = held (auto-repeat);
#define KBD_STATE_UP   0
#define KBD_STATE_HELD 3

// HID interface subclass and protocol of a boot protocol mouse (HID 1.11, 4.2 and 4.3)
#define HID_SUBCLASS_BOOT   1
#define HID_PROTOCOL_MOUSE  2
// HIDSetProtocol() argument: 0 = boot protocol, 1 = report protocol
#define HID_SET_BOOT_PROTOCOL 0

// What the HID attach callback returns. The OS offers every attached device to each HID client in
// turn: 1 = this client uses it, 0 = it does not, and the device is left to the others (the OS
// keyboard library would otherwise never see a keyboard).
#define HID_ATTACH_CLAIMED   1
#define HID_ATTACH_UNCLAIMED 0

enum
{
    KEY_UP,
    KEY_DOWN,
    KEY_REPEAT
};

typedef struct
{
    Uint16 text; // UTF-16 character the key makes, 0 if none
    Uint8 hid;
    Uint8 state; // KEY_*
} USB_KeyEvent;

typedef struct
{
    Sint32 dx;
    Sint32 dy;
    Sint32 wheel;
    Uint8 buttons; // SDL_BUTTON_MASK of every button held, over all mice
} USB_MouseEvent;

typedef struct
{
    bool active; // a mouse is attached in this slot
    uint32_t handle;
    uint8_t interface_index;
    uint32_t read_size;
    uint8_t *buffer; // libc memalign; see WIIU_USB_Quit()
    Uint8 buttons;
} USB_MouseSlot;

typedef struct
{
    bool initialized;

    OSMutex lock;
    bool active; // under lock: the callbacks may touch the state below

    // Keyboard. Under lock, except kbd_added.
    bool kbd_ok;
    Uint32 kbd_channels; // one bit per attached keyboard channel
    USB_KeyEvent kbd_queue[KEY_QUEUE_SIZE];
    int kbd_head;
    int kbd_count;
    bool kbd_overflow; // an event was dropped; held keys may be stale
    bool kbd_added;    // main thread only: SDL knows the keyboard

    // Mouse. Under lock, except mouse_added and mouse_sent.
    bool mouse_ok;
    bool hid_setup_ok; // our HIDSetup() worked, so we owe a HIDTeardown()
    HIDClient hid_client;
    USB_MouseSlot mice[MAX_MICE];
    USB_MouseEvent mouse_queue[MOUSE_QUEUE_SIZE];
    int mouse_head;
    int mouse_count;
    Uint8 mouse_queued; // buttons as of the newest queued event
    bool mouse_added;   // main thread only: SDL knows the mouse
    Uint8 mouse_sent;   // main thread only: buttons as of the last event sent to SDL
} USB_Data;

static USB_Data USB;

/* ---------------------------------------------------------------------- */
// Keyboard callbacks (OS context)

static void KeyboardAttachCallback(KBDAttachEvent *event)
{
    if (!event || event->channel >= 32) {
        return;
    }
    OSLockMutex(&USB.lock);
    if (USB.active) {
        USB.kbd_channels |= 1u << event->channel;
    }
    OSUnlockMutex(&USB.lock);
}

static void KeyboardDetachCallback(KBDAttachEvent *event)
{
    if (!event || event->channel >= 32) {
        return;
    }
    OSLockMutex(&USB.lock);
    if (USB.active) {
        USB.kbd_channels &= ~(1u << event->channel);
    }
    OSUnlockMutex(&USB.lock);
}

static void KeyboardKeyCallback(KBDKeyEvent *event)
{
    USB_KeyEvent key;

    if (!event) {
        return;
    }
    key.hid = event->hidCode;
    key.text = event->asUTF16Character;
    if (event->isPressedDown == KBD_STATE_UP) {
        key.state = KEY_UP;
    } else if (event->isPressedDown == KBD_STATE_HELD) {
        key.state = KEY_REPEAT;
    } else {
        key.state = KEY_DOWN;
    }

    OSLockMutex(&USB.lock);
    if (USB.active) {
        if (USB.kbd_count < KEY_QUEUE_SIZE) {
            USB.kbd_queue[(USB.kbd_head + USB.kbd_count) % KEY_QUEUE_SIZE] = key;
            USB.kbd_count++;
        } else {
            USB.kbd_overflow = true;
        }
    }
    OSUnlockMutex(&USB.lock);
}

/* ---------------------------------------------------------------------- */
// Mouse callbacks (OS context)

// Caller holds the lock
static Uint8 CombinedButtons(void)
{
    Uint8 buttons = 0;
    for (int i = 0; i < MAX_MICE; i++) {
        if (USB.mice[i].active) {
            buttons |= USB.mice[i].buttons;
        }
    }
    return buttons;
}

/* Caller holds the lock. Queues the change, merged into the newest event where only motion
 * changed (a fast mouse reports far more often than the game pumps events) or when the queue is
 * full. A merged event takes the latest button state, so a full queue can lose a click but never
 * leaves a button stuck down. */
static void PushMouse(Sint32 dx, Sint32 dy, Sint32 wheel)
{
    const Uint8 buttons = CombinedButtons();
    USB_MouseEvent *event;

    if (dx == 0 && dy == 0 && wheel == 0 && buttons == USB.mouse_queued) {
        return;
    }

    if (USB.mouse_count > 0) {
        USB_MouseEvent *tail = &USB.mouse_queue[(USB.mouse_head + USB.mouse_count - 1) % MOUSE_QUEUE_SIZE];
        if (tail->buttons == buttons || USB.mouse_count >= MOUSE_QUEUE_SIZE) {
            tail->dx += dx;
            tail->dy += dy;
            tail->wheel += wheel;
            tail->buttons = buttons;
            USB.mouse_queued = buttons;
            return;
        }
    }

    event = &USB.mouse_queue[(USB.mouse_head + USB.mouse_count) % MOUSE_QUEUE_SIZE];
    event->dx = dx;
    event->dy = dy;
    event->wheel = wheel;
    event->buttons = buttons;
    USB.mouse_count++;
    USB.mouse_queued = buttons;
}

// Caller holds the lock. The mouse is gone: release what it was holding.
static void DetachMouse(USB_MouseSlot *slot)
{
    slot->active = false;
    slot->buttons = 0;
    PushMouse(0, 0, 0);
}

static void MouseReadCallback(uint32_t handle, int32_t error, uint8_t *buffer, uint32_t bytes, void *context)
{
    const int index = (int)(uintptr_t)context;
    USB_MouseSlot *slot;

    (void)buffer;
    if (index < 0 || index >= MAX_MICE) {
        return;
    }

    OSLockMutex(&USB.lock);
    slot = &USB.mice[index];

    if (!USB.active || !slot->active || slot->handle != handle) {
        // Shutting down, detached, or the slot has been reused
        OSUnlockMutex(&USB.lock);
        return;
    }

    if (error < 0) {
        // The mouse is gone, or the read failed: stop reading it
        DetachMouse(slot);
        OSUnlockMutex(&USB.lock);
        return;
    }

    if (bytes >= 3) {
        Uint8 buttons = 0;
        if (slot->buffer[0] & 0x1) {
            buttons |= SDL_BUTTON_LMASK;
        }
        if (slot->buffer[0] & 0x2) {
            buttons |= SDL_BUTTON_RMASK;
        }
        if (slot->buffer[0] & 0x4) {
            buttons |= SDL_BUTTON_MMASK;
        }
        slot->buttons = buttons;
        PushMouse((Sint8)slot->buffer[1], (Sint8)slot->buffer[2], (bytes >= 4) ? (Sint8)slot->buffer[3] : 0);
    }

    // Keep one read outstanding
    if (HIDRead(slot->handle, slot->buffer, slot->read_size, MouseReadCallback, context) < 0) {
        DetachMouse(slot);
    }
    OSUnlockMutex(&USB.lock);
}

static void MouseProtocolCallback(uint32_t handle, int32_t error, uint8_t *buffer, uint32_t bytes, void *context)
{
    const int index = (int)(uintptr_t)context;
    USB_MouseSlot *slot;

    (void)buffer;
    (void)bytes;
    if (index < 0 || index >= MAX_MICE) {
        return;
    }

    OSLockMutex(&USB.lock);
    slot = &USB.mice[index];

    if (USB.active && slot->active && slot->handle == handle) {
        // Without the boot protocol the report layout is unknown, so do not read
        if (error < 0 || HIDRead(slot->handle, slot->buffer, slot->read_size, MouseReadCallback, context) < 0) {
            slot->active = false;
        }
    }
    OSUnlockMutex(&USB.lock);
}

static int32_t MouseAttachCallback(HIDClient *client, HIDDevice *device, HIDAttachEvent attach)
{
    int32_t result = HID_ATTACH_UNCLAIMED;
    int index = -1;
    USB_MouseSlot *slot;
    uint32_t size;

    (void)client;
    if (!device) {
        return HID_ATTACH_UNCLAIMED;
    }

    OSLockMutex(&USB.lock);

    if (!USB.active) {
        goto done;
    }

    if (attach == HID_DEVICE_DETACH) {
        for (int i = 0; i < MAX_MICE; i++) {
            if (USB.mice[i].active && USB.mice[i].handle == device->handle) {
                DetachMouse(&USB.mice[i]);
                result = HID_ATTACH_CLAIMED;
                break;
            }
        }
        goto done;
    }

    // Mice only: keyboards and everything else are left to their own clients
    if (device->subClass != HID_SUBCLASS_BOOT || device->protocol != HID_PROTOCOL_MOUSE) {
        goto done;
    }

    for (int i = 0; i < MAX_MICE; i++) {
        if (!USB.mice[i].active) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        goto done;
    }

    slot = &USB.mice[index];
    if (!slot->buffer) {
        slot->buffer = (uint8_t *)memalign(0x40, MOUSE_BUFFER_MAX); // This should NOT be SDL_memalign()
    }
    if (!slot->buffer) {
        goto done;
    }

    size = device->maxPacketSizeRx;
    if (size < 4) {
        size = 4;
    }
    if (size > MOUSE_BUFFER_MAX) {
        size = MOUSE_BUFFER_MAX;
    }

    slot->active = true;
    slot->handle = device->handle;
    slot->interface_index = device->interfaceIndex;
    slot->read_size = size;
    slot->buttons = 0;

    if (HIDSetProtocol(slot->handle, slot->interface_index, HID_SET_BOOT_PROTOCOL,
                       MouseProtocolCallback, (void *)(uintptr_t)index) < 0) {
        slot->active = false;
        goto done;
    }
    result = HID_ATTACH_CLAIMED;

done:
    OSUnlockMutex(&USB.lock);
    return result;
}

/* ---------------------------------------------------------------------- */
// Main thread: keyboard

static void SendText(Uint16 character)
{
    char utf8[5];
    char *end;

    // Controls (SDL ignores them too), surrogates, private use (the library may use it for
    // function keys) and noncharacters
    if (character < 0x20 || (character >= 0x7F && character < 0xA0) ||
        (character >= 0xD800 && character <= 0xF8FF) || character >= 0xFFFE) {
        return;
    }
    // Shortcuts are not text. Right Alt stays: it is AltGr on most layouts.
    if (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_LALT | SDL_KMOD_GUI)) {
        return;
    }

    end = SDL_UCS4ToUTF8(character, utf8);
    *end = '\0';
    SDL_SendKeyboardText(utf8);
}

static void PumpKeyboard(void)
{
    Uint32 channels;
    bool overflow;

    if (!USB.kbd_ok) {
        return;
    }

    OSLockMutex(&USB.lock);
    channels = USB.kbd_channels;
    OSUnlockMutex(&USB.lock);

    if (channels && !USB.kbd_added) {
        SDL_AddKeyboard(SDL_DEFAULT_KEYBOARD_ID, "USB Keyboard");
        USB.kbd_added = true;
    }

    for (;;) {
        USB_KeyEvent key;

        OSLockMutex(&USB.lock);
        if (USB.kbd_count == 0) {
            OSUnlockMutex(&USB.lock);
            break;
        }
        key = USB.kbd_queue[USB.kbd_head];
        USB.kbd_head = (USB.kbd_head + 1) % KEY_QUEUE_SIZE;
        USB.kbd_count--;
        OSUnlockMutex(&USB.lock);

        // SDL_SCANCODE_A (4) to SDL_SCANCODE_RGUI (231) are the HID usages
        if (key.hid >= 4 && key.hid <= 231) {
            const bool down = (key.state != KEY_UP);

            // A held key arrives as another press, which SDL turns into a repeat event
            SDL_SendKeyboardKey(0, SDL_DEFAULT_KEYBOARD_ID, key.hid, (SDL_Scancode)key.hid, down);
            if (down) {
                SendText(key.text);
            }
        }
    }

    OSLockMutex(&USB.lock);
    overflow = USB.kbd_overflow;
    USB.kbd_overflow = false;
    channels = USB.kbd_channels;
    OSUnlockMutex(&USB.lock);

    // After a dropped event a key may be stuck down, and nothing will release the keys of a
    // keyboard that is gone
    if (overflow || (!channels && USB.kbd_added)) {
        SDL_ResetKeyboard();
    }
    if (!channels && USB.kbd_added) {
        SDL_RemoveKeyboard(SDL_DEFAULT_KEYBOARD_ID);
        USB.kbd_added = false;
    }
}

/* ---------------------------------------------------------------------- */
// Main thread: mouse

static void SendMouseButtons(SDL_Window *window, Uint8 buttons)
{
    const Uint8 changed = buttons ^ USB.mouse_sent;

    for (int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_RIGHT; button++) {
        if (changed & SDL_BUTTON_MASK(button)) {
            SDL_SendMouseButton(0, window, SDL_DEFAULT_MOUSE_ID, (Uint8)button, (buttons & SDL_BUTTON_MASK(button)) != 0);
        }
    }
    USB.mouse_sent = buttons;
}

// Releases the buttons this driver pressed (SDL's own SDL_ResetMouse() is not declared)
static void ReleaseMouseButtons(void)
{
    if (USB.mouse_sent) {
        SendMouseButtons(SDL_GetMouseFocus(), 0);
    }
}

static void PumpMouse(void)
{
    bool attached = false;

    if (!USB.mouse_ok) {
        return;
    }

    OSLockMutex(&USB.lock);
    for (int i = 0; i < MAX_MICE; i++) {
        if (USB.mice[i].active) {
            attached = true;
        }
    }
    OSUnlockMutex(&USB.lock);

    if (attached && !USB.mouse_added) {
        SDL_AddMouse(SDL_DEFAULT_MOUSE_ID, "USB Mouse");
        USB.mouse_added = true;
    }

    for (;;) {
        USB_MouseEvent event;
        SDL_Window *window;

        OSLockMutex(&USB.lock);
        if (USB.mouse_count == 0) {
            OSUnlockMutex(&USB.lock);
            break;
        }
        event = USB.mouse_queue[USB.mouse_head];
        USB.mouse_head = (USB.mouse_head + 1) % MOUSE_QUEUE_SIZE;
        USB.mouse_count--;
        OSUnlockMutex(&USB.lock);

        window = SDL_GetMouseFocus();
        if (!window) {
            // Nobody to send it to. Keep the button state in step, or the next click looks wrong.
            USB.mouse_sent = event.buttons;
            continue;
        }

        if (!SDL_GetMouse()->has_position) {
            // Relative motion needs a starting point: the middle of the window, not its corner
            SDL_SendMouseMotion(0, window, SDL_DEFAULT_MOUSE_ID, false, (float)window->w * 0.5f, (float)window->h * 0.5f);
        }
        if (event.dx != 0 || event.dy != 0) {
            SDL_SendMouseMotion(0, window, SDL_DEFAULT_MOUSE_ID, true, (float)event.dx, (float)event.dy);
        }
        if (event.wheel != 0) {
            SDL_SendMouseWheel(0, window, SDL_DEFAULT_MOUSE_ID, 0.0f, (float)event.wheel, SDL_MOUSEWHEEL_NORMAL);
        }
        SendMouseButtons(window, event.buttons);
    }

    if (!attached && USB.mouse_added) {
        ReleaseMouseButtons();
        SDL_RemoveMouse(SDL_DEFAULT_MOUSE_ID);
        USB.mouse_added = false;
    }
}

/* ---------------------------------------------------------------------- */
// Init, quit, pump

bool WIIU_USB_Init(void)
{
    SDL_zero(USB);
    OSInitMutex(&USB.lock);

    // The callbacks can fire from inside KBDSetup() and HIDAddClient() (for devices that are
    // already plugged in), so everything they touch is ready first.
    OSLockMutex(&USB.lock);
    USB.active = true;
    OSUnlockMutex(&USB.lock);
    USB.initialized = true;

    USB.kbd_ok = (KBDSetup(KeyboardAttachCallback, KeyboardDetachCallback, KeyboardKeyCallback) == KDB_ERROR_NONE);
    if (!USB.kbd_ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Wii U USB keyboard library did not start; no USB keyboard");
    }

    // The OS keyboard library may have done HIDSetup() already (then ours fails), so only tear
    // down in WIIU_USB_Quit() what we set up ourselves
    USB.hid_setup_ok = (HIDSetup() >= 0);
    USB.mouse_ok = (HIDAddClient(&USB.hid_client, MouseAttachCallback) >= 0);
    if (!USB.mouse_ok) {
        if (USB.hid_setup_ok) {
            HIDTeardown();
            USB.hid_setup_ok = false;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Wii U HID library did not start; no USB mouse");
    }
    return true;
}

void WIIU_USB_Quit(void)
{
    if (!USB.initialized) {
        return;
    }

    // From here the callbacks do nothing, whatever the libraries still deliver
    OSLockMutex(&USB.lock);
    USB.active = false;
    OSUnlockMutex(&USB.lock);

    if (USB.kbd_ok) {
        KBDTeardown();
    }
    if (USB.mouse_ok) {
        HIDDelClient(&USB.hid_client);
        if (USB.hid_setup_ok) {
            HIDTeardown();
        }
    }

    // No SDL calls from here on: SDL_VideoQuit() has already removed the keyboard and the mouse
    // (SDL_QuitKeyboard(), SDL_QuitMouse()) before it calls the driver's VideoQuit().

    /* A read that was cancelled may still point at its buffer. Only HIDTeardown() ends every read,
     * so when somebody else owns the HID library the buffers (at most 4 x 64 bytes) are left
     * allocated instead of risking a write into freed memory. */
    for (int i = 0; i < MAX_MICE; i++) {
        if (USB.hid_setup_ok) {
            free(USB.mice[i].buffer); // This should NOT be SDL_free()
        }
        USB.mice[i].buffer = NULL;
    }

    USB.initialized = false;
}

void WIIU_USB_Pump(void)
{
    if (!USB.initialized) {
        return;
    }
    PumpKeyboard();
    PumpMouse();
}

void WIIU_USB_Suspend(void)
{
    if (!USB.initialized) {
        return;
    }

    // Whatever is typed or clicked while the HOME menu has the foreground is not for us
    OSLockMutex(&USB.lock);
    USB.kbd_head = USB.kbd_count = 0;
    USB.kbd_overflow = false;
    USB.mouse_head = USB.mouse_count = 0;
    USB.mouse_queued = 0;
    for (int i = 0; i < MAX_MICE; i++) {
        USB.mice[i].buttons = 0;
    }
    OSUnlockMutex(&USB.lock);

    // Nothing will report the release of a key or a button that was down when HOME was pressed
    SDL_ResetKeyboard();
    ReleaseMouseButtons();
}

#endif // SDL_VIDEO_DRIVER_WIIU
