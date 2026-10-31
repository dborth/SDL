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

#ifdef SDL_AUDIO_DRIVER_WIIU

// Wii U audio through AX (sndcore2).
//
// Written for SDL3 from the SDL2 Wii U patch
//
// * The device is fixed: stereo, SDL_AUDIO_S16BE, at the rate AX renders at
//   (48 kHz when this driver initialises AX). SDL's audio streams convert
//   anything else, so AX's own sample-rate converter is not used.
// * Two voices (L, R) loop over a small planar ring of ping-pong buffers and
//   are configured once. The SDL2 patch rewrote each voice's end and loop
//   offsets on every AX frame; that is not needed with a channel-major ring,
//   and it keeps AX calls out of the frame callback. The callback only reads
//   the voices' position, counts, and wakes WaitDevice.
// * SDL3 calls GetDeviceBuf -> PlayDevice -> WaitDevice, in that order, and
//   fills the first buffer without waiting. So buffer 0 starts as silence on
//   the DSP, SDL fills buffer 1, and WaitDevice then blocks until the voices
//   have left the buffer SDL would fill next.
// * SDL writes interleaved samples into a staging buffer; PlayDevice
//   deinterleaves them straight into the DSP buffer. (The SDL2 patch
//   deinterleaved through a scratch copy and memcpy'd back.)
// * The audio thread is not pinned. It makes no AX calls; only the AX setup and
//   teardown calls need a particular core (CPU1), see RunOnAXCore().

#include "../SDL_sysaudio.h"
#include "SDL_wiiuaudio.h"

#include <coreinit/cache.h>
#include <coreinit/event.h>
#include <coreinit/memorymap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/memdefaultheap.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/result.h>
#include <sndcore2/voice.h>

#ifdef SDL_VIDEO_DRIVER_WIIU
#include "../../video/wiiu/SDL_wiiuvideo.h" // WIIU_AddForegroundListener()
#endif

#define WIIUAUDIO_DRIVER_NAME "wiiu"

// Full volume for voices and mix buses.
#define WIIUAUDIO_AX_VOLUME_MAX 0x8000

// Output channels AX mixes to: TV has 6, GamePad has 4.
#define WIIUAUDIO_AX_TV_CHANNELS  6
#define WIIUAUDIO_AX_DRC_CHANNELS 4

// AX setup and teardown run on this core (the SDL2 patch's AX_MAIN_AFFINITY).
#define WIIUAUDIO_AX_AFFINITY OS_THREAD_ATTRIB_AFFINITY_CPU1
#define WIIUAUDIO_AX_THREAD_STACK_SIZE (32 * 1024)

// A missed frame-callback signal costs at most this long (two AX frames).
#define WIIUAUDIO_WAIT_TIMEOUT_MS 6

// How long to wait for the frame callback to confirm the voices are stopped
// when the foreground is released. It normally takes one AX frame (~3 ms).
#define WIIUAUDIO_PAUSE_TIMEOUT_MS 50

// How long to let an in-flight frame callback finish after deregistering it.
#define WIIUAUDIO_CALLBACK_DRAIN_MS 10

// Attempts at getting a DSP-safe buffer (as in the SDL2 patch).
#define WIIUAUDIO_DSP_ALLOC_TRIES 32

// The frame callback has no user data, and there is only one AX output, so one
// device at a time. Set only while the callback may run.
static struct SDL_PrivateAudioData *volatile wiiu_active_device = NULL;

// The callback and the audio thread run on different cores and the CPU
// reorders memory accesses, so shared values go through a full barrier.
static SDL_INLINE Uint32 LoadShared(const volatile Uint32 *p)
{
    const Uint32 v = *p;
    OSMemoryBarrier();
    return v;
}

static SDL_INLINE void StoreShared(volatile Uint32 *p, Uint32 v)
{
    OSMemoryBarrier();
    *p = v;
}

// ---------------------------------------------------------------------------
// AX frame callback. Runs on AX's thread every AX frame (~3 ms), before the
// frame is rendered. Keep it short. Only coreinit and sndcore2 calls: no SDL,
// no libc.
// ---------------------------------------------------------------------------

