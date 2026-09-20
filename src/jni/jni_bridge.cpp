#include "jni_bridge.h"
#include "../android/asset_manager.h"
#include "../android/log.h"
#include "../audio/audio.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <string>
#include <unordered_map>
#include <vector>

static struct JNINativeInterface_ g_native_interface;
static struct JNIInvokeInterface_ g_invoke_interface;
static const struct JNINativeInterface_* g_env_ptr = &g_native_interface;
static const struct JNIInvokeInterface_* g_vm_ptr = &g_invoke_interface;

struct FakeJavaClass {
    std::string name;
};

struct FakeJavaMethod {
    std::string className;
    std::string name;
    std::string signature;
    bool isStatic;
};

struct FakeJavaField {
    std::string className;
    std::string name;
    std::string signature;
    bool isStatic;
};

static std::unordered_map<std::string, FakeJavaClass*> g_classes;
static std::vector<FakeJavaMethod*> g_methods;
static std::vector<FakeJavaField*> g_fields;

onVideoEnded_t g_onVideoEnded = nullptr;
static jlong g_video_native_ptr = 0;

// --- String allocation helper ---
static jstring create_fake_string(const char* utf) {
    if (!utf) return nullptr;
    return (jstring)strdup(utf);
}

// --- JNIEnv Callbacks ---
static jint JNICALL GetVersion_impl(JNIEnv* env) {
    (void)env;
    return JNI_VERSION_1_6;
}

static jclass JNICALL FindClass_impl(JNIEnv* env, const char* name) {
    (void)env;
    if (!name) return nullptr;
    std::string sname(name);
    if (g_classes.find(sname) == g_classes.end()) {
        FakeJavaClass* cls = new FakeJavaClass{ sname };
        g_classes[sname] = cls;
    }
    return (jclass)g_classes[sname];
}

static jmethodID JNICALL GetMethodID_impl(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    (void)env;
    FakeJavaClass* cls = (FakeJavaClass*)clazz;
    std::string cname = cls ? cls->name : "UnknownClass";
    FakeJavaMethod* m = new FakeJavaMethod{ cname, name ? name : "", sig ? sig : "", false };
    g_methods.push_back(m);
    return (jmethodID)m;
}

static jmethodID JNICALL GetStaticMethodID_impl(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    (void)env;
    FakeJavaClass* cls = (FakeJavaClass*)clazz;
    std::string cname = cls ? cls->name : "UnknownClass";
    FakeJavaMethod* m = new FakeJavaMethod{ cname, name ? name : "", sig ? sig : "", true };
    g_methods.push_back(m);
    return (jmethodID)m;
}

static jfieldID JNICALL GetFieldID_impl(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    (void)env;
    FakeJavaClass* cls = (FakeJavaClass*)clazz;
    std::string cname = cls ? cls->name : "UnknownClass";
    FakeJavaField* f = new FakeJavaField{ cname, name ? name : "", sig ? sig : "", false };
    g_fields.push_back(f);
    return (jfieldID)f;
}

static jfieldID JNICALL GetStaticFieldID_impl(JNIEnv* env, jclass clazz, const char* name, const char* sig) {
    (void)env;
    FakeJavaClass* cls = (FakeJavaClass*)clazz;
    std::string cname = cls ? cls->name : "UnknownClass";
    FakeJavaField* f = new FakeJavaField{ cname, name ? name : "", sig ? sig : "", true };
    g_fields.push_back(f);
    return (jfieldID)f;
}

static jstring JNICALL NewStringUTF_impl(JNIEnv* env, const char* bytes) {
    (void)env;
    return create_fake_string(bytes ? bytes : "");
}

static jsize JNICALL GetStringLength_impl(JNIEnv* env, jstring str) {
    (void)env;
    if (!str) return 0;
    return (jsize)strlen((const char*)str);
}

static jsize JNICALL GetStringUTFLength_impl(JNIEnv* env, jstring str) {
    (void)env;
    if (!str) return 0;
    return (jsize)strlen((const char*)str);
}

static const char* JNICALL GetStringUTFChars_impl(JNIEnv* env, jstring str, jboolean* isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    return (const char*)str;
}

