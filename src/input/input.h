/* input.h -- modular pointer (mouse + multi-touch) and keyboard front-end for
 * the Fusion engine.
 *
 * The engine never sees SDL: it exports
 *
 *   Java_com_rovio_fusion_MyInputHandler_nativeInput(int action, float x, float y, int pointerId)
 *   Java_com_rovio_fusion_MyInputHandler_nativeKeyInput(int keyCode, int action, int unicodeChar, int deviceId)
 *
 * and its Java layer (com.rovio.fusion.InputDelegator) simply forwards Android
 * MotionEvent/KeyEvent data into them. `action` is the raw Android
 * MotionEvent action, so the engine already understands the mouse: 9/7/10 are
 * ACTION_HOVER_ENTER/_MOVE/_EXIT, which Android only ever produces for a real
 * pointing device. We therefore do NOT fake a pointer device as a finger -- a
 * desktop mouse is delivered as hover + button events, exactly like Android
 * would, and a touchscreen is delivered as real multi-touch (one pointer id per
 * finger). Both paths may be live at the same time.
 *
 * This module owns only the translation. It knows nothing about JNI; the host
 * drains the translated events and forwards them. Swap it out for a gamepad or
 * a virtual cursor by writing another producer of AbInputEvent.
 */

#ifndef AB_INPUT_H
#define AB_INPUT_H

#include <SDL3/SDL.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Android MotionEvent actions -- what Fusion's nativeInput expects. */
enum {
  AB_ACTION_DOWN        = 0,
  AB_ACTION_UP          = 1,
  AB_ACTION_MOVE        = 2,
  AB_ACTION_CANCEL      = 3,
  AB_ACTION_HOVER_MOVE  = 7,
  AB_ACTION_HOVER_ENTER = 9,
  AB_ACTION_HOVER_EXIT  = 10,
};

/* Fusion's nativeKeyInput action: the Java layer sends 1 for KEY_DOWN and 0 for
 * KEY_UP (the reverse of android.view.KeyEvent). */
enum {
  AB_KEY_UP   = 0,
  AB_KEY_DOWN = 1,
};

/* How desktop/pointing input is delivered to the engine. */
enum {
  AB_INPUT_AUTO  = 0, /* mouse + touchscreen, whichever produces events */
  AB_INPUT_MOUSE = 1, /* force the mouse path (hover + buttons) */
  AB_INPUT_TOUCH = 2, /* force touch semantics (down/move/up, no hover) */
};

typedef enum {
  AB_EV_POINTER = 0,
  AB_EV_KEY     = 1,
} AbEventType;

typedef struct {
  AbEventType type;

  /* pointer events */
  int32_t action;      /* AB_ACTION_* */
  int32_t pointer_id;  /* 0 = primary point; 1..N for extra fingers */
  float   x, y;        /* render-space pixels, origin top-left */

  /* key events */
  int32_t keycode;     /* Android KeyEvent key code */
  int32_t unicode;     /* unicode char for text fields, 0 if none */
  int32_t device_id;   /* synthetic input device id */
} AbInputEvent;

typedef struct {
  int render_w, render_h;    /* size handed to nativeInit/nativeResize */
  int window_w, window_h;    /* current SDL window size (mouse space) */
  int mode;                  /* AB_INPUT_* (default AB_INPUT_AUTO) */
  int mouse_hover;           /* emit 9/7/10 for mouse motion (default 1) */
  int mouse_pointer_id;      /* pointer id used for mouse buttons (default 0) */
  int keyboard;              /* translate key events (default 1) */
  int max_pointers;          /* touch slots to track (default 8) */
} AbInputConfig;

/* Call once after the window exists. Sets the SDL hints that stop SDL from
 * synthesising a mouse from touch (which would double up every finger). */
void ab_input_init(const AbInputConfig *cfg);

/* Keep the window size in sync (call on SDL_EVENT_WINDOW_RESIZED) and the
 * engine render size in sync (call on nativeResize). */
void ab_input_resize(int window_w, int window_h);
void ab_input_set_render_size(int render_w, int render_h);

/* Feed one raw SDL event. Everything unrecognised is ignored. */
void ab_input_handle(const SDL_Event *ev);

/* Drain translated events. Returns how many were written (0..max). */
int  ab_input_poll(AbInputEvent *out, int max);

/* Drop all tracked pointers (call when the window loses focus). */
void ab_input_reset(void);

void ab_input_shutdown(void);

/* Queries, mostly for logging. */
int ab_input_mode(void);          /* the effective mode */
int ab_input_mouse_hover(void);   /* is hover delivery on? */
int ab_input_active_pointers(void);

#ifdef __cplusplus
}
#endif

#endif /* AB_INPUT_H */