static void WIIUAUDIO_FrameCallback(void)
{
    struct SDL_PrivateAudioData *h = wiiu_active_device;
    AXVoiceOffsets offs;
    Uint32 b;

    if (!h) {
        return;
    }
    OSMemoryBarrier();

    /* HOME menu: stop the voices where they are and start them again from the
       same place. SDL's audio thread keeps its normal cadence: WaitDevice waits
       for 'playing' to advance, which it cannot while the voices are stopped,
       so nothing is rendered ahead and no stale buffer is replayed on return. */
    if (LoadShared(&h->pause_request)) {
        if (!h->paused) {
            if (h->started) {
                AXGetVoiceOffsets(h->voice[0], &offs);
                h->saved_offset = offs.currentOffset;
                AXSetVoiceState(h->voice[0], AX_VOICE_STATE_STOPPED);
                AXSetVoiceState(h->voice[1], AX_VOICE_STATE_STOPPED);
            }
            h->paused = 1;
            OSSignalEvent(&h->pause_ack);
        }
        return;
    }
    if (h->paused) {
        if (h->started) {
            AXSetVoiceCurrentOffset(h->voice[0], h->saved_offset);
            AXSetVoiceCurrentOffset(h->voice[1], h->saved_offset);
            AXSetVoiceState(h->voice[0], AX_VOICE_STATE_PLAYING);
            AXSetVoiceState(h->voice[1], AX_VOICE_STATE_PLAYING);
        }
        h->paused = 0;
        return;
    }

    if (!h->started) {
        // Both voices are started inside one callback so they begin on the
        // same AX frame and stay in step (no left/right skew).
        AXSetVoiceState(h->voice[0], AX_VOICE_STATE_PLAYING);
        AXSetVoiceState(h->voice[1], AX_VOICE_STATE_PLAYING);
        h->started = 1;
        return;
    }

    // Both voices have the same layout, so voice 0 says where both are. Each
    // channel's region holds WIIUAUDIO_NUM_BUFFERS buffers of 'frames' samples.
    AXGetVoiceOffsets(h->voice[0], &offs);
    b = offs.currentOffset / h->frames;
    if (b >= WIIUAUDIO_NUM_BUFFERS) {
        b = WIIUAUDIO_NUM_BUFFERS - 1; // not expected; stay in range
    }

    if (b != h->last_playing) {
        // The voices moved on, into buffer b (normally exactly one step).
        // Each buffer must have been committed again since the hardware last
        // entered it; if not, the voices play stale data: an underrun. This is
        // tracked per buffer, so one late buffer is one underrun and the count
        // does not drift afterwards.
        Uint32 slot = h->last_playing;
        Uint32 commits;
        do {
            slot = (slot + 1) % WIIUAUDIO_NUM_BUFFERS;
            commits = LoadShared(&h->slot_commits[slot]);
            if (commits == h->slot_consumed[slot] && !LoadShared(&h->closing)) {
                h->underruns++;
            }
            h->slot_consumed[slot] = commits;
            h->entered++;
        } while (slot != b);
        h->last_playing = b;

        // The buffer we just left is free for SDL to fill.
        StoreShared(&h->playing, b);
        OSSignalEvent(&h->wake);
    }
}

// ---------------------------------------------------------------------------
// Running AX calls on the AX core.
// ---------------------------------------------------------------------------

typedef struct WiiUAXCall
{
    int (*fn)(void *arg);
    void *arg;
} WiiUAXCall;

static int WIIUAUDIO_AXThreadMain(int argc, const char **argv)
{
    // argv carries the WiiUAXCall (see RunOnAXCore); argc is unused.
    WiiUAXCall *call = (WiiUAXCall *)(void *)argv;
    (void)argc;
    return call->fn(call->arg);
}

static bool WIIUAUDIO_RunOnAXCore(int (*fn)(void *arg), void *arg, int *result)
{
    WiiUAXCall call;
    OSThread *self = OSGetCurrentThread();

    call.fn = fn;
    call.arg = arg;

    if (OSGetThreadAffinity(self) == WIIUAUDIO_AX_AFFINITY) {
        *result = fn(arg);
        return true;
    }

    {
        // One block: the OSThread, then the stack.
        const size_t header_size = (sizeof(OSThread) + 31) & ~(size_t)31;
        const size_t total_size = header_size + WIIUAUDIO_AX_THREAD_STACK_SIZE;
        OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx((uint32_t)total_size, 32);
        int thread_result = 0;

        if (!thread) {
            return SDL_OutOfMemory();
        }

        // OSCreateThread takes the address of the top of the stack. The thread
        // gets no cleanup callback (wut's newlib layer owns that) and no
        // deallocator: it is joined and freed here.
        if (!OSCreateThread(thread, WIIUAUDIO_AXThreadMain, 0, (char *)&call,
                            (Uint8 *)thread + total_size, WIIUAUDIO_AX_THREAD_STACK_SIZE,
                            OSGetThreadPriority(self), WIIUAUDIO_AX_AFFINITY)) {
            MEMFreeToDefaultHeap(thread);
            return SDL_SetError("OSCreateThread() failed");
        }
        OSResumeThread(thread);

        if (!OSJoinThread(thread, &thread_result)) {
            // Cannot free a thread we could not join.
            return SDL_SetError("OSJoinThread() failed");
        }
        MEMFreeToDefaultHeap(thread);
        *result = thread_result;
    }
    return true;
}

