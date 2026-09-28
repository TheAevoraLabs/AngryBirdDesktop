#include "audio.h"
#include "jni/jni.h"
#include "jni/jni_fake.h"
#include "fusion/fusion.h"
#include "common/util.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <vector>

typedef jint (*fn_mixdata)(JNIEnv env, void *thiz, void *peer, jbyteArray buf, jint count);

static fn_mixdata s_mix = nullptr;
static void *s_thiz = nullptr;
static SDL_AudioStream *s_audio_stream = nullptr;
static int s_open = 0;
static std::vector<uint8_t> s_mix_buffer;

void audio_set_mixer(void *native_mix_data_addr, void *thiz) {
    s_mix = (fn_mixdata)native_mix_data_addr;
    s_thiz = thiz;
    printf("[Audio] Registered nativeMixData @ %p (thiz=%p)\n", native_mix_data_addr, thiz);
}

void audio_poll(void) {
    void *peer = fusion_audio_peer();
    if (!s_open && fusion_audio_wanted() && s_mix && peer) {
        int rate = fusion_audio_rate();
        int ch = fusion_audio_channels();
        int bits = fusion_audio_bits();

        SDL_AudioSpec spec;
        spec.format = (bits == 8) ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        spec.channels = ch;
        spec.freq = rate;

        s_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (s_audio_stream) {
            SDL_ResumeAudioStreamDevice(s_audio_stream);
            s_open = 1;
            s_mix_buffer.resize(4096 * ch);
            printf("[Audio] SDL3 Audio device stream opened: %d Hz, %d channels, %d bits\n", rate, ch, bits);
        }
    }

    if (s_open && s_audio_stream && s_mix && peer) {
        int queued = SDL_GetAudioStreamQueued(s_audio_stream);
        int target_queued = (fusion_audio_rate() * fusion_audio_channels() * (fusion_audio_bits() / 8)) / 20; // 50ms buffer

        while (queued < target_queued) {
            jbyteArray arr = jni_wrap_bytearray(s_mix_buffer.data(), (int)s_mix_buffer.size());
            s_mix(fake_env, s_thiz, peer, arr, (jint)s_mix_buffer.size());
            SDL_PutAudioStreamData(s_audio_stream, s_mix_buffer.data(), (int)s_mix_buffer.size());
            jni_free_wrapper(arr);
            queued += (int)s_mix_buffer.size();
        }
    }
}

void audio_shutdown(void) {
    if (s_audio_stream) {
        SDL_DestroyAudioStream(s_audio_stream);
        s_audio_stream = nullptr;
    }
    s_open = 0;
}
