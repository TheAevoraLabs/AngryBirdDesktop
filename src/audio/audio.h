#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

void audio_set_mixer(void *native_mix_data_addr, void *thiz);
void audio_poll(void);
void audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_H
