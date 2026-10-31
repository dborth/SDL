Wii U
=====

SDL3 port for the Nintendo Wii U, built with devkitPPC and [wut](https://github.com/devkitPro/wut).

Building
--------

Install devkitPro with the `wiiu-dev` group (devkitPPC, wut) and `wiiu-cmake`, then:

```
/opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$DEVKITPRO/portlibs/wiiu
cmake --build build
cmake --install build
```

Configuring the SDL build itself with `-DSDL_WIIU_TOOLS=ON` (the default on Wii U) builds the same tools next to `libSDL3.a`.

Notes
-----

* Wii U has a hardware bug in the load-exclusive and store-exclusive
  instructions (`lwarx`/`stwcx.`). GCC atomics are turned off and
  `SDL_atomic.c` and `SDL_spinlock.c` use the coreinit atomics instead.
* `SDL_MemoryBarrier*` uses a full `sync`.
* Each subsystem has a `src/<subsystem>/wiiu/` directory that CMake globs.
  Threads, mutexes, semaphores, the timer and the clock are
  real coreinit implementations; condition variables, rwlocks and TLS are the
  generic SDL3 ones built on them.
* C++ is only enabled if a `.cpp` file appears under a `wiiu` directory (it is
  needed for `nn::swkbd`).
* The joystick driver (`src/joystick/wiiu`) covers the GamePad and four Wii
  Remote channels (Wii Remote, Nunchuk, Classic Controller, Pro Controller).
  SDL sees one joystick per player. The player index is the channel the Wii U
  reports (player 1 is index 0), the GamePad is coalesced into player 1 together
  with the Wii Remote on channel 0. `docs/wiiu/hello/joystick.c` shows the raw and
  the mapped input live.
* Threads use coreinit directly: 128 KiB default stack, any core, a new thread
  inherits its creator's priority, and `SDL_SetCurrentThreadPriority` maps to
  coreinit priorities 24/16/10/3. `docs/wiiu/hello/threads.c` exercises all of it.
  `SDL_GetPrefPath(org, app)` is `fs:/vol/external01/wiiu/save/<org>/<app>/`,
  `SDL_GetBasePath()` is `.../wiiu/apps/<rpx name>/` if that folder exists and
  the SD card root otherwise, and `SDL_GetUserFolder(SDL_FOLDER_HOME)` is the
  SD card root. romfs is not supported. `docs/wiiu/hello/files.c` exercises it.

Audio
-----

Audio goes through AX (`src/audio/wiiu`). There is one playback device (TV and
GamePad together) and no recording device. The device is always stereo,
`SDL_AUDIO_S16BE`, at the rate AX renders at (48000 Hz); SDL's audio streams
convert whatever the application opens. Buffers are a whole number of AX frames
(144 samples at 48 kHz), so the default 1024-frame buffer becomes 1152 frames
(24 ms), two of them.

* If the application initialises AX itself before opening audio, the driver uses
  AX's settings and leaves AX running on close. Otherwise it initialises AX
  (48 kHz renderer) on open and quits it on close.
* AX setup runs on CPU1; from any other thread the driver borrows a short-lived 
  CPU1 thread for it.
* Set the log priority of `SDL_LOG_CATEGORY_AUDIO` to INFO to see the AX rate,
  buffer size and mix buffer address on open, and the underrun count on close.
* `docs/wiiu/hello/audio.c` is the test (target `sdl3-wiiu-audio`).
  
Video
-----

The `wiiu` video driver (`src/video/wiiu/`) has two displays: the TV (primary,
the system's output size: 1280x720, 1920x1080, 854x480 or 640x480) and the
GamePad (854x480). Each has exactly one mode. Windows are always fullscreen and
exactly as big as their display. Pick the display the usual SDL3 way, for
example with `SDL_PROP_WINDOW_CREATE_X_NUMBER` set to
`SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_id)`.

The software renderer (and `SDL_GetWindowSurface()`) draws into a linear
RGBA8888 surface in MEM2. `SDL_UpdateWindowSurface()` flushes it, the GPU
converts it into a MEM1 color buffer (`GX2CopySurface`), and the color buffers 
are copied to the scan buffers and swapped.

Which screen shows a window is chosen with a window creation property, a hint,
or the display the window is on, in that order. The literal strings are:

* `SDL.window.create.wiiu.target` (string): `"tv"`, `"gamepad"` or `"both"`.
* `SDL_WIIU_WINDOW_TARGET` (hint, same values). For programs that cannot set
  window properties.
* `SDL.window.create.wiiu.prevent_swap` (boolean): the window's updates copy to
  the screen buffers but do not swap or wait. Use it for the first of two
  windows (TV and GamePad) so both flip together.

A window routed to a screen of another size shows its top left part (and the
rest of that screen stays black). `SDL_SetWindowSurfaceVSync()` sets the GX2
swap interval (default 1, a 60 Hz swap).

`PumpEvents` runs ProcUI when SDL started it (it starts ProcUI unless the
application already did): HOME releases the foreground (scan buffers and MEM1
are freed and recreated, the process blocks while in the background), and the
HOME menu's exit request becomes `SDL_EVENT_QUIT`.

`docs/wiiu/hello/video.c` builds as `sdl3-wiiu-video.rpx`.

Renderer
--------

The `gx2` render driver (`src/render/wiiu/`) is listed before the software
renderer, so `SDL_CreateRenderer(window, NULL)` uses it; set
`SDL_HINT_RENDER_DRIVER` to `software` to use the CPU renderer instead. It draws
with the GPU into the color buffer of the window's display. Textures and render
targets live in MEM2 and survive HOME; render targets must be RGBA8888 or
RGBX8888. A window routed to `"both"` screens (see Video above) is scaled to the
second screen with its aspect ratio kept. The two shaders are precompiled
(`src/render/wiiu/shaders/`, from the SDL2 port). Vsync is
`SDL_SetRenderVSync()` (GX2 swap interval). Pixels read back with
`SDL_RenderReadPixels()` from the window depend on the GPU converting the tiled
color buffer to linear.

`docs/wiiu/hello/render.c` builds as `sdl3-wiiu-render.rpx` and logs frame rates
for the GX2 and the software renderer under the same load.

HOME and exit
------------------------

* **HOME** sends `SDL_EVENT_WILL_ENTER_BACKGROUND` and, after the foreground
  is given up, `SDL_EVENT_DID_ENTER_BACKGROUND`; on return
  `SDL_EVENT_WILL_ENTER_FOREGROUND` and `SDL_EVENT_DID_ENTER_FOREGROUND`.
  Windows are minimized and restored and the keyboard focus is dropped and
  returned. Audio is paused and resumes where it was, controllers keep their
  IDs and player indexes (no removed/added events) and are sent neutral on the
  way out.
* `SDL_GetSystemRAM()` is the size of the application's MEM2 area.

`docs/wiiu/hello/lifecycle.c` builds as `sdl3-wiiu-lifecycle.rpx` (memory
cycles and a HOME test).

## Input: touch and text

The GamePad's touch panel is an SDL touch device. SDL's own touch
to mouse conversion makes mouse events from it. The finger goes to the window
that is shown on the GamePad (the keyboard-focused one if it is), at the pixel
where it is drawn; a window that is not shown on the GamePad gets no touch.

`SDL_StartTextInput()` (and `SDL_StartTextInputWithProperties()`) shows the
system software keyboard on the GamePad when there is no USB keyboard (SDL's
`SDL_HINT_ENABLE_SCREEN_KEYBOARD`, default `auto`; set it to `1` to always show
it, `0` to never).
The confirmed text arrives as one `SDL_EVENT_TEXT_INPUT`; cancelling sends
nothing. The keyboard is drawn over the application's frames, so the
application must keep presenting while it is up. Options, as text input
properties: the standard `SDL_PROP_TEXTINPUT_TYPE_NUMBER` (number types give the
number pad, the `*_PASSWORD_HIDDEN` types hide the text),
`SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN` (word suggestions) and
`SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN`, and, as literal strings:

* `SDL.textinput.wiiu.initial_text`, `SDL.textinput.wiiu.hint_text`,
  `SDL.textinput.wiiu.ok_label` (strings, UTF-8)
* `SDL.textinput.wiiu.max_length` (number)

The language comes from the system settings. The Wii Remote pointer is not
supported. `docs/wiiu/hello/touch.c` builds as `sdl3-wiiu-touch.rpx`.

## Input: USB keyboard and mouse

A USB keyboard is an SDL keyboard (`SDL_HasKeyboard()`, `SDL_EVENT_KEYBOARD_ADDED`
and `_REMOVED` on hot-plug) and a USB mouse an SDL mouse. Several keyboards or
mice are merged into one device each; SDL sees one keyboard (ID 1, "USB Keyboard")
and one mouse (ID 1, "USB Mouse"). Both are hot-pluggable, and both belong to the
video subsystem (`SDL_INIT_VIDEO`).

* **Keyboard.** The OS reports USB HID usages, which are SDL's scancodes, so every
  key a HID keyboard has works as a scancode. Keycodes (`SDL_Keycode`) follow SDL's
  default US layout. Text (`SDL_EVENT_TEXT_INPUT`, while text input is active) is
  the character the OS makes for the key in the keyboard layout set in the system
  settings, so it is correct for other layouts too. A held key repeats as
  `SDL_EVENT_KEY_DOWN` with `repeat` set, and repeats its text. Ctrl, Left Alt and
  GUI combinations make no text; Right Alt does (AltGr).
* **Mouse.** Left, middle and right button, motion and the wheel. The OS has no
  mouse API, so the driver is a client of the HID library (boot protocol), and
  mice that only speak a vendor protocol are not seen. Motion is relative; SDL keeps
  the position and keeps it inside the window, starting in the middle. **No cursor is
  drawn** (SDL has no software cursor): an application that wants one draws it
  itself from `SDL_GetMouseState()`.
* **HOME.** While the application is in the background the queued events are
  dropped and held keys and buttons are released, so nothing sticks.

`docs/wiiu/hello/usbinput.c` builds as `sdl3-wiiu-usb.rpx`.
