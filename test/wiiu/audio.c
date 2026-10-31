/*
  AX audio tests for the SDL3 Wii U port

  Automatic checks print PASS or FAIL through SDL_Log (OSReport on Wii U); the
  return value is 0 if all passed. Some things can only be judged by ear, and the
  program says when.

  What to listen for (on the TV and on the GamePad):
    phase 1  left channel only   (440 Hz)
    phase 2  right channel only  (660 Hz)
    phase 3  both together
    phase 4  the looped WAV, if one was found (see WAV_PATHS)
  Any clicks, stutter or repeating fragments are an underrun. The driver also logs
  "wiiu audio: closed after N buffer(s) entered, M underrun(s)" when the device
  closes; M should be 0.

  For the several-minutes soak, build with -DAUDIO_TONE_SECONDS=300 (or more).
*/
#include <SDL3/SDL.h>

#include "console.h"

#include <math.h>

#ifndef AUDIO_TONE_SECONDS
#define AUDIO_TONE_SECONDS 12 // split over the three tone phases
#endif
#ifndef AUDIO_WAV_SECONDS
#define AUDIO_WAV_SECONDS 12
#endif

static const char *const WAV_PATHS[] = {
    "test.wav",
    "fs:/vol/external01/wiiu/apps/sdl3-wiiu-audio/test.wav",
};

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// ------------------------------------------------------------ tone source

#define TONE_RATE 48000

static volatile int tone_mode; // 1 left, 2 right, 3 both
static double phase_l, phase_r;
static volatile Uint64 tone_frames_made;

static void SDLCALL tone_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
    float buf[512 * 2];
    int frames = additional / (int)(2 * sizeof(float));

    (void)userdata;
    (void)total;
    while (frames > 0) {
        const int n = SDL_min(frames, 512);
        const int mode = tone_mode;
        int i;
        for (i = 0; i < n; i++) {
            const float l = (float)(0.25 * sin(phase_l));
            const float r = (float)(0.25 * sin(phase_r));
            phase_l += 2.0 * M_PI * 440.0 / TONE_RATE;
            phase_r += 2.0 * M_PI * 660.0 / TONE_RATE;
            buf[2 * i] = (mode & 1) ? l : 0.0f;
            buf[2 * i + 1] = (mode & 2) ? r : 0.0f;
        }
        SDL_PutAudioStreamData(stream, buf, n * 2 * (int)sizeof(float));
        tone_frames_made += (Uint64)n;
        frames -= n;
    }
}

// ------------------------------------------------------------------ tests

static void test_driver(void)
{
    int count = 0;
    SDL_AudioDeviceID *playback;
    SDL_AudioDeviceID *recording;

    check("SDL_Init(SDL_INIT_AUDIO)", SDL_Init(SDL_INIT_AUDIO));
    check("SDL_GetCurrentAudioDriver() == \"wiiu\"",
          SDL_GetCurrentAudioDriver() && SDL_strcmp(SDL_GetCurrentAudioDriver(), "wiiu") == 0);

    playback = SDL_GetAudioPlaybackDevices(&count);
    check("exactly one playback device", playback && count == 1);
    if (playback && count > 0) {
        SDL_Log("INFO playback device: \"%s\"", SDL_GetAudioDeviceName(playback[0]));
    }
    SDL_free(playback);

    recording = SDL_GetAudioRecordingDevices(&count);
    check("no recording devices", count == 0);
    SDL_free(recording);
}

static SDL_AudioStream *open_tone(void)
{
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = TONE_RATE;
    return SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, tone_callback, NULL);
}