// ---------------------------------------------------------------------------
// AX setup and teardown (these run through RunOnAXCore: no SDL, no libc).
// ---------------------------------------------------------------------------

// Initialises AX if nobody has, and reads its rate. AX is initialised once per
// process; if the application already did it, its settings are used and it is
// left running when the device closes.
static int WIIUAUDIO_InitAX(void *arg)
{
    struct SDL_PrivateAudioData *h = (struct SDL_PrivateAudioData *)arg;

    if (!AXIsInit()) {
        AXInitParams params = {
            .renderer = AX_INIT_RENDERER_48KHZ,
            .pipeline = AX_INIT_PIPELINE_SINGLE,
        };
        AXInitWithParams(&params);
        h->we_initialized_ax = true;
    }
    if (!AXIsInit()) {
        return -1;
    }

    h->ax_rate = AXGetInputSamplesPerSec();
    h->ax_frame = AXGetInputSamplesPerFrame();
    return 0;
}

// Voice 'channel' plays one channel's region of the ring, looping forever, hard
// panned to output channel 'channel' on both the TV and the GamePad.
static void WIIUAUDIO_ConfigureVoice(AXVoice *v, int channel, const Uint8 *region, Uint32 region_samples)
{
    AXVoiceDeviceMixData tv[WIIUAUDIO_AX_TV_CHANNELS];
    AXVoiceDeviceMixData drc[WIIUAUDIO_AX_DRC_CHANNELS];
    AXVoiceVeData ve;
    AXVoiceSrc src;
    AXVoiceOffsets offs;
    unsigned int i, j;

    for (i = 0; i < WIIUAUDIO_AX_TV_CHANNELS; i++) {
        for (j = 0; j < 4; j++) {
            tv[i].bus[j].volume = 0;
            tv[i].bus[j].delta = 0;
        }
    }
    for (i = 0; i < WIIUAUDIO_AX_DRC_CHANNELS; i++) {
        for (j = 0; j < 4; j++) {
            drc[i].bus[j].volume = 0;
            drc[i].bus[j].delta = 0;
        }
    }
    tv[channel].bus[0].volume = WIIUAUDIO_AX_VOLUME_MAX;
    drc[channel].bus[0].volume = WIIUAUDIO_AX_VOLUME_MAX;

    ve.volume = WIIUAUDIO_AX_VOLUME_MAX;
    ve.delta = 0;

    // The device runs at AX's own rate, so no rate conversion: ratio 1.0 in
    // 16.16 fixed point, resampler off.
    src.ratio = 0x00010000;
    src.currentOffsetFrac = 0;
    for (i = 0; i < 4; i++) {
        src.lastSample[i] = 0;
    }

    offs.dataType = AX_VOICE_FORMAT_LPCM16;
    offs.loopingEnabled = AX_VOICE_LOOP_ENABLED;
    offs.loopOffset = 0;
    offs.endOffset = region_samples - 1; // inclusive: the last sample played
    offs.currentOffset = 0;
    offs.data = region;

    AXVoiceBegin(v);
    AXSetVoiceType(v, 0);
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_TV, 0, tv);
    AXSetVoiceDeviceMix(v, AX_DEVICE_TYPE_DRC, 0, drc);
    AXSetVoiceVe(v, &ve);
    AXSetVoiceSrc(v, &src);
    AXSetVoiceSrcType(v, AX_VOICE_SRC_TYPE_NONE);
    AXSetVoiceOffsets(v, &offs);
    AXSetVoiceState(v, AX_VOICE_STATE_STOPPED); // started from the frame callback
    AXVoiceEnd(v);
}