static void JNICALL ReleaseStringUTFChars_impl(JNIEnv* env, jstring str, const char* utf) {
    (void)env;
    (void)str;
    (void)utf;
}

static void JNICALL GetStringRegion_impl(JNIEnv* env, jstring str, jsize start, jsize len, jchar* buf) {
    (void)env;
    if (!str || !buf) return;
    const char* s = (const char*)str;
    for (jsize i = 0; i < len; i++) {
        buf[i] = (jchar)(unsigned char)s[start + i];
    }
}

static void JNICALL GetStringUTFRegion_impl(JNIEnv* env, jstring str, jsize start, jsize len, char* buf) {
    (void)env;
    if (!str || !buf || len <= 0) return;
    const char* s = (const char*)str;
    size_t slen = strlen(s);
    if ((size_t)start < slen) {
        size_t to_copy = ((size_t)(start + len) > slen) ? (slen - start) : (size_t)len;
        memcpy(buf, s + start, to_copy);
    }
}

static jobject JNICALL CallStaticObjectMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticObjectMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "getConnectivityManager" || m->name == "getActiveNetwork" || m->name == "getLinkProperties") {
            return nullptr;
        } else if (m->name == "getPathToFileCacheDirectory") {
            return (jobject)create_fake_string("./save/");
        } else if (m->name == "getDeviceModel" || m->name == "getModel") {
            return (jobject)create_fake_string("Linux Desktop PC");
        } else if (m->name == "getOSVersion") {
            return (jobject)create_fake_string("Android 9.0 (PC Shim)");
        } else if (m->name == "getDeviceLanguage" || m->name == "getLanguage" || m->name == "systemLocale" || m->name == "deviceLocale") {
            return (jobject)create_fake_string("en_US");
        } else if (m->name == "getDeviceCountry" || m->name == "getCountry") {
            return (jobject)create_fake_string("US");
        } else if (m->name == "getVersionName" || m->name == "getApplicationVersionString") {
            return (jobject)create_fake_string("8.0.3");
        } else if (m->name == "getUniqueId" || m->name == "getAndroidId" || m->name == "advertisingId") {
            return (jobject)create_fake_string("3f2504e0-4f89-11d3-9a0c-0305e82c3301");
        } else if (m->name == "networkType") {
            return (jobject)create_fake_string("wifi");
        } else if (m->name == "getCarrierName") {
            return (jobject)create_fake_string("");
        } else if (m->name == "userAgentString") {
            return (jobject)create_fake_string("Mozilla/5.0 (Linux; Android 9; Linux Desktop PC) AppleWebKit/537.36");
        } else if (m->name == "packageName") {
            return (jobject)create_fake_string("com.rovio.angrybirds");
        } else if (m->name == "getActivity") {
            return (jobject)(uintptr_t)0x12340001;
        } else if (m->name == "getAssets") {
            return (jobject)AAssetManager_fromJava((void*)env, nullptr);
        }
    }
    return (jobject)create_fake_string("");
}

static jobject JNICALL CallStaticObjectMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jobject res = CallStaticObjectMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jint JNICALL CallStaticIntMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticIntMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "getScreenDensity" || m->name == "getDensity" || m->name == "getDisplayDensityGroup" || m->name == "getPPI") return 160;
        if (m->name == "getScreenWidth" || m->name == "getDisplayWidth" || m->name == "getViewWidth") return 1280;
        if (m->name == "getScreenHeight" || m->name == "getDisplayHeight" || m->name == "getViewHeight") return 720;
        if (m->name == "getSdkVersion" || m->name == "getOSVersionInt" || m->name == "getAPILevel") return 28;
        if (m->name == "getDisplayConfigurationGroup") return 2; /* SCREENLAYOUT_SIZE_NORMAL */
    }
    return 0;
}

static jint JNICALL CallStaticIntMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jint res = CallStaticIntMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jboolean JNICALL CallStaticBooleanMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticBooleanMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "isTablet") return JNI_FALSE;
        if (m->name == "hasKeyboard") return JNI_TRUE;
        if (m->name == "isNetworkConnected") return JNI_TRUE;
        if (m->name == "hasSystemFeature") return JNI_TRUE;
        if (m->name == "advertisingTrackingEnabled") return JNI_TRUE;
    }
    return JNI_FALSE;
}

