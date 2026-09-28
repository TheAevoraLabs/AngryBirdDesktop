/* input.cpp -- SDL3 -> Fusion input translation. See input.h for the model.
 *
 * Pointers are tracked through a small slot table so the engine receives small,
 * dense pointer ids (0, 1, 2, ...). Slot 0 is the primary point: nativeInput
 * special-cases pointerId 0 (it also drives the engine's synthetic "tap" key),
 * so the first finger -- or a mouse click -- must land on id 0.
 *
 * One queue, two producers. A touchscreen never emits hover; a mouse always
 * does while no button is held, and switches to DOWN/MOVE/UP while one is.
 * That is precisely the split android.view.View gives the engine on Android:
 * onTouchEvent for buttons, onHoverEvent for a hand that is merely moving.
 */

#include "input.h"
#include "common/util.h"

#include <string.h>

#define AB_MAX_QUEUE 96

static AbInputConfig s_cfg;
static AbInputEvent  s_queue[AB_MAX_QUEUE];
static int s_qcount = 0;

/* touch slot table: slot i (== pointer id i) belongs to SDL finger s_fingers[i]
 * while s_finger_used[i] is set. */
static uint64_t s_fingers[16];
static int      s_finger_used[16];

/* mouse state */
static float s_mouse_x = 0.0f, s_mouse_y = 0.0f;
static int   s_hover_active = 0;
static int   s_mouse_down = 0;

static int effective_mode(void) {
    return s_cfg.mode;
}

/* ------------------------------------------------------------------ helpers */

static float to_render_x(float px) {
    if (s_cfg.window_w > 0 && s_cfg.render_w > 0 && s_cfg.window_w != s_cfg.render_w)
        return px * (float)s_cfg.render_w / (float)s_cfg.window_w;
    return px;
}

static float to_render_y(float py) {
    if (s_cfg.window_h > 0 && s_cfg.render_h > 0 && s_cfg.window_h != s_cfg.render_h)
        return py * (float)s_cfg.render_h / (float)s_cfg.window_h;
    return py;
}

static int get_max_pointers(void) {
    return (s_cfg.max_pointers > 0 && s_cfg.max_pointers <= 16) ? s_cfg.max_pointers : 8;
}

static void push(const AbInputEvent *ev) {
    if (s_qcount >= AB_MAX_QUEUE) {           /* drop oldest: never grow the latency */
        memmove(&s_queue[0], &s_queue[1], sizeof(s_queue[0]) * (AB_MAX_QUEUE - 1));
        s_qcount = AB_MAX_QUEUE - 1;
    }
    s_queue[s_qcount++] = *ev;
}

static void push_pointer(int action, int pointer_id, float x, float y) {
    AbInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AB_EV_POINTER;
    ev.action = action;
    ev.pointer_id = pointer_id;
    ev.x = x;
    ev.y = y;
    push(&ev);
}

static void push_key(int keycode, int action, int unicode) {
    AbInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AB_EV_KEY;
    ev.action = action;
    ev.keycode = keycode;
    ev.unicode = unicode;
    ev.device_id = 1;                         /* one synthetic "keyboard" device */
    push(&ev);
}

/* -------------------------------------------------------------------- touch */

static int finger_find(uint64_t id) {
    int max = get_max_pointers();
    for (int i = 0; i < max; i++)
        if (s_finger_used[i] && s_fingers[i] == id) return i;
    return -1;
}

static int finger_alloc(uint64_t id) {
    int i = finger_find(id);
    if (i >= 0) return i;
    int max = get_max_pointers();
    for (i = 0; i < max; i++)
        if (!s_finger_used[i]) { s_finger_used[i] = 1; s_fingers[i] = id; return i; }
    return -1;
}

static void finger_free(int slot) {
    int max = get_max_pointers();
    if (slot >= 0 && slot < max) s_finger_used[slot] = 0;
}

/* --------------------------------------------------------------------- mouse */

static void mouse_hover_enter(void) {
    if (s_hover_active) return;
    s_hover_active = 1;
    push_pointer(AB_ACTION_HOVER_ENTER, 0, to_render_x(s_mouse_x), to_render_y(s_mouse_y));
}

static void mouse_hover_exit(void) {
    if (!s_hover_active) return;
    s_hover_active = 0;
    push_pointer(AB_ACTION_HOVER_EXIT, 0, to_render_x(s_mouse_x), to_render_y(s_mouse_y));
}

