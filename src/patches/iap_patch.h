/* iap_patch.h -- Google Play billing, answered locally.
 *
 * The engine never talks to Google Play directly: it constructs
 * com.rovio.rcs.payment.google.GooglePlayPaymentProvider over JNI and waits for
 * that Java object to call the static natives it registered:
 *
 *   initFinished(long handle, boolean ok)
 *   paymentFinished(long handle, String purchaseData, int status,
 *                   String signature, String s3, String s4)
 *   restoreDone(long handle) / restoreFailed(long handle)
 *   skuDetailsLoaded(long handle, SkuDetails[] details)
 *
 * With no Java runtime behind us those callbacks would never arrive, so a tap
 * on a shop item would sit there forever. This module plays the Java side:
 * the provider constructor hands us its handle, every purchase request is
 * queued, and a few frames later (like a real network round trip) we call the
 * engine's own paymentFinished() with a synthetic Google Play receipt and
 * status SUCCESS. Restores always succeed, consumption always succeeds.
 *
 * It is driven from fusion.c (every engine -> Java call goes through there) and
 * polled from the main loop, so the callback lands on the game thread instead of
 * re-entering the engine from inside a Java call.
 */

#ifndef AB_IAP_PATCH_H
#define AB_IAP_PATCH_H

#include <stdarg.h>

#include "loader/so_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve the engine's billing entry points. `enabled` comes from config.txt
 * (`iap on|off`); when off the module still resolves/handles nothing and the
 * stock "purchase never completes" behaviour is left alone. */
void iap_patch_init(so_module *mod, int enabled);

/* Called by jni_fake for every NewObject. Returns 1 when this was the billing
 * provider (the native handle has been taken out of the constructor args). */
int iap_patch_ctor(const char *class_name, va_list ap);

/* Called by fusion_call before its own dispatch. Returns 1 when the call was a
 * billing request and has been queued. */
int iap_handles_java_call(const char *cls, const char *method, const char *sig, va_list ap);

/* Run due callbacks. Call once per frame from the main loop. */
void iap_patch_poll(void);

/* Pretend the engine asked to buy `sku` (used by the AB_FAKE_PURCHASE self-test
 * so the whole path can be exercised without a mouse). */
void iap_patch_request(const char *sku);

#ifdef __cplusplus
}
#endif

#endif /* AB_IAP_PATCH_H */
