#ifndef ANGRYBIRDS_AUDIO_H
#define ANGRYBIRDS_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_init(int sampleRate, int channels);
void audio_shutdown();
void audio_set_mix_context(int64_t context);
void audio_tick();

#ifdef __cplusplus
}
#endif

#endif // ANGRYBIRDS_AUDIO_H
