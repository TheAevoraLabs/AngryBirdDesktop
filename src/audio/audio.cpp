/* audio.cpp -- SDL3 pull-audio bridge to the Fusion engine's mixer. See audio.h.
 *
 * The mixer is called from SDL's audio thread, which is exactly where Android
 * called it from (AudioOutput's own AudioTrack notification thread), so that is
 * a supported path rather than an accident.
 */

#include "audio.h"
#include "jni/jni.h"
#include "jni/jni_fake.h"
#include "fusion/fusion.h"
#include "common/util.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

/* nativeMixData(JNIEnv*, jobject, jlong peer, jbyteArray, jint bytes)
 * `peer` MUST be jlong: on x86-32 it occupies two stack slots. */
typedef jint (*fn_mixdata)(JNIEnv env, void *thiz, jlong peer, jbyteArray buf, jint bytes);

static fn_mixdata s_mix  = nullptr;
static void      *s_thiz = nullptr;

static SDL_AudioStream *s_stream = nullptr;
static int   s_open        = 0;
static int   s_running     = 0;
static int   s_frame_bytes = 4;      /* channels * bytes-per-sample */

static int   s_rate = 44100, s_channels = 2, s_bits = 16;

static std::vector<uint8_t> s_chunk;  /* fixed-size mix scratch, shared with engine */
static std::vector<uint8_t> s_silence;

static unsigned long s_mix_calls = 0;
static unsigned long s_bytes_mixed = 0;
static int s_announced_first_mix = 0;
static int s_announced_no_peer = 0;

/* 1024 frames per mixer call (~23 ms at 44.1 kHz) -- the same order as the
 * period Android's AudioTrack asked for. */
#define AUDIO_CHUNK_FRAMES 1024

void audio_set_mixer(void *native_mix_data_addr, void *thiz) {
    s_mix  = (fn_mixdata)native_mix_data_addr;
    s_thiz = thiz;
    debugPrintf("[Audio] Registered nativeMixData @ %p (thiz=%p)\n",
                native_mix_data_addr, thiz);
}

/* ---------------------------------------------------------------- callback */

static void SDLCALL audio_stream_cb(void *userdata, SDL_AudioStream *stream,
                                    int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0) return;

    void *peer = fusion_audio_peer();
    if (!s_mix || !peer) {
        if (!s_announced_no_peer) {
            debugPrintf("[Audio] device wants %d bytes but mixer=%p peer=%p -- silence\n",
                        additional_amount, (void *)s_mix, peer);
            s_announced_no_peer = 1;
        }
        return; /* leave the stream empty: SDL plays silence */
    }

    int remaining = additional_amount;
    while (remaining >= s_frame_bytes && s_frame_bytes > 0) {
        int want = (int)s_chunk.size();
        if (want > remaining) want = (remaining / s_frame_bytes) * s_frame_bytes;
        if (want < s_frame_bytes) break;

        jbyteArray arr = jni_wrap_bytearray(s_chunk.data(), want);
        s_mix(fake_env, s_thiz, (jlong)(intptr_t)peer, arr, (jint)want);
        jni_free_wrapper(arr);

        SDL_PutAudioStreamData(stream, s_chunk.data(), want);

        s_mix_calls++;
        s_bytes_mixed += (unsigned long)want;
        remaining -= want;

        if (!s_announced_first_mix) {
            debugPrintf("[Audio] first mix: %d bytes from peer %p\n", want, peer);
            s_announced_first_mix = 1;
        }
    }
}

/* -------------------------------------------------------------- device open */

static void audio_open(void) {
    int rate = fusion_audio_rate();
    int ch   = fusion_audio_channels();
    int bits = fusion_audio_bits();

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format   = (bits == 8) ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
    spec.channels = (Uint8)ch;
    spec.freq     = rate;

    /* A callback-driven device stream: SDL asks us for data whenever the device
     * needs it, so playback does not depend on the frame rate of the game loop. */
    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                  audio_stream_cb, nullptr);
    if (!stream) {
        debugPrintf("[Audio] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        return;
    }

    /* Confirm the format we will actually be handing the client-side stream. */
    SDL_AudioSpec src, dst;
    if (SDL_GetAudioStreamFormat(stream, &src, &dst)) {
        spec = src;
    }

    s_frame_bytes = (int)spec.channels * (SDL_AUDIO_BITSIZE(spec.format) / 8);
    if (s_frame_bytes <= 0) s_frame_bytes = 4;

    s_rate = spec.freq; s_channels = spec.channels;
    s_bits = SDL_AUDIO_BITSIZE(spec.format);

    s_chunk.assign((size_t)AUDIO_CHUNK_FRAMES * s_frame_bytes, 0);
    s_silence.assign((size_t)AUDIO_CHUNK_FRAMES * s_frame_bytes, 0);

    s_stream = stream;
    s_open = 1;

    SDL_ResumeAudioStreamDevice(s_stream);
    s_running = 1;

    debugPrintf("[Audio] device open: %d Hz, %d ch, fmt=0x%04x, frame=%d bytes, chunk=%zu\n",
                s_rate, s_channels, (unsigned)spec.format, s_frame_bytes, s_chunk.size());
}

/* --------------------------------------------------------------------- API */

void audio_poll(void) {
    const int wanted = fusion_audio_wanted();
    void *peer = fusion_audio_peer();

    if (!s_open) {
        /* Open lazily: the peer struct carries the exact format, and the mixer
         * cannot run before the engine hands us that peer anyway. */
        if (wanted && s_mix && peer) {
            debugPrintf("[Audio] engine requested output; opening device\n");
            audio_open();
        }
        return;
    }

    if (s_stream && wanted != s_running) {
        if (wanted) {
            SDL_ResumeAudioStreamDevice(s_stream);
            debugPrintf("[Audio] resumed\n");
        } else {
            SDL_PauseAudioStreamDevice(s_stream);
            debugPrintf("[Audio] paused\n");
        }
        s_running = wanted;
    }
}

void audio_shutdown(void) {
    if (s_stream) {
        SDL_DestroyAudioStream(s_stream);   /* joins the audio thread + its callback */
        s_stream = nullptr;
    }
    s_open = 0;
    s_running = 0;
}

unsigned long audio_mix_calls(void)   { return s_mix_calls; }
unsigned long audio_bytes_mixed(void) { return s_bytes_mixed; }
