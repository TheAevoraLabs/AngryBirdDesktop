/* audio.h -- SDL3 audio bridge to the Fusion engine's own mixer.
 *
 * The engine mixes its audio itself. Its Java layer hands it a native peer
 * pointer and then pulls PCM out of:
 *
 *   Java_com_rovio_fusion_AudioOutput_nativeMixData(JNIEnv*, jobject,
 *                                                   jlong peer, jbyteArray, jint bytes)
 *
 * On Android a Java AudioTrack thread called that to fill a byte[] and wrote it
 * to the device. Here an SDL3 audio stream callback does the same job: when the
 * device wants more samples we call nativeMixData and push the result.
 *
 * Two ABI details matter on 32-bit x86 and are easy to get wrong:
 *   - `peer` is a jlong, i.e. TWO 32-bit stack slots. Declaring it as a plain
 *     pointer silently misaligns every argument that follows it.
 *   - `bytes` is a BYTE count (AudioOutput.startOutput passes byte[].length),
 *     not a frame or sample count.
 */

#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the resolved nativeMixData entry point and the AudioOutput receiver
 * object. `peer` and the output format are negotiated later, when the engine
 * constructs its AudioOutput (see fusion.c / jni_fake.c). */
void audio_set_mixer(void *native_mix_data_addr, void *thiz);

/* Call once per frame. Opens the SDL device as soon as the engine has a peer
 * and has asked for output, and pauses/resumes it as the game demands. */
void audio_poll(void);

void audio_shutdown(void);

/* Diagnostics: how many times the engine mixer has run, bytes produced. */
unsigned long audio_mix_calls(void);
unsigned long audio_bytes_mixed(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