static int WIIUAUDIO_StartVoices(void *arg)
{
    struct SDL_PrivateAudioData *h = (struct SDL_PrivateAudioData *)arg;
    const Uint32 region_samples = h->frames * WIIUAUDIO_NUM_BUFFERS;
    int i;

    for (i = 0; i < WIIUAUDIO_CHANNELS; i++) {
        h->voice[i] = AXAcquireVoice(31, NULL, NULL); // top priority
        if (!h->voice[i]) {
            return -1;
        }
        WIIUAUDIO_ConfigureVoice(h->voice[i], i, h->ring + (size_t)i * region_samples * 2, region_samples);
    }

    // Publish the device before the callback can see it.
    wiiu_active_device = h;
    OSMemoryBarrier();

    if (AXRegisterAppFrameCallback(WIIUAUDIO_FrameCallback) != AX_RESULT_SUCCESS) {
        wiiu_active_device = NULL;
        return -2;
    }
    h->callback_registered = true;
    return 0;
}

// The caller has already cleared wiiu_active_device, so the callback returns at
// once if it fires from now on.
static int WIIUAUDIO_StopAX(void *arg)
{
    struct SDL_PrivateAudioData *h = (struct SDL_PrivateAudioData *)arg;
    int i;

    for (i = 0; i < WIIUAUDIO_CHANNELS; i++) {
        if (h->voice[i]) {
            AXSetVoiceState(h->voice[i], AX_VOICE_STATE_STOPPED);
        }
    }
    if (h->callback_registered) {
        AXDeregisterAppFrameCallback(WIIUAUDIO_FrameCallback);
        h->callback_registered = false;

        // A callback that was already running when it was removed gets time to
        // finish before the voices and buffers go away.
        OSSleepTicks(OSMillisecondsToTicks(WIIUAUDIO_CALLBACK_DRAIN_MS));
    }
    for (i = 0; i < WIIUAUDIO_CHANNELS; i++) {
        if (h->voice[i]) {
            AXFreeVoice(h->voice[i]);
            h->voice[i] = NULL;
        }
    }
    if (h->we_initialized_ax && AXIsInit()) {
        AXQuit();
    }
    h->we_initialized_ax = false;
    return 0;
}

// ---------------------------------------------------------------------------
// DSP buffer allocation.
// ---------------------------------------------------------------------------

// The DSP reads the ring directly, so it must be physically contiguous and
// must not cross a 512 MiB physical boundary. This is the rule the SDL2 patch
// enforces (it masks the start address with 0x1fffffff, so it checks for no
// crossing, not for "inside the first 512 MiB"). Which reading is the real
// hardware rule is unconfirmed: the physical address is logged when the device
// opens so a hardware run can settle it.
static bool WIIUAUDIO_IsDspSafe(const void *ptr, size_t size)
{
    const uint32_t start = (uint32_t)(uintptr_t)ptr;
    const uint32_t first = OSEffectiveToPhysical(start);
    const uint32_t last = OSEffectiveToPhysical(start + (uint32_t)size - 1);

    if (first == 0 || last == 0) {
        return false; // not mapped
    }
    if ((last - first) != (uint32_t)(size - 1)) {
        return false; // not physically contiguous
    }
    return ((first & 0x1FFFFFFFu) + (uint32_t)size) <= 0x1FFFFFFFu;
}

