#include "audio.h"
#include "../jni/jni_bridge.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <vector>

static SDL_AudioStream* g_audio_stream = nullptr;
static int64_t g_mix_context = 0;
static int g_sample_rate = 44100;
static int g_channels = 2;
static std::vector<uint8_t> g_pcm_buffer;

extern nativeMixData_t g_nativeMixData;

bool audio_init(int sampleRate, int channels) {
    g_sample_rate = sampleRate > 0 ? sampleRate : 44100;
    g_channels = channels > 0 ? channels : 2;

    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = g_channels;
    spec.freq = g_sample_rate;

    g_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audio_stream) {
        printf("[Audio] Warning: Failed to open SDL3 audio device: %s\n", SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(g_audio_stream);
    g_pcm_buffer.resize(4096 * g_channels);
    printf("[Audio] SDL3 Audio initialized: %d Hz, %d channels\n", g_sample_rate, g_channels);
    return true;
}

void audio_shutdown() {
    if (g_audio_stream) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = nullptr;
    }
}

void audio_set_mix_context(int64_t context) {
    g_mix_context = context;
}

void audio_tick() {
    if (!g_audio_stream || !g_nativeMixData || g_mix_context == 0) return;

    // Check how much audio is currently queued
    int queued = SDL_GetAudioStreamQueued(g_audio_stream);
    int target_queued = (g_sample_rate * g_channels * 2) / 20; // 50ms buffer

    while (queued < target_queued) {
        JNIEnv* env = jni_get_env();
        jbyteArray arr = env->NewByteArray((jsize)g_pcm_buffer.size());
        
        g_nativeMixData(env, nullptr, (jlong)g_mix_context, arr, (jint)g_pcm_buffer.size());
        
        jbyte* elems = env->GetByteArrayElements(arr, nullptr);
        if (elems) {
            SDL_PutAudioStreamData(g_audio_stream, elems, (int)g_pcm_buffer.size());
            env->ReleaseByteArrayElements(arr, elems, 0);
        }
        queued += (int)g_pcm_buffer.size();
    }
}
