/* iap_patch.c -- see iap_patch.h.
 *
 * Everything here is deliberately shaped like the Java class it replaces:
 * the engine creates a provider object, asks it to start a purchase, and then
 * sits in its own callback waiting for the static native to fire. We capture the
 * object's handle in the constructor, remember the request, and fire the
 * callback a few frames later from iap_patch_poll() -- same thread the Java
 * layer would have used (Globals.runOnAppThread), so the engine is never
 * re-entered from the middle of a JNI call.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#include "jni.h"
#include "jni_fake.h"
#include "fusion/fusion.h"
#include "loader/so_util.h"
#include "common/game_config.h"
#include "common/util.h"
#include "iap_patch.h"

/* ------------------------------------------------------------------ state */

/* The engine's static natives, i386 cdecl. jlong travels as an 8-byte value in
 * the prototype so the compiler lays the stack out exactly like the Java-side
 * native would have. */
typedef void (*payment_finished_fn)(void *env, void *clazz, int64_t handle, void *data,
                                    int32_t status, void *signature, void *s3, void *s4);
typedef void (*handle_bool_fn)(void *env, void *clazz, int64_t handle, int32_t flag);
typedef void (*handle_void_fn)(void *env, void *clazz, int64_t handle);

static payment_finished_fn s_payment_finished;
static handle_bool_fn      s_init_finished;
static handle_void_fn      s_restore_done;
static handle_void_fn      s_restore_failed;

static int       s_enabled;
static int64_t   s_handle;        /* the provider object the engine created */
static int       s_init_sent;     /* billing-ready already delivered */
static unsigned  s_order;         /* receipt counter, so each looks unique */

#define AB_IAP_MAX_PENDING 16
#define AB_IAP_DELAY_FRAMES 8

enum { IAP_REQ_PURCHASE = 1, IAP_REQ_INIT, IAP_REQ_RESTORE };

typedef struct {
    int  kind;
    int  frames;          /* frames left before the reply is delivered */
    char sku[96];
} IapRequest;

static IapRequest s_queue[AB_IAP_MAX_PENDING];
static int        s_queue_len;

/* Google Play's purchaseData for this title. The engine parses packageName,
 * orderId, productId, autoRenewing, purchaseTime, purchaseState and
 * developerPayload out of it and treats purchaseState 0 as "purchased". */
static const char *const GAME_PACKAGE = "com.rovio.angrybirds";

/* ------------------------------------------------------------------ helpers */

/* i386 stacks an 8-byte argument as two 32-bit words at 4-byte alignment. The
 * handle is a pointer with the low bit tags cleared, so take the first word that
 * looks like one -- the same heuristic the AudioOutput peer capture uses. */
static int take_handle(va_list ap, int64_t *out) {
#if defined(__i386__) || defined(_M_IX86)
    const uint32_t *w = (const uint32_t *)(ap);
    for (int i = 0; i < 2; i++) {
        uint32_t v = w[i];
        if ((v & 3u) == 0 && v >= 0x10000u && v != 0xffffffffu) {
            *out = (int64_t)(uintptr_t)v;
            return 1;
        }
    }
    return 0;
#else
    int64_t v = va_arg(ap, int64_t);
    if (!v) return 0;
    *out = v;
    return 1;
#endif
}

/* First string argument of an engine -> Java call (the sku, in every billing
 * method we care about). */
static void first_string_arg(const char *sig, va_list ap, char *out, size_t cap) {
    out[0] = 0;
    if (!sig) return;
    const char *p = strchr(sig, '(');
    if (!p) return;
    p++;

    while (*p && *p != ')') {
        char c = *p++;
        switch (c) {
            case 'Z': case 'B': case 'C': case 'S': case 'I':
                (void)va_arg(ap, int);
                break;
            case 'J':
                (void)va_arg(ap, int64_t);
                break;
            case 'F': case 'D':
                (void)va_arg(ap, double);
                break;
            case '[':
                if (*p == 'L') { while (*p && *p != ';') p++; if (*p == ';') p++; }
                else if (*p) p++;
                (void)va_arg(ap, void *);
                break;
            case 'L': {
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                void *o = va_arg(ap, void *);
                const char *s = jni_obj_string(o);
                if (s && *s) { snprintf(out, cap, "%s", s); return; }
                break;
            }
            default:
                return;
        }
    }
}