static jboolean JNICALL CallStaticBooleanMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jboolean res = CallStaticBooleanMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static void JNICALL CallStaticVoidMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticVoidMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
    }
}

static void JNICALL CallStaticVoidMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    CallStaticVoidMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
}

static jlong JNICALL CallStaticLongMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticLongMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "currentTimeMillis") {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            return (jlong)ts.tv_sec * 1000 + (jlong)(ts.tv_nsec / 1000000);
        }
    }
    return 0;
}

static jlong JNICALL CallStaticLongMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jlong res = CallStaticLongMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jfloat JNICALL CallStaticFloatMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticFloatMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "getBrightness") return 1.0f;
    }
    return 0.0f;
}

static jfloat JNICALL CallStaticFloatMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jfloat res = CallStaticFloatMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jdouble JNICALL CallStaticDoubleMethodV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallStaticDoubleMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
    }
    return 0.0;
}

static jdouble JNICALL CallStaticDoubleMethod_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jdouble res = CallStaticDoubleMethodV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jlong JNICALL CallLongMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallLongMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
    }
    return 0;
}

static jlong JNICALL CallLongMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jlong res = CallLongMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static jfloat JNICALL CallFloatMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallFloatMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
    }
    return 0.0f;
}

static jfloat JNICALL CallFloatMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jfloat res = CallFloatMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static jdouble JNICALL CallDoubleMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallDoubleMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
    }
    return 0.0;
}

static jdouble JNICALL CallDoubleMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jdouble res = CallDoubleMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static jobject JNICALL CallObjectMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallObjectMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "getActiveNetwork" || m->name == "getLinkProperties" || m->name == "getDnsServers" || m->name == "getDomains") {
            return nullptr;
        } else if (m->name == "getAssets") {
            return (jobject)AAssetManager_fromJava((void*)env, nullptr);
        } else if (m->name == "getAbsolutePath" || m->name == "getPath") {
            return (jobject)create_fake_string("./save/");
        } else if (m->name == "getPackageName") {
            return (jobject)create_fake_string("com.rovio.angrybirds");
        } else if (m->name == "toString") {
            return (jobject)create_fake_string("3f2504e0-4f89-11d3-9a0c-0305e82c3301");
        }
    }
    return (jobject)create_fake_string("");
}

static jobject JNICALL CallObjectMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jobject res = CallObjectMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static jint JNICALL CallIntMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallIntMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->className.find("SystemFontRenderer") != std::string::npos) {
            if (m->name == "getAscender") return 20;
            if (m->name == "getDescender") return -5;
            if (m->name == "getLeading") return 24;
            if (m->name == "getWidth") return 100;
            if (m->name == "getHeight") return 24;
        }
    }
    return 0;
}

static jint JNICALL CallIntMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jint res = CallIntMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static jboolean JNICALL CallBooleanMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallBooleanMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->name == "isVideoPlaying") return JNI_FALSE;
        if (m->name == "isReady" || m->name == "isLoaded" || m->name == "isAvailable") return JNI_TRUE;
        if (m->name == "isNetworkConnected" || m->name == "hasKeyboard" || m->name == "hasSystemFeature") return JNI_TRUE;
    }
    return JNI_FALSE;
}

static jboolean JNICALL CallBooleanMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jboolean res = CallBooleanMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
    return res;
}

static void JNICALL CallVoidMethodV_impl(JNIEnv* env, jobject obj, jmethodID methodID, va_list args) {
    (void)env;
    (void)obj;
    (void)args;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] CallVoidMethod: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->className.find("VideoPlayerBridge") != std::string::npos) {
            if (m->name.find("play") != std::string::npos || m->name.find("load") != std::string::npos) {
                printf("[Video] Video playback requested (%s), signaling onVideoEnded...\n", m->name.c_str());
                fflush(stdout);
                if (g_onVideoEnded && g_video_native_ptr) {
                    g_onVideoEnded(env, obj, g_video_native_ptr, JNI_TRUE, 0, 0);
                }
            }
        }
    }
}