static void mouse_motion(float wx, float wy) {
    s_mouse_x = wx;
    s_mouse_y = wy;

    if (effective_mode() == AB_INPUT_TOUCH) {
        /* Touch semantics: a mouse drag is just a finger drag. */
        if (s_mouse_down)
            push_pointer(AB_ACTION_MOVE, s_cfg.mouse_pointer_id,
                         to_render_x(wx), to_render_y(wy));
        return;
    }

    if (s_mouse_down) {
        /* A held button produces ACTION_MOVE, exactly like a mouse drag on
         * Android (the hover stream stops while a button is down). */
        push_pointer(AB_ACTION_MOVE, s_cfg.mouse_pointer_id,
                     to_render_x(wx), to_render_y(wy));
    } else if (s_cfg.mouse_hover) {
        mouse_hover_enter();
        push_pointer(AB_ACTION_HOVER_MOVE, 0, to_render_x(wx), to_render_y(wy));
    }
}

/* ----------------------------------------------------------------- keyboard */

/* SDL key -> Android KeyEvent key code. Only the keys this game can plausibly
 * use are mapped; anything unmapped is reported as 0 and dropped. */
static int android_keycode(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z)
        return 29 + (int)(key - SDLK_A);              /* KEYCODE_A .. KEYCODE_Z */
    if (key >= SDLK_0 && key <= SDLK_9)
        return 7 + (int)(key - SDLK_0);               /* KEYCODE_0 .. KEYCODE_9 */

    switch (key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return 66;  /* KEYCODE_ENTER   */
        case SDLK_BACKSPACE:                  return 67;  /* KEYCODE_DEL     */
        case SDLK_SPACE:                      return 62;  /* KEYCODE_SPACE   */
        case SDLK_TAB:                        return 61;  /* KEYCODE_TAB     */
        case SDLK_ESCAPE:                     return 111; /* KEYCODE_ESCAPE  */
        case SDLK_UP:                         return 19;  /* KEYCODE_DPAD_UP */
        case SDLK_DOWN:                       return 20;
        case SDLK_LEFT:                       return 21;
        case SDLK_RIGHT:                      return 22;
        case SDLK_MENU:                       return 82;  /* KEYCODE_MENU    */
        case SDLK_VOLUMEUP:                   return 24;
        case SDLK_VOLUMEDOWN:                 return 25;
        default:                              return 0;
    }
}

static int unicode_of(SDL_Keycode key) {
    return (key >= 0x20 && key < 0x7f) ? (int)key : 0;
}

static void handle_key(const SDL_KeyboardEvent *k, int down) {
    if (!s_cfg.keyboard) return;
    int code = android_keycode(k->key);
    if (!code) return;
    push_key(code, down ? AB_KEY_DOWN : AB_KEY_UP, down ? unicode_of(k->key) : 0);
}

/* ------------------------------------------------------------------ the API */