static void queue_push(int kind, const char *sku) {
    if (s_queue_len >= AB_IAP_MAX_PENDING) {
        /* Drop the oldest; a queue this deep only happens if the engine asks for
         * more purchases than anyone can tap. */
        memmove(&s_queue[0], &s_queue[1], sizeof(s_queue[0]) * (AB_IAP_MAX_PENDING - 1));
        s_queue_len = AB_IAP_MAX_PENDING - 1;
    }
    IapRequest *r = &s_queue[s_queue_len++];
    r->kind = kind;
    r->frames = AB_IAP_DELAY_FRAMES;
    snprintf(r->sku, sizeof(r->sku), "%s", sku ? sku : "");
    debugPrintf("[IAP] queued %s for '%s' (handle=%p)\n",
                kind == IAP_REQ_PURCHASE ? "purchase" :
                kind == IAP_REQ_RESTORE ? "restore" : "billing-init",
                r->sku[0] ? r->sku : "(none)", (void *)(uintptr_t)s_handle);
}

/* --------------------------------------------------------------- delivery */

static void deliver_purchase(const char *sku) {
    if (!s_payment_finished || !s_handle) {
        debugPrintf("[IAP] purchase of '%s' ignored (no provider handle)\n", sku);
        return;
    }

    const char *product = (sku && *sku) ? sku : "com.rovio.angrybirds.removeads";
    char receipt[512];
    unsigned long long now = (unsigned long long)time(NULL) * 1000ULL;

    snprintf(receipt, sizeof(receipt),
             "{\"orderId\":\"ABD.%.8llu-%u.1\",\"packageName\":\"%s\","
             "\"productId\":\"%s\",\"purchaseTime\":%llu,\"purchaseState\":0,"
             "\"developerPayload\":\"\",\"purchaseToken\":\"abdesktop.%u.%llu\","
             "\"autoRenewing\":false}",
             now, s_order, GAME_PACKAGE, product, now, s_order, now);
    s_order++;

    /* Leaked on purpose: a real JNI local reference stays valid until the frame
     * returns, and the engine is free to read the string after the call (it
     * builds a std::string and fires an analytics event). A few hundred bytes
     * per purchase is nothing next to a dangling pointer. */
    void *data = jni_make_string(receipt);
    void *signature = jni_make_string("ABDESKTOP-FAKE-PLAY-SIGNATURE-0000000000000000");
    void *clazz = jni_make_string("GooglePlayPaymentProvider");

    debugPrintf("[IAP] paymentFinished(product='%s', status=0/success)\n", product);
    s_payment_finished(fake_env, clazz, s_handle, data, 0 /* SUCCESS */, signature, NULL, NULL);
}

void iap_patch_poll(void) {
    if (!s_enabled || s_queue_len <= 0) return;

    /* Billing-ready goes out first, before any result: the engine will not route
     * a purchase until the provider has told it the service is bound. */
    for (int i = 0; i < s_queue_len; i++) {
        IapRequest *r = &s_queue[i];
        if (r->kind == IAP_REQ_INIT && r->frames > 0) r->frames = 0;
    }

    for (int i = 0; i < s_queue_len; i++) {
        IapRequest *r = &s_queue[i];
        if (r->frames > 0) { r->frames--; continue; }

        switch (r->kind) {
            case IAP_REQ_INIT:
                if (s_init_finished) {
                    debugPrintf("[IAP] initFinished(handle=%p, ok=true) -- billing ready\n",
                                (void *)(uintptr_t)s_handle);
                    s_init_finished(fake_env, NULL, s_handle, 1);
                }
                break;
            case IAP_REQ_RESTORE:
                debugPrintf("[IAP] restoreDone(handle=%p) -- nothing to restore\n",
                            (void *)(uintptr_t)s_handle);
                if (s_restore_done) s_restore_done(fake_env, NULL, s_handle);
                break;
            case IAP_REQ_PURCHASE:
                deliver_purchase(r->sku);
                break;
            default:
                break;
        }

        memmove(&s_queue[i], &s_queue[i + 1], sizeof(s_queue[0]) * (size_t)(s_queue_len - i - 1));
        s_queue_len--;
        i--;
    }
}