static void JNICALL CallVoidMethod_impl(JNIEnv* env, jobject obj, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    CallVoidMethodV_impl(env, obj, methodID, ap);
    va_end(ap);
}

// --- Array Operations ---
struct FakeByteArray {
    jsize length;
    std::vector<jbyte> data;
};

static jbyteArray JNICALL NewByteArray_impl(JNIEnv* env, jsize length) {
    (void)env;
    FakeByteArray* arr = new FakeByteArray();
    arr->length = length;
    arr->data.resize(length, 0);
    return (jbyteArray)arr;
}

static jsize JNICALL GetArrayLength_impl(JNIEnv* env, jarray array) {
    (void)env;
    if (!array) return 0;
    FakeByteArray* arr = (FakeByteArray*)array;
    return arr->length;
}

static jbyte* JNICALL GetByteArrayElements_impl(JNIEnv* env, jbyteArray array, jboolean* isCopy) {
    (void)env;
    if (!array) return nullptr;
    if (isCopy) *isCopy = JNI_FALSE;
    FakeByteArray* arr = (FakeByteArray*)array;
    return arr->data.data();
}

static void JNICALL ReleaseByteArrayElements_impl(JNIEnv* env, jbyteArray array, jbyte* elems, jint mode) {
    (void)env;
    (void)array;
    (void)elems;
    (void)mode;
}

static void JNICALL GetByteArrayRegion_impl(JNIEnv* env, jbyteArray array, jsize start, jsize len, jbyte* buf) {
    (void)env;
    if (!array || !buf) return;
    FakeByteArray* arr = (FakeByteArray*)array;
    if (start + len <= arr->length) {
        memcpy(buf, arr->data.data() + start, len);
    }
}

static void JNICALL SetByteArrayRegion_impl(JNIEnv* env, jbyteArray array, jsize start, jsize len, const jbyte* buf) {
    (void)env;
    if (!array || !buf) return;
    FakeByteArray* arr = (FakeByteArray*)array;
    if (start + len <= arr->length) {
        memcpy(arr->data.data() + start, buf, len);
    }
}

static jobject JNICALL NewObjectV_impl(JNIEnv* env, jclass clazz, jmethodID methodID, va_list args) {
    (void)env;
    (void)clazz;
    FakeJavaMethod* m = (FakeJavaMethod*)methodID;
    if (m) {
        printf("[JNI] NewObject: %s.%s(%s)\n", m->className.c_str(), m->name.c_str(), m->signature.c_str());
        fflush(stdout);
        if (m->className.find("AudioOutput") != std::string::npos) {
            jlong nativePtr = va_arg(args, jlong);
            jint sampleRate = va_arg(args, jint);
            jint channels = va_arg(args, jint);
            printf("[Audio] AudioOutput instantiated: nativePtr=0x%llx rate=%d channels=%d\n",
                   (unsigned long long)nativePtr, (int)sampleRate, (int)channels);
            audio_init(sampleRate, channels);
            audio_set_mix_context(nativePtr);
        } else if (m->className.find("VideoPlayerBridge") != std::string::npos) {
            jlong nativePtr = va_arg(args, jlong);
            jboolean isFullscreen = (jboolean)va_arg(args, int);
            printf("[Video] VideoPlayerBridge instantiated: nativePtr=0x%llx fullscreen=%d\n",
                   (unsigned long long)nativePtr, (int)isFullscreen);
            g_video_native_ptr = nativePtr;
        }
    }
    return (jobject)(uintptr_t)0x20000001;
}

static jobject JNICALL NewObject_impl(JNIEnv* env, jclass clazz, jmethodID methodID, ...) {
    va_list ap;
    va_start(ap, methodID);
    jobject res = NewObjectV_impl(env, clazz, methodID, ap);
    va_end(ap);
    return res;
}

static jboolean IsSameObject_impl(JNIEnv* env, jobject obj1, jobject obj2) {
    (void)env;
    return obj1 == obj2 ? JNI_TRUE : JNI_FALSE;
}

