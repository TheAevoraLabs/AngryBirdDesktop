/* present.h -- presentation scaler ("upscaler") and resolution control.
 *
 * The engine renders into whatever framebuffer is bound when it runs. In the
 * default `native` mode we let it draw straight to the window, which on a modern
 * GPU is the best-looking option: Fusion picks its UI layout and texture scale
 * from the size it was told, so a 1920x1080 window means it renders at 1920x1080.
 *
 * The `linear` and `integer` modes instead render the frame into an offscreen
 * texture at an internal resolution (renderScale % of the window) and then draw
 * that texture over the window. That is what makes low-end machines playable and
 * gives a crisp, pixel-exact picture in `integer` mode.
 *
 * Everything here needs the GL context to be current; the calls are GLES2 only.
 */

#ifndef AB_PRESENT_H
#define AB_PRESENT_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AB_SCALER_NATIVE  = 0,  /* engine draws to the window directly              */
  AB_SCALER_LINEAR  = 1,  /* offscreen + smooth upscale, aspect preserved     */
  AB_SCALER_INTEGER = 2,  /* offscreen + nearest, whole-number scale          */
};

typedef struct {
  int scaler;        /* AB_SCALER_*                                        */
  int render_scale;  /* internal size as a percentage of the window        */
  int window_w, window_h;
} PresentConfig;

/* `scaler_name` is the config.txt spelling; unknown names fall back to native. */
int  present_scaler_from_name(const char *scaler_name);

/* Must be called once with a current GL context. Returns 0 on success. */
int  present_init(const PresentConfig *cfg);
void present_shutdown(void);

/* Window/drawable size changed. Recreates the offscreen target if needed. */
void present_set_window(int w, int h);

/* The size the engine should be told about (nativeInit/nativeResize). */
int  present_render_w(void);
int  present_render_h(void);

/* 1 when the frame is rendered offscreen and blitted. */
int  present_active(void);

/* Called immediately before and after the engine's update/render for the frame. */
void present_begin(void);
void present_end(void);

/* Dump the frame that is about to be swapped to `path` as a BMP. `path` should
 * end in .bmp. Returns 0 on success. Needs a current GL context. */
int  present_capture(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AB_PRESENT_H */
