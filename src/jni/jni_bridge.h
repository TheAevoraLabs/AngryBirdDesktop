#ifndef ANGRYBIRDS_JNI_BRIDGE_H
#define ANGRYBIRDS_JNI_BRIDGE_H

#include "jni.h"
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize JavaVM and JNIEnv singletons
void jni_bridge_init();

// Retrieve global VM and Env instances
JavaVM* jni_get_java_vm();
JNIEnv* jni_get_env();

// Native function pointer types
typedef jint (*JNI_OnLoad_t)(JavaVM* vm, void* reserved);
typedef void (*nativeConfig_t)(JNIEnv* env, jobject thiz, jstring configJson);
typedef jint (*nativeGetPossibleOrientations_t)(JNIEnv* env, jobject thiz);
typedef void (*nativeInit_t)(JNIEnv* env, jobject thiz, jint width, jint height);
typedef void (*nativeDeinit_t)(JNIEnv* env, jobject thiz);
typedef void (*nativePause_t)(JNIEnv* env, jobject thiz);
typedef void (*nativeResume_t)(JNIEnv* env, jobject thiz);
typedef jboolean (*nativeResize_t)(JNIEnv* env, jobject thiz, jint width, jint height);
typedef jboolean (*nativeUpdate_t)(JNIEnv* env, jobject thiz);
typedef jboolean (*nativeRender_t)(JNIEnv* env, jobject thiz);
typedef void (*nativeFrameClear_t)(JNIEnv* env, jobject thiz);
typedef void (*nativeInput_t)(JNIEnv* env, jobject thiz, jint action, jfloat x, jfloat y, jint pointerId);
typedef void (*nativeKeyInput_t)(JNIEnv* env, jobject thiz, jint keyCode, jint unicode, jint action, jint metaState);
typedef void (*nativeMixData_t)(JNIEnv* env, jobject thiz, jlong context, jbyteArray buffer, jint length);
typedef void (*onVideoEnded_t)(JNIEnv* env, jobject thiz, jlong nativePtr, jboolean completed, jint position, jlong duration);

extern onVideoEnded_t g_onVideoEnded;

#ifdef __cplusplus
}
#endif

#endif // ANGRYBIRDS_JNI_BRIDGE_H