static jclass JNICALL GetObjectClass_impl(JNIEnv* env, jobject obj) {
    (void)obj;
    return FindClass_impl(env, "java/lang/Object");
}

static jclass JNICALL GetSuperclass_impl(JNIEnv* env, jclass sub) {
    (void)sub;
    return FindClass_impl(env, "java/lang/Object");
}

static jboolean JNICALL IsAssignableFrom_impl(JNIEnv* env, jclass sub, jclass sup) {
    (void)env;
    (void)sub;
    (void)sup;
    return JNI_TRUE;
}

static jobject JNICALL AllocObject_impl(JNIEnv* env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jobject)(uintptr_t)0x20000001;
}

static jint JNICALL EnsureLocalCapacity_impl(JNIEnv* env, jint capacity) {
    (void)env;
    (void)capacity;
    return JNI_OK;
}

static jint JNICALL PushLocalFrame_impl(JNIEnv* env, jint capacity) {
    (void)env;
    (void)capacity;
    return JNI_OK;
}

static jobject JNICALL PopLocalFrame_impl(JNIEnv* env, jobject result) {
    (void)env;
    return result;
}

static jboolean JNICALL IsInstanceOf_impl(JNIEnv* env, jobject obj, jclass clazz) {
    (void)env;
    (void)obj;
    (void)clazz;
    return JNI_TRUE;
}

static jobject JNICALL GetObjectField_impl(JNIEnv* env, jobject obj, jfieldID fieldID) {
    (void)env;
    (void)obj;
    (void)fieldID;
    return (jobject)create_fake_string("");
}

static jboolean JNICALL GetBooleanField_impl(JNIEnv* env, jobject obj, jfieldID fieldID) {
    (void)env;
    (void)obj;
    (void)fieldID;
    return JNI_FALSE;
}

static jint JNICALL GetIntField_impl(JNIEnv* env, jobject obj, jfieldID fieldID) {
    (void)env;
    (void)obj;
    (void)fieldID;
    return 0;
}

static jfloat JNICALL GetFloatField_impl(JNIEnv* env, jobject obj, jfieldID fieldID) {
    (void)env;
    (void)obj;
    (void)fieldID;
    return 0.0f;
}

static void JNICALL SetObjectField_impl(JNIEnv* env, jobject obj, jfieldID fieldID, jobject val) {
    (void)env;
    (void)obj;
    (void)fieldID;
    (void)val;
}

static void JNICALL SetBooleanField_impl(JNIEnv* env, jobject obj, jfieldID fieldID, jboolean val) {
    (void)env;
    (void)obj;
    (void)fieldID;
    (void)val;
}

static void JNICALL SetIntField_impl(JNIEnv* env, jobject obj, jfieldID fieldID, jint val) {
    (void)env;
    (void)obj;
    (void)fieldID;
    (void)val;
}

static jobject JNICALL GetStaticObjectField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    return (jobject)create_fake_string("");
}

static jboolean JNICALL GetStaticBooleanField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    return JNI_FALSE;
}

static jint JNICALL GetStaticIntField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    return 0;
}

static void JNICALL SetStaticObjectField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID, jobject val) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    (void)val;
}

static void JNICALL SetStaticBooleanField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID, jboolean val) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    (void)val;
}

static void JNICALL SetStaticIntField_impl(JNIEnv* env, jclass clazz, jfieldID fieldID, jint val) {
    (void)env;
    (void)clazz;
    (void)fieldID;
    (void)val;
}

static jobject JNICALL NewGlobalRef_impl(JNIEnv* env, jobject obj) {
    (void)env;
    return obj;
}

static void JNICALL DeleteGlobalRef_impl(JNIEnv* env, jobject obj) {
    (void)env;
    (void)obj;
}

static void JNICALL DeleteLocalRef_impl(JNIEnv* env, jobject obj) {
    (void)env;
    (void)obj;
}

static jint JNICALL RegisterNatives_impl(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods) {
    (void)env;
    (void)clazz;
    (void)methods;
    (void)nMethods;
    return JNI_OK;
}

static jboolean JNICALL ExceptionCheck_impl(JNIEnv* env) {
    (void)env;
    return JNI_FALSE;
}