static Uint8 *WIIUAUDIO_AllocDspBuffer(size_t size)
{
    void *rejected[WIIUAUDIO_DSP_ALLOC_TRIES];
    int num_rejected = 0;
    Uint8 *result = NULL;
    int i;

    // Keep rejected blocks allocated until we have a good one, so the
    // allocator hands out a different block each time.
    for (i = 0; i < WIIUAUDIO_DSP_ALLOC_TRIES; i++) {
        Uint8 *p = (Uint8 *)SDL_aligned_alloc(0x40, size);
        if (!p) {
            break;
        }
        if (WIIUAUDIO_IsDspSafe(p, size)) {
            result = p;
            break;
        }
        rejected[num_rejected++] = p;
    }
    while (num_rejected > 0) {
        SDL_aligned_free(rejected[--num_rejected]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// SDL audio driver hooks.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// HOME menu (ProcUI). The video driver calls this listener when the application
// loses the foreground (acquired == false, from inside ProcUIDrawDoneRelease())
// and when it gets it back. Policy: the voices are paused and resume from the
// same position, the audio thread and the device stay open, nothing is
// flushed. The wait confirms the voices are stopped before the foreground goes;
// if AX stops calling the frame callback first, the timeout ends the wait and
// the voices stop (or resume) at the next callback.
// ---------------------------------------------------------------------------

#ifdef SDL_VIDEO_DRIVER_WIIU
static void WIIUAUDIO_ForegroundChanged(bool acquired, void *userdata)
{
    struct SDL_PrivateAudioData *h = (struct SDL_PrivateAudioData *)userdata;

    if (h != wiiu_active_device) {
        return;
    }
    if (acquired) {
        StoreShared(&h->pause_request, 0);
    } else {
        OSResetEvent(&h->pause_ack);
        StoreShared(&h->pause_request, 1);
        OSWaitEventWithTimeout(&h->pause_ack, OSMillisecondsToTicks(WIIUAUDIO_PAUSE_TIMEOUT_MS));
    }
}
#endif

static Uint32 WIIUAUDIO_RoundFrames(int requested, Uint32 ax_frame)
{
    Uint32 frames = (requested > 0) ? (Uint32)requested : 0;

    // At least two AX frames per buffer, and a whole number of them, so that
    // buffer boundaries fall on AX frame boundaries.
    if (frames < 2 * ax_frame) {
        frames = 2 * ax_frame;
    }
    return ((frames + ax_frame - 1) / ax_frame) * ax_frame;
}

static bool WIIUAUDIO_OpenDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h;
    int result = 0;

    if (wiiu_active_device) {
        return SDL_SetError("The Wii U audio device is already open");
    }

    h = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*h));
    if (!h) {
        return false;
    }
    // From here on, CloseDevice() cleans up after any failure.
    device->hidden = h;

    if (!WIIUAUDIO_RunOnAXCore(WIIUAUDIO_InitAX, h, &result)) {
        return false;
    }
    if (result != 0 || h->ax_rate == 0 || h->ax_frame == 0) {
        return SDL_SetError("Couldn't initialize AX");
    }

    // Fixed format. Anything the app asked for is converted by SDL's streams.
    device->spec.format = SDL_AUDIO_S16BE;
    device->spec.channels = WIIUAUDIO_CHANNELS;
    device->spec.freq = (int)h->ax_rate;
    h->frames = WIIUAUDIO_RoundFrames(device->sample_frames, h->ax_frame);
    device->sample_frames = (int)h->frames;
    SDL_UpdatedAudioDeviceFormat(device);

    OSInitEvent(&h->wake, false, OS_EVENT_MODE_AUTO);
    OSInitEvent(&h->pause_ack, false, OS_EVENT_MODE_AUTO);

    // Ring for the DSP: WIIUAUDIO_CHANNELS regions of NUM_BUFFERS buffers.
    h->ring_size = (size_t)WIIUAUDIO_CHANNELS * WIIUAUDIO_NUM_BUFFERS * h->frames * sizeof(Sint16);
    h->ring = WIIUAUDIO_AllocDspBuffer(h->ring_size);
    if (!h->ring) {
        return SDL_SetError("Couldn't allocate an AX-accessible mix buffer");
    }
    SDL_memset(h->ring, 0, h->ring_size); // silence
    DCStoreRange(h->ring, (uint32_t)h->ring_size);

    h->staging = (Uint8 *)SDL_calloc(1, (size_t)device->buffer_size);
    if (!h->staging) {
        return false;
    }

    // SDL fills buffer 1 first (it does not wait before its first buffer); the
    // voices start on buffer 0, which is silence.
    h->playing = 0;
    h->last_playing = 0;
    h->rendering = 1 % WIIUAUDIO_NUM_BUFFERS;

    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                "wiiu audio: AX %u Hz, %u samples per AX frame; %u frames x %d buffers; ring %p, physical 0x%08x",
                (unsigned)h->ax_rate, (unsigned)h->ax_frame, (unsigned)h->frames, WIIUAUDIO_NUM_BUFFERS,
                (void *)h->ring, (unsigned)OSEffectiveToPhysical((uint32_t)(uintptr_t)h->ring));

    if (!WIIUAUDIO_RunOnAXCore(WIIUAUDIO_StartVoices, h, &result)) {
        return false;
    }
    if (result == -1) {
        return SDL_SetError("Couldn't acquire an AX voice");
    } else if (result != 0) {
        return SDL_SetError("Couldn't register the AX frame callback");
    }
#ifdef SDL_VIDEO_DRIVER_WIIU
    // Pause across the HOME menu. Cannot fail in a way that matters: the list is
    // small and a full list only means no pause (the OS then decides).
    if (WIIU_AddForegroundListener(WIIUAUDIO_ForegroundChanged, h)) {
        h->listener_registered = true;
    }