/* ------------------------------------------------------------------ engine -> Java */

/* The engine asks the provider object to do something. We only queue; the reply
 * is delivered from iap_patch_poll() so it never re-enters the engine inside a
 * JNI call. */
int iap_handles_java_call(const char *cls, const char *method, const char *sig, va_list ap) {
    if (!s_enabled || !method) return 0;

    char sku[96];
    first_string_arg(sig, ap, sku, sizeof(sku));

    if (!strcmp(method, "startPurchase") || !strcmp(method, "purchaseProduct") ||
        !strcmp(method, "purchase")) {
        debugPrintf("[IAP] engine -> %s.%s%s sku='%s'\n", cls ? cls : "?", method,
                    sig ? sig : "", sku);
        queue_push(IAP_REQ_PURCHASE, sku);
        return 1;
    }
    if (!strcmp(method, "restorePurchases") || !strcmp(method, "restore")) {
        debugPrintf("[IAP] engine -> %s.%s -- answering with a successful restore\n",
                    cls ? cls : "?", method);
        queue_push(IAP_REQ_RESTORE, NULL);
        return 1;
    }
    if (!strcmp(method, "consumePurchase") || !strcmp(method, "finishPurchase") ||
        !strcmp(method, "completeExternalPurchase") || !strcmp(method, "loadCatalog") ||
        !strcmp(method, "unregisterBroadcastReceiver")) {
        debugPrintf("[IAP] engine -> %s.%s -- ok (fake store, nothing to bill)\n",
                    cls ? cls : "?", method);
        return 1;
    }
    return 0;
}

int iap_patch_ctor(const char *class_name, va_list ap) {
    if (!s_enabled || !class_name) return 0;
    if (!strstr(class_name, "PaymentProvider") && !strstr(class_name, "Payment"))
        return 0;

    int64_t handle = 0;
    if (take_handle(ap, &handle)) {
        s_handle = handle;
        debugPrintf("[IAP] provider %s created, native handle=%p\n",
                    class_name, (void *)(uintptr_t)handle);
        if (!s_init_sent) {
            s_init_sent = 1;
            queue_push(IAP_REQ_INIT, NULL);
        }
        return 1;
    }
    debugPrintf("[IAP] provider %s created without a handle -- purchases will not "
                "complete\n", class_name);
    return 1;
}

void iap_patch_request(const char *sku) {
    if (!s_enabled) return;
    debugPrintf("[IAP] self-test purchase request '%s'\n", sku ? sku : "(default)");
    queue_push(IAP_REQ_PURCHASE, sku);
}

void iap_patch_init(so_module *mod, int enabled) {
    s_enabled = enabled ? 1 : 0;
    s_handle = 0;
    s_init_sent = 0;
    s_order = 1;
    s_queue_len = 0;

    if (!s_enabled) {
        debugPrintf("[IAP] fake billing disabled (iap off)\n");
        return;
    }
    if (!mod || !mod->base) {
        debugPrintf("[IAP] no engine module -- fake billing unavailable\n");
        s_enabled = 0;
        return;
    }

    /* The engine exports its Java-facing natives, so the fake Java side can call
     * them like any other function. */
    s_payment_finished = (payment_finished_fn)
        so_symbol(mod, "Java_com_rovio_rcs_payment_google_GooglePlayPaymentProvider_paymentFinished");
    s_restore_done = (handle_void_fn)
        so_symbol(mod, "Java_com_rovio_rcs_payment_google_GooglePlayPaymentProvider_restoreDone");
    s_restore_failed = (handle_void_fn)
        so_symbol(mod, "Java_com_rovio_rcs_payment_google_GooglePlayPaymentProvider_restoreFailed");
    s_init_finished = (handle_bool_fn)
        so_symbol(mod, "Java_com_rovio_rcs_payment_google_GooglePlayPaymentProvider_initFinished");

    debugPrintf("[IAP] fake Google Play billing ready (paymentFinished=%p restoreDone=%p "
                "initFinished=%p)\n",
                (void *)s_payment_finished, (void *)s_restore_done, (void *)s_init_finished);

    if (!s_payment_finished) {
        debugPrintf("[IAP] engine billing entry points missing; leaving shop alone\n");
        s_enabled = 0;
    }
}