static jthrowable JNICALL ExceptionOccurred_impl(JNIEnv* env) {
    (void)env;
    return nullptr;
}

static void JNICALL ExceptionClear_impl(JNIEnv* env) {
    (void)env;
}

static void JNICALL ExceptionDescribe_impl(JNIEnv* env) {
    (void)env;
}

static jint JNICALL GetJavaVM_impl(JNIEnv* env, JavaVM** vm) {
    (void)env;
    if (vm) *vm = (JavaVM*)&g_vm_ptr;
    return JNI_OK;
}

// --- JavaVM Callbacks ---
static jint JNICALL GetEnv_impl(JavaVM* vm, void** env, jint version) {
    (void)vm;
    (void)version;
    if (env) *env = (void*)&g_env_ptr;
    return JNI_OK;
}

static jint JNICALL AttachCurrentThread_impl(JavaVM* vm, void** env, void* args) {
    (void)vm;
    (void)args;
    if (env) *env = (void*)&g_env_ptr;
    return JNI_OK;
}

static jint JNICALL DetachCurrentThread_impl(JavaVM* vm) {
    (void)vm;
    return JNI_OK;
}

void jni_bridge_init() {
    memset(&g_native_interface, 0, sizeof(g_native_interface));
    memset(&g_invoke_interface, 0, sizeof(g_invoke_interface));

    // JNIEnv function mappings
    g_native_interface.GetVersion = GetVersion_impl;
    g_native_interface.FindClass = FindClass_impl;
    g_native_interface.GetMethodID = GetMethodID_impl;
    g_native_interface.GetStaticMethodID = GetStaticMethodID_impl;
    g_native_interface.GetFieldID = GetFieldID_impl;
    g_native_interface.GetStaticFieldID = GetStaticFieldID_impl;

    g_native_interface.NewStringUTF = NewStringUTF_impl;
    g_native_interface.GetStringLength = GetStringLength_impl;
    g_native_interface.GetStringUTFLength = GetStringUTFLength_impl;
    g_native_interface.GetStringUTFChars = GetStringUTFChars_impl;
    g_native_interface.ReleaseStringUTFChars = ReleaseStringUTFChars_impl;
    g_native_interface.GetStringRegion = GetStringRegion_impl;
    g_native_interface.GetStringUTFRegion = GetStringUTFRegion_impl;

    g_native_interface.CallObjectMethod = CallObjectMethod_impl;
    g_native_interface.CallObjectMethodV = CallObjectMethodV_impl;
    g_native_interface.CallIntMethod = CallIntMethod_impl;
    g_native_interface.CallIntMethodV = CallIntMethodV_impl;
    g_native_interface.CallLongMethod = CallLongMethod_impl;
    g_native_interface.CallLongMethodV = CallLongMethodV_impl;
    g_native_interface.CallFloatMethod = CallFloatMethod_impl;
    g_native_interface.CallFloatMethodV = CallFloatMethodV_impl;
    g_native_interface.CallDoubleMethod = CallDoubleMethod_impl;
    g_native_interface.CallDoubleMethodV = CallDoubleMethodV_impl;
    g_native_interface.CallBooleanMethod = CallBooleanMethod_impl;
    g_native_interface.CallBooleanMethodV = CallBooleanMethodV_impl;
    g_native_interface.CallVoidMethod = CallVoidMethod_impl;
    g_native_interface.CallVoidMethodV = CallVoidMethodV_impl;

    g_native_interface.CallStaticObjectMethod = CallStaticObjectMethod_impl;
    g_native_interface.CallStaticObjectMethodV = CallStaticObjectMethodV_impl;
    g_native_interface.CallStaticIntMethod = CallStaticIntMethod_impl;
    g_native_interface.CallStaticIntMethodV = CallStaticIntMethodV_impl;
    g_native_interface.CallStaticLongMethod = CallStaticLongMethod_impl;
    g_native_interface.CallStaticLongMethodV = CallStaticLongMethodV_impl;
    g_native_interface.CallStaticFloatMethod = CallStaticFloatMethod_impl;
    g_native_interface.CallStaticFloatMethodV = CallStaticFloatMethodV_impl;
    g_native_interface.CallStaticDoubleMethod = CallStaticDoubleMethod_impl;
    g_native_interface.CallStaticDoubleMethodV = CallStaticDoubleMethodV_impl;
    g_native_interface.CallStaticBooleanMethod = CallStaticBooleanMethod_impl;
    g_native_interface.CallStaticBooleanMethodV = CallStaticBooleanMethodV_impl;
    g_native_interface.CallStaticVoidMethod = CallStaticVoidMethod_impl;
    g_native_interface.CallStaticVoidMethodV = CallStaticVoidMethodV_impl;

    g_native_interface.NewObject = NewObject_impl;
    g_native_interface.NewObjectV = NewObjectV_impl;
    g_native_interface.AllocObject = AllocObject_impl;
    g_native_interface.GetObjectClass = GetObjectClass_impl;
    g_native_interface.GetSuperclass = GetSuperclass_impl;
    g_native_interface.IsAssignableFrom = IsAssignableFrom_impl;
    g_native_interface.IsSameObject = IsSameObject_impl;
    g_native_interface.IsInstanceOf = IsInstanceOf_impl;
    g_native_interface.EnsureLocalCapacity = EnsureLocalCapacity_impl;
    g_native_interface.PushLocalFrame = PushLocalFrame_impl;
    g_native_interface.PopLocalFrame = PopLocalFrame_impl;

    g_native_interface.GetObjectField = GetObjectField_impl;
    g_native_interface.GetBooleanField = GetBooleanField_impl;
    g_native_interface.GetIntField = GetIntField_impl;
    g_native_interface.GetFloatField = GetFloatField_impl;
    g_native_interface.SetObjectField = SetObjectField_impl;
    g_native_interface.SetBooleanField = SetBooleanField_impl;
    g_native_interface.SetIntField = SetIntField_impl;

    g_native_interface.GetStaticObjectField = GetStaticObjectField_impl;
    g_native_interface.GetStaticBooleanField = GetStaticBooleanField_impl;
    g_native_interface.GetStaticIntField = GetStaticIntField_impl;
    g_native_interface.SetStaticObjectField = SetStaticObjectField_impl;
    g_native_interface.SetStaticBooleanField = SetStaticBooleanField_impl;
    g_native_interface.SetStaticIntField = SetStaticIntField_impl;

    g_native_interface.NewByteArray = NewByteArray_impl;
    g_native_interface.GetArrayLength = GetArrayLength_impl;
    g_native_interface.GetByteArrayElements = GetByteArrayElements_impl;
    g_native_interface.ReleaseByteArrayElements = ReleaseByteArrayElements_impl;
    g_native_interface.GetByteArrayRegion = GetByteArrayRegion_impl;
    g_native_interface.SetByteArrayRegion = SetByteArrayRegion_impl;

    g_native_interface.ExceptionCheck = ExceptionCheck_impl;
    g_native_interface.ExceptionOccurred = ExceptionOccurred_impl;
    g_native_interface.ExceptionClear = ExceptionClear_impl;
    g_native_interface.ExceptionDescribe = ExceptionDescribe_impl;
    g_native_interface.NewGlobalRef = NewGlobalRef_impl;
    g_native_interface.DeleteGlobalRef = DeleteGlobalRef_impl;
    g_native_interface.DeleteLocalRef = DeleteLocalRef_impl;
    g_native_interface.RegisterNatives = RegisterNatives_impl;
    g_native_interface.GetJavaVM = GetJavaVM_impl;

    // JavaVM function mappings
    g_invoke_interface.GetEnv = GetEnv_impl;
    g_invoke_interface.AttachCurrentThread = AttachCurrentThread_impl;
    g_invoke_interface.AttachCurrentThreadAsDaemon = AttachCurrentThread_impl;
    g_invoke_interface.DetachCurrentThread = DetachCurrentThread_impl;

    printf("[JNI Bridge] Initialized 32-bit JNI environment and JavaVM tables\n");
}

JavaVM* jni_get_java_vm() {
    return (JavaVM*)&g_vm_ptr;
}

JNIEnv* jni_get_env() {
    return (JNIEnv*)&g_env_ptr;
}