static void test_tone(void)
{
    SDL_AudioStream *stream;
    SDL_AudioSpec spec;
    int frames = 0;
    Uint64 start;
    Uint64 made_start;
    Uint64 elapsed_ms;
    Uint64 made;
    int phase;

    stream = open_tone();
    check("open the default device with a stereo F32 48 kHz stream", stream != NULL);
    if (!stream) {
        SDL_Log("INFO %s", SDL_GetError());
        return;
    }

    check("device format is S16BE, 2 channels", SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &spec, &frames) &&
                                                    spec.format == SDL_AUDIO_S16BE && spec.channels == 2);
    SDL_Log("INFO device: %d Hz, %d sample frames (%d ms per buffer)", spec.freq, frames, frames * 1000 / spec.freq);

    tone_mode = 1;
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Delay(300); // let it get going before measuring
    start = SDL_GetTicksNS();
    made_start = tone_frames_made;

    for (phase = 1; phase <= 3; phase++) {
        SDL_Log("INFO tone phase %d: %s", phase, phase == 1 ? "left only" : phase == 2 ? "right only" : "both");
        tone_mode = phase;
        SDL_Delay((Uint32)(AUDIO_TONE_SECONDS * 1000 / 3));
    }

    // The device must consume audio at the real 48 kHz. If AX ran fast or slow
    // (or the device stalled), the frames it asked for would differ from the
    // wall clock. The queue depth is a few buffers, so allow 3%.
    elapsed_ms = (SDL_GetTicksNS() - start) / SDL_NS_PER_MS;
    made = tone_frames_made - made_start;
    SDL_Log("INFO %llu ms elapsed, %llu frames consumed (expected about %llu)",
            (unsigned long long)elapsed_ms, (unsigned long long)made, (unsigned long long)(elapsed_ms * TONE_RATE / 1000));
    check("device consumed audio at 48 kHz within 3%",
          made > elapsed_ms * TONE_RATE / 1000 * 97 / 100 && made < elapsed_ms * TONE_RATE / 1000 * 103 / 100);

    // Pause, resume.
    check("pause", SDL_PauseAudioStreamDevice(stream));
    SDL_Delay(300);
    check("resume", SDL_ResumeAudioStreamDevice(stream));
    SDL_Delay(300);

    SDL_DestroyAudioStream(stream); // logs the underrun count (see header comment)
    SDL_Log("INFO check the underrun line above: it should say 0");
}

static void test_wav(void)
{
    SDL_AudioSpec spec;
    Uint8 *data = NULL;
    Uint32 len = 0;
    SDL_AudioStream *stream;
    size_t i;
    Uint64 end;

    for (i = 0; i < SDL_arraysize(WAV_PATHS); i++) {
        if (SDL_LoadWAV(WAV_PATHS[i], &spec, &data, &len)) {
            SDL_Log("INFO loaded %s (%u bytes, %d Hz, %d ch, format 0x%x)", WAV_PATHS[i], (unsigned)len, spec.freq, spec.channels, (unsigned)spec.format);
            break;
        }
    }
    if (!data) {
        SDL_Log("INFO no test.wav found, skipping the looped WAV (looked in cwd and the SD card app folder)");
        return;
    }

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    check("open a stream in the WAV's own format", stream != NULL);
    if (stream) {
        SDL_Log("INFO looping the WAV for %d s", AUDIO_WAV_SECONDS);
        SDL_ResumeAudioStreamDevice(stream);
        end = SDL_GetTicks() + (Uint64)AUDIO_WAV_SECONDS * 1000;
        while (SDL_GetTicks() < end) {
            // Keep about a second queued; SDL converts to the device format.
            if (SDL_GetAudioStreamQueued(stream) < (int)len) {
                SDL_PutAudioStreamData(stream, data, (int)len);
            }
            SDL_Delay(50);
        }
        SDL_DestroyAudioStream(stream);
    }
    SDL_free(data);
}

static void test_reopen(void)
{
    int i;
    bool ok = true;

    // AX is initialised on open and quit on close; do it a few times.
    for (i = 0; i < 5; i++) {
        SDL_AudioStream *stream = open_tone();
        if (!stream) {
            ok = false;
            SDL_Log("INFO reopen %d: %s", i, SDL_GetError());
            break;
        }
        tone_mode = 3;
        SDL_ResumeAudioStreamDevice(stream);
        SDL_Delay(400);
        SDL_DestroyAudioStream(stream);
    }
    check("open and close the device 5 times", ok);
}

int tool_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetLogPriority(SDL_LOG_CATEGORY_AUDIO, SDL_LOG_PRIORITY_INFO);
    SDL_Log("SDL %d.%d.%d on %s: P2 audio tests", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION, SDL_GetPlatform());

    test_driver();
    test_tone();
    test_wav();
    test_reopen();

    SDL_Quit();
    SDL_Log("Done: %d failure(s)", failures);
    return failures ? 1 : 0;
}