void ab_input_init(const AbInputConfig *cfg) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    if (cfg) s_cfg = *cfg;

    if (s_cfg.max_pointers <= 0 || s_cfg.max_pointers > 16) s_cfg.max_pointers = 8;
    if (s_cfg.render_w <= 0) s_cfg.render_w = 1280;
    if (s_cfg.render_h <= 0) s_cfg.render_h = 720;
    if (s_cfg.window_w <= 0) s_cfg.window_w = s_cfg.render_w;
    if (s_cfg.window_h <= 0) s_cfg.window_h = s_cfg.render_h;
    if (s_cfg.mouse_pointer_id < 0) s_cfg.mouse_pointer_id = 0;

    memset(s_fingers, 0, sizeof(s_fingers));
    memset(s_finger_used, 0, sizeof(s_finger_used));
    s_qcount = 0;
    s_hover_active = 0;
    s_mouse_down = 0;

    /* Do not let SDL turn a finger into a synthetic mouse (or a mouse into a
     * synthetic finger): we want the two streams to stay distinguishable so
     * the engine's own hover handling is what a mouse actually exercises. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    debugPrintf("[Input] module ready: mode=%s hover=%d render=%dx%d window=%dx%d\n",
                s_cfg.mode == AB_INPUT_MOUSE ? "mouse" :
                s_cfg.mode == AB_INPUT_TOUCH ? "touch" : "auto",
                s_cfg.mouse_hover, s_cfg.render_w, s_cfg.render_h,
                s_cfg.window_w, s_cfg.window_h);
}

void ab_input_resize(int window_w, int window_h) {
    if (window_w > 0) s_cfg.window_w = window_w;
    if (window_h > 0) s_cfg.window_h = window_h;
}

void ab_input_set_render_size(int render_w, int render_h) {
    if (render_w > 0) s_cfg.render_w = render_w;
    if (render_h > 0) s_cfg.render_h = render_h;
}

void ab_input_reset(void) {
    int max = get_max_pointers();
    for (int i = 0; i < max; i++) {
        if (!s_finger_used[i]) continue;
        push_pointer(AB_ACTION_CANCEL, i, 0.0f, 0.0f);
        s_finger_used[i] = 0;
    }
    if (s_mouse_down) {
        push_pointer(AB_ACTION_CANCEL, s_cfg.mouse_pointer_id,
                     to_render_x(s_mouse_x), to_render_y(s_mouse_y));
        s_mouse_down = 0;
    }
    s_hover_active = 0;
}

void ab_input_handle(const SDL_Event *ev) {
    if (!ev) return;
    switch (ev->type) {
        /* ---- mouse ---- */
        case SDL_EVENT_MOUSE_MOTION:
            mouse_motion(ev->motion.x, ev->motion.y);
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            s_mouse_x = ev->button.x;
            s_mouse_y = ev->button.y;
            if (ev->button.button == SDL_BUTTON_LEFT) {
                if (effective_mode() != AB_INPUT_TOUCH)
                    mouse_hover_exit();     /* Android stops hover on button down */
                s_mouse_down = 1;
                push_pointer(AB_ACTION_DOWN, s_cfg.mouse_pointer_id,
                             to_render_x(s_mouse_x), to_render_y(s_mouse_y));
            } else if (ev->button.button == SDL_BUTTON_RIGHT) {
                /* Android maps the secondary mouse button to KEYCODE_BACK. */
                push_key(4, AB_KEY_DOWN, 0);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            s_mouse_x = ev->button.x;
            s_mouse_y = ev->button.y;
            if (ev->button.button == SDL_BUTTON_LEFT) {
                s_mouse_down = 0;
                push_pointer(AB_ACTION_UP, s_cfg.mouse_pointer_id,
                             to_render_x(s_mouse_x), to_render_y(s_mouse_y));
                if (effective_mode() != AB_INPUT_TOUCH && s_cfg.mouse_hover)
                    mouse_hover_enter();    /* back to hovering where we are */
            } else if (ev->button.button == SDL_BUTTON_RIGHT) {
                push_key(4, AB_KEY_UP, 0);
            }
            break;

        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            if (effective_mode() != AB_INPUT_TOUCH && s_cfg.mouse_hover && !s_mouse_down)
                mouse_hover_enter();
            break;

        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (!s_mouse_down) mouse_hover_exit();
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            ab_input_reset();
            break;

        /* ---- touchscreen ---- */
        case SDL_EVENT_FINGER_DOWN: {
            int slot = finger_alloc(ev->tfinger.fingerID);
            if (slot < 0) break;
            push_pointer(AB_ACTION_DOWN, slot,
                         to_render_x(ev->tfinger.x * s_cfg.window_w),
                         to_render_y(ev->tfinger.y * s_cfg.window_h));
            break;
        }

        case SDL_EVENT_FINGER_MOTION: {
            int slot = finger_find(ev->tfinger.fingerID);
            if (slot < 0) break;
            push_pointer(AB_ACTION_MOVE, slot,
                         to_render_x(ev->tfinger.x * s_cfg.window_w),
                         to_render_y(ev->tfinger.y * s_cfg.window_h));
            break;
        }

        case SDL_EVENT_FINGER_UP: {
            int slot = finger_find(ev->tfinger.fingerID);
            if (slot < 0) break;
            push_pointer(AB_ACTION_UP, slot,
                         to_render_x(ev->tfinger.x * s_cfg.window_w),
                         to_render_y(ev->tfinger.y * s_cfg.window_h));
            finger_free(slot);
            break;
        }

        case SDL_EVENT_FINGER_CANCELED: {
            int slot = finger_find(ev->tfinger.fingerID);
            if (slot < 0) break;
            push_pointer(AB_ACTION_CANCEL, slot, 0.0f, 0.0f);
            finger_free(slot);
            break;
        }

        /* ---- keyboard ---- */
        case SDL_EVENT_KEY_DOWN:
            if (!ev->key.repeat) handle_key(&ev->key, 1);
            break;

        case SDL_EVENT_KEY_UP:
            handle_key(&ev->key, 0);
            break;

        default:
            break;
    }
}

int ab_input_poll(AbInputEvent *out, int max) {
    if (!out || max <= 0) return 0;
    int n = s_qcount < max ? s_qcount : max;
    memcpy(out, s_queue, sizeof(s_queue[0]) * n);
    if (n < s_qcount) {
        memmove(&s_queue[0], &s_queue[n], sizeof(s_queue[0]) * (s_qcount - n));
        s_qcount -= n;
    } else {
        s_qcount = 0;
    }
    return n;
}

void ab_input_shutdown(void) {
    ab_input_reset();
    s_qcount = 0;
}

int ab_input_mode(void)        { return effective_mode(); }
int ab_input_mouse_hover(void) { return s_cfg.mouse_hover; }

int ab_input_active_pointers(void) {
    int n = s_mouse_down ? 1 : 0;
    int max = get_max_pointers();
    for (int i = 0; i < max; i++)
        if (s_finger_used[i]) n++;
    return n;
}
