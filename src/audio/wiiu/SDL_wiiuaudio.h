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
#ifndef SDL_wiiuaudio_h_
#define SDL_wiiuaudio_h_

#include "../SDL_sysaudio.h"

#include <coreinit/event.h>
#include <sndcore2/voice.h>

// The device is always stereo: one AX voice per channel.
#define WIIUAUDIO_CHANNELS    2

// Buffers in the ring. Two is the minimum; each extra one adds one buffer
// period of latency and one of tolerance to a late audio thread.
#define WIIUAUDIO_NUM_BUFFERS 2

struct SDL_PrivateAudioData
{
    // One looping voice per channel (L, R).
    AXVoice *voice[WIIUAUDIO_CHANNELS];

    // True if this driver called AXInitWithParams() and so must AXQuit().
    bool we_initialized_ax;

    // True once the AX frame callback is registered (and so must be removed).
    bool callback_registered;

    // True once registered as a foreground (HOME menu) listener of the video driver.
    bool listener_registered;

    // What AX is actually rendering at. The device uses this rate.
    Uint32 ax_rate;  // AXGetInputSamplesPerSec()
    Uint32 ax_frame; // AXGetInputSamplesPerFrame(): samples per ~3 ms AX frame

    // Sample frames per ring buffer. A multiple of AXGetInputSamplesPerFrame().
    Uint32 frames;

    // The DSP-visible ring, planar and channel-major:
    //   [ L: buf0 buf1 ... ][ R: buf0 buf1 ... ]
    // so each voice loops over one contiguous region. Every buffer is 'frames'
    // 16-bit samples, so buffer b of channel c starts at
    //   ring + (c * WIIUAUDIO_NUM_BUFFERS + b) * frames * 2   (bytes).
    // The whole allocation sits inside one 512 MiB physical block.
    Uint8 *ring;
    size_t ring_size;

    // What SDL writes into (interleaved S16BE). Not DSP memory.
    Uint8 *staging;

    // Auto-reset event the AX frame callback signals when a buffer is released.
    OSEvent wake;

    // --- Shared between the audio thread and the AX frame callback. ---
    // Read and written only through the barriers in SDL_wiiuaudio.c.
    volatile Uint32 playing; // ring buffer the voices are in (callback writes)
    volatile Uint32 slot_commits[WIIUAUDIO_NUM_BUFFERS]; // times each buffer was handed to the DSP (audio thread writes)
    volatile Uint32 closing; // the audio thread has seen shutdown (it writes)

    // Pause across the HOME menu (see WIIUAUDIO_EnterBackground in the .c file)
    volatile Uint32 pause_request; // set when the foreground is released, cleared when it is back (main thread writes)
    OSEvent pause_ack;             // the frame callback signals it once the voices are stopped

    // --- Audio thread only. ---
    Uint32 rendering; // ring buffer SDL is currently filling

    // --- Frame callback only (read after the callback is gone). ---
    Uint32 started;      // voices have been set to PLAYING
    Uint32 paused;       // voices are stopped because of pause_request
    Uint32 saved_offset; // voice position (in samples, within the region) when paused
    Uint32 last_playing; // 'playing' at the previous tick
    Uint32 entered;      // buffers the hardware has moved into
    Uint32 slot_consumed[WIIUAUDIO_NUM_BUFFERS]; // slot_commits seen at the last entry into each buffer
    Uint32 underruns;    // times it moved into a buffer with nothing new committed since its last entry
};

#endif // SDL_wiiuaudio_h_