#endif
    return true;
}

static bool WIIUAUDIO_WaitDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h = device->hidden;

    // Wait until the voices have left the buffer SDL would fill next. The frame
    // callback signals when they move on; the timeout is only there so that
    // device shutdown is noticed, and so that a missed signal costs little.
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        if (h->rendering != LoadShared(&h->playing)) {
            return true;
        }
        OSWaitEventWithTimeout(&h->wake, OSMillisecondsToTicks(WIIUAUDIO_WAIT_TIMEOUT_MS));
    }

    // Shutting down: SDL stops feeding the device, so the voices running on into
    // buffers nobody refilled from here on is not an underrun.
    StoreShared(&h->closing, 1);
    return true;
}

static Uint8 *WIIUAUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    *buffer_size = device->buffer_size;
    return device->hidden->staging;
}

static bool WIIUAUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buflen)
{
    struct SDL_PrivateAudioData *h = device->hidden;
    const Uint32 frames = h->frames;
    const Uint32 b = h->rendering;
    Uint16 *left = (Uint16 *)(void *)(h->ring + ((size_t)(0 * WIIUAUDIO_NUM_BUFFERS + b) * frames * sizeof(Uint16)));
    Uint16 *right = (Uint16 *)(void *)(h->ring + ((size_t)(1 * WIIUAUDIO_NUM_BUFFERS + b) * frames * sizeof(Uint16)));
    const Uint16 *src = (const Uint16 *)(const void *)buffer;
    Uint32 i;

    SDL_assert(buflen == (int)(frames * WIIUAUDIO_CHANNELS * sizeof(Uint16)));
    (void)buflen;

    // Samples are S16BE on both sides, so they are moved as opaque 16-bit words.
    for (i = 0; i < frames; i++) {
        left[i] = src[2 * i];
        right[i] = src[2 * i + 1];
    }

    // The DSP reads memory directly, so push the data out of the CPU cache.
    DCStoreRange(left, frames * sizeof(Uint16));
    DCStoreRange(right, frames * sizeof(Uint16));

    // Publish the buffer (the barrier orders it after the data above), then move
    // on to the next one. This does not block, as SDL requires.
    StoreShared(&h->slot_commits[b], h->slot_commits[b] + 1);
    h->rendering = (b + 1) % WIIUAUDIO_NUM_BUFFERS;
    return true;
}

static void WIIUAUDIO_CloseDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h = device->hidden;
    int result;

    // Also called when OpenDevice() failed part way, so everything is optional.
    if (!h) {
        return;
    }

#ifdef SDL_VIDEO_DRIVER_WIIU
    if (h->listener_registered) {
        WIIU_RemoveForegroundListener(WIIUAUDIO_ForegroundChanged, h);
        h->listener_registered = false;
    }
#endif

    // The callback returns at once from here on.
    wiiu_active_device = NULL;
    OSMemoryBarrier();

    if (h->callback_registered || h->voice[0] || h->voice[1] || h->we_initialized_ax) {
        WIIUAUDIO_RunOnAXCore(WIIUAUDIO_StopAX, h, &result);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, "wiiu audio: closed after %u buffer(s) entered, %u underrun(s)",
                (unsigned)h->entered, (unsigned)h->underruns);

    SDL_aligned_free(h->ring);
    SDL_free(h->staging);
    SDL_free(h);
    device->hidden = NULL;
}

static bool WIIUAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = WIIUAUDIO_OpenDevice;
    impl->WaitDevice = WIIUAUDIO_WaitDevice;
    impl->GetDeviceBuf = WIIUAUDIO_GetDeviceBuf;
    impl->PlayDevice = WIIUAUDIO_PlayDevice;
    impl->CloseDevice = WIIUAUDIO_CloseDevice;

    // No ThreadInit: SDL's default puts the audio thread at TIME_CRITICAL, which
    // is coreinit priority 3 here. Nothing in this driver spins; the thread
    // sleeps in WaitDevice for all but a fraction of a millisecond per buffer.

    impl->OnlyHasDefaultPlaybackDevice = true; // TV and GamePad together
    impl->HasRecordingSupport = false;         // capture is a non-goal
    return true;
}

AudioBootStrap WIIUAUDIO_bootstrap = {
    WIIUAUDIO_DRIVER_NAME, "Wii U AX audio driver", WIIUAUDIO_Init, false, false
};

#endif // SDL_AUDIO_DRIVER_WIIU
