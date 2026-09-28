/* present.c -- see present.h. GLES2 only: one FBO + one textured quad. */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengles2.h>

#include "present.h"
#include "common/util.h"

/* Vertex attrib slots for the upscale blit. Kept away from 0/1 (and away from
 * most engines' 0..3) so our quad never stomps the geometry arrays the engine
 * left behind; present.c saves and restores exactly these two. */
#define AB_ATTRIB_POS 6
#define AB_ATTRIB_UV  7

/* GLES2 only carries the depth-stencil pair through an extension; fall back to
 * the raw enum values so this compiles with any gl2.h. */
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT_OES
#define GL_DEPTH_STENCIL_ATTACHMENT_OES 0x821A
#endif
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#endif

static PresentConfig s_cfg;

static GLuint s_fbo, s_tex, s_depth;
static int    s_fbo_w, s_fbo_h;
static GLuint s_prog, s_vbo, s_pos_loc, s_uv_loc, s_tex_loc;
static int    s_ready;
static int    s_depth_attached = 1;

/* --------------------------------------------------------------- shaders */

static const char *VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        debugPrintf("[Present] shader compile failed: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

int present_scaler_from_name(const char *name) {
    if (!name || !*name) return AB_SCALER_NATIVE;
    if (!strcasecmp(name, "linear") || !strcasecmp(name, "smooth")) return AB_SCALER_LINEAR;
    if (!strcasecmp(name, "integer") || !strcasecmp(name, "nearest") ||
        !strcasecmp(name, "pixel")) return AB_SCALER_INTEGER;
    return AB_SCALER_NATIVE;
}

int present_init(const PresentConfig *cfg) {
    if (cfg) s_cfg = *cfg;
    if (s_cfg.render_scale < 10) s_cfg.render_scale = 10;
    if (s_cfg.render_scale > 400) s_cfg.render_scale = 400;
    if (s_cfg.window_w <= 0 || s_cfg.window_h <= 0) {
        s_cfg.window_w = 1280; s_cfg.window_h = 720;
    }

    s_fbo = s_tex = s_depth = 0;
    s_fbo_w = s_fbo_h = 0;
    s_ready = 0;
    s_depth_attached = 1;

    if (s_cfg.scaler == AB_SCALER_NATIVE) {
        debugPrintf("[Present] scaler=native (engine renders at the window size)\n");
        return 0;
    }

    debugPrintf("[Present] GL_VERSION=%s GL_RENDERER=%s\n",
                (const char *)glGetString(GL_VERSION),
                (const char *)glGetString(GL_RENDERER));

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return -1; }

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    /* Locations 6/7: engines pass their geometry in 0..3, and our save/restore
     * only watches these two, so neither side disturbs the other. */
    glBindAttribLocation(s_prog, AB_ATTRIB_POS, "a_pos");
    glBindAttribLocation(s_prog, AB_ATTRIB_UV, "a_uv");
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(s_prog, sizeof(log) - 1, NULL, log);
        debugPrintf("[Present] program link failed: %s\n", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return -1;
    }

    s_pos_loc = glGetAttribLocation(s_prog, "a_pos");
    s_uv_loc  = glGetAttribLocation(s_prog, "a_uv");
    s_tex_loc = glGetUniformLocation(s_prog, "u_tex");

    glGenBuffers(1, &s_vbo);
    glGenTextures(1, &s_tex);
    glGenFramebuffers(1, &s_fbo);
    glGenRenderbuffers(1, &s_depth);

    s_ready = 1;
    present_set_window(s_cfg.window_w, s_cfg.window_h);
    debugPrintf("[Present] scaler=%s renderScale=%d%%\n",
                s_cfg.scaler == AB_SCALER_INTEGER ? "integer" : "linear",
                s_cfg.render_scale);
    return 0;
}

void present_shutdown(void) {
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_tex) glDeleteTextures(1, &s_tex);
    if (s_depth) glDeleteRenderbuffers(1, &s_depth);
    if (s_vbo) glDeleteBuffers(1, &s_vbo);
    if (s_prog) glDeleteProgram(s_prog);
    s_fbo = s_tex = s_depth = s_vbo = s_prog = 0;
    s_ready = 0;
}

int present_render_w(void) {
    if (!present_active()) return s_cfg.window_w;
    int w = s_cfg.window_w * s_cfg.render_scale / 100;
    return w > 0 ? w : 1;
}

int present_render_h(void) {
    if (!present_active()) return s_cfg.window_h;
    int h = s_cfg.window_h * s_cfg.render_scale / 100;
    return h > 0 ? h : 1;
}

int present_active(void) {
    return s_ready && s_cfg.scaler != AB_SCALER_NATIVE;
}

void present_set_window(int w, int h) {
    if (w <= 0 || h <= 0) return;
    s_cfg.window_w = w;
    s_cfg.window_h = h;
    if (!present_active()) return;
    if (!SDL_GL_GetCurrentContext()) return;
    if (!s_ready || !s_tex || !s_fbo) return;

    int iw = present_render_w(), ih = present_render_h();
    if (iw <= 0 || ih <= 0) return;
    if (iw == s_fbo_w && ih == s_fbo_h) return;

    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    s_cfg.scaler == AB_SCALER_INTEGER ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    s_cfg.scaler == AB_SCALER_INTEGER ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);

    /* Drivers disagree about packed depth-stencil, and a few refuse the depth
     * attachment outright, so walk a few combinations and keep the first that
     * comes back complete. AB_FBO_DEPTH (packed24 | depth16 | none) forces one,
     * so the choice can be bisected without a rebuild.
     *
     * Note the ES2 spelling: the renderbuffer *storage* format for packed
     * depth+stencil is GL_DEPTH24_STENCIL8_OES (0x88F0); GL_DEPTH_STENCIL_OES
     * (0x84F9) is the attachment-side enum only. Passing it to
     * glRenderbufferStorage is INVALID_ENUM, which silently drops the depth
     * attachment -- the first version of this code did exactly that and quietly
     * landed on a 16-bit depth buffer the engine has no precision for. */
    glBindRenderbuffer(GL_RENDERBUFFER, s_depth);

    static const struct {
        unsigned fmt;                       /* renderbuffer storage format, 0 = none */
        unsigned attach;
        const char *name;
    } plans[] = {
        { 0x88F0u /*GL_DEPTH24_STENCIL8_OES*/, GL_DEPTH_STENCIL_ATTACHMENT_OES, "depth24-stencil8" },
        { GL_DEPTH_COMPONENT16,              GL_DEPTH_ATTACHMENT,              "depth16" },
        { 0,                                0,                               "color-only" },
    };

    const char *force = getenv("AB_FBO_DEPTH");
    int start = 0;
    if (force && *force) {
        if (!strncasecmp(force, "depth16", 7) || !strncasecmp(force, "16", 2)) start = 1;
        else if (!strncasecmp(force, "none", 4) || !strncasecmp(force, "color", 5)) start = 2;
    }

    GLenum st = GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    const char *mode = "none";

    while (glGetError() != GL_NO_ERROR) { /* clear stale errors */ }

    for (int k = start; k < (int)(sizeof plans / sizeof *plans); k++) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT_OES,
                                  GL_RENDERBUFFER, 0);
        if (plans[k].fmt) {
            glRenderbufferStorage(GL_RENDERBUFFER, plans[k].fmt, iw, ih);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, plans[k].attach, GL_RENDERBUFFER, s_depth);
        }
        st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st == GL_FRAMEBUFFER_COMPLETE) { mode = plans[k].name; break; }
        if (force && *force) break;     /* forced choice: report it and stop */
    }

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (st != GL_FRAMEBUFFER_COMPLETE) {
        /* glGetTexLevelParameteriv is ES3-only, so report what ES2 does give us:
         * the pending GL error, the target size and the driver string. */
        debugPrintf("[Present] FBO incomplete (0x%x) -- falling back to native "
                    "[tex=%u size=%dx%d glError=0x%x "
                    "GL_VERSION=%s GL_RENDERER=%s]\n",
                    (unsigned)st, (unsigned)s_tex, iw, ih,
                    (unsigned)glGetError(),
                    (const char *)glGetString(GL_VERSION),
                    (const char *)glGetString(GL_RENDERER));
        present_shutdown();
        s_cfg.scaler = AB_SCALER_NATIVE;
        return;
    }

    s_depth_attached = (mode[0] != 'n');
    s_fbo_w = iw;
    s_fbo_h = ih;
    debugPrintf("[Present] offscreen target %dx%d -> window %dx%d (depth: %s)\n",
                iw, ih, s_cfg.window_w, s_cfg.window_h, mode);
}

/* ------------------------------------------------------------------ state
 *
 * The engine configures GL state once during boot (blend, depth, its program,
 * its VBO, its vertex attrib arrays) and then relies on it surviving from frame
 * to frame -- Android builds never notice because the engine owns the whole
 * frame. Our clear and our upscale blit are guest draws: if we leave state
 * switched off afterwards, the engine's next frame silently draws with
 * blending and depth disabled, its sprites vanish and the screen goes dark.
 * So borrow the state, put every bit of it back, and only keep the two things
 * we own (our framebuffer binding and the viewport over it).
 */

enum { AB_NATTR = 2 };
static const GLuint s_attr_ids[AB_NATTR] = { AB_ATTRIB_POS, AB_ATTRIB_UV };

typedef struct {
    GLboolean scissor, stencil, depth, blend, cull;
    GLboolean color_mask[4];
    GLint   viewport[4];
    GLfloat clear_color[4];
    GLint   fbo, prog, array_buf, active_tex, tex2d;
    GLint   attr_enabled[AB_NATTR];
    GLint   attr_buf[AB_NATTR];
    GLint   attr_size[AB_NATTR];
    GLint   attr_type[AB_NATTR];
    GLint   attr_norm[AB_NATTR];
    GLint   attr_stride[AB_NATTR];
    GLvoid *attr_ptr[AB_NATTR];
} GlBorrow;

static void gl_save(GlBorrow *s) {
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    s->stencil = glIsEnabled(GL_STENCIL_TEST);
    s->depth   = glIsEnabled(GL_DEPTH_TEST);
    s->blend   = glIsEnabled(GL_BLEND);
    s->cull    = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->color_mask);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex2d);

    for (int i = 0; i < AB_NATTR; i++) {
        GLuint a = s_attr_ids[i];
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr_enabled[i]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &s->attr_buf[i]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s->attr_size[i]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s->attr_type[i]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &s->attr_norm[i]);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s->attr_stride[i]);
        glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &s->attr_ptr[i]);
    }
}

/* `keep_target` leaves our framebuffer/viewport in place (used after
 * present_begin, where the engine must find our render target bound);
 * `keep_viewport` likewise leaves the viewport alone. */
static void gl_restore(const GlBorrow *s, int keep_fbo, int keep_viewport) {
    if (!keep_fbo) glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
    if (!keep_viewport)
        glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);

    if (s->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (s->depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (s->blend)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (s->cull)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);

    glUseProgram((GLuint)s->prog);
    glActiveTexture((GLenum)s->active_tex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex2d);

    /* Our blit pointed attribs at our VBO; put the engine's pointers back. */
    for (int i = 0; i < AB_NATTR; i++) {
        GLuint a = s_attr_ids[i];
        if (s->attr_buf[i]) {
            /* ES2 has no client-side arrays: with no buffer bound there is
             * nothing valid to restore, and the attrib is disabled anyway. */
            glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->attr_buf[i]);
            glVertexAttribPointer(a, s->attr_size[i], s->attr_type[i],
                                  (GLboolean)s->attr_norm[i], s->attr_stride[i], s->attr_ptr[i]);
        }
        if (s->attr_enabled[i]) glEnableVertexAttribArray(a);
        else                    glDisableVertexAttribArray(a);
    }
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->array_buf);
}

void present_begin(void) {
    if (!present_active()) return;
    if (!SDL_GL_GetCurrentContext()) return;
    if (!s_fbo || s_fbo_w <= 0 || s_fbo_h <= 0) return;

    GlBorrow st;
    gl_save(&st);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_fbo_w, s_fbo_h);

    /* Our clear is a guest draw: the engine routinely hands the frame back with
     * the scissor (or stencil test) still switched on, which would clip it to
     * the last box the engine set and leave stale pixels everywhere else. */
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    GLbitfield bits = GL_COLOR_BUFFER_BIT;
    if (s_depth_attached) bits |= GL_DEPTH_BUFFER_BIT;
    glClear(bits);

    /* Hand the engine back exactly the state it left -- only our render target
     * and the viewport over it stay (those are ours, not its). */
    gl_restore(&st, 1, 1);

    static int traced;
    if (traced < 4) {
        GLint vp[4] = {0, 0, 0, 0};
        GLint fb = 0, prog = 0;
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        debugPrintf("[Present] begin#%d fbo=%d viewport=%dx%d prog=%d glError=0x%x\n",
                    traced, (int)fb, (int)vp[2], (int)vp[3], (int)prog,
                    (unsigned)glGetError());
        traced++;
    }
}

/* ------------------------------------------------------------- capture */

/* Dump the current front-of-swap frame to a BMP. Handy for bug reports and for
 * eyeballing the scaler without a screenshot tool (Wayland has no portable
 * screen-capture API -- hence doing it inside the renderer). */
int present_capture(const char *path) {
    if (!path || !*path) return -1;
    int w = s_cfg.window_w, h = s_cfg.window_h;
    if (w <= 0 || h <= 0) return -1;
    if (w > 16384) w = 16384;
    if (h > 16384) h = 16384;

    size_t stride = (size_t)w * 4;
    unsigned char *px = malloc(stride * (size_t)h);
    if (!px) return -1;

    /* AB_SCREENSHOT_TARGET=fbo dumps the offscreen target instead of the window,
     * which is how you tell "the engine drew nothing" from "the blit is wrong". */
    const char *tgt = getenv("AB_SCREENSHOT_TARGET");
    GLint want = (tgt && (tgt[0] == 'f' || tgt[0] == 'F')) ? (GLint)s_fbo : 0;
    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)want);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fb);

    /* GL hands back bottom-up rows; BMP wants the same, but SDL's surface is
     * top-down, so flip into a second buffer. */
    unsigned char *flipped = malloc(stride * (size_t)h);
    if (!flipped) { free(px); return -1; }
    for (int y = 0; y < h; y++)
        memcpy(flipped + (size_t)y * stride, px + (size_t)(h - 1 - y) * stride, stride);
    free(px);

    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, flipped,
                                              (int)stride);
    int rc = -1;
    if (surf) {
        rc = SDL_SaveBMP(surf, path) ? 0 : -1;
        SDL_DestroySurface(surf);
    }
    free(flipped);
    debugPrintf("[Present] frame capture %s %dx%d -> %s (%s)\n",
                (want ? "fbo" : "window"), w, h, path, rc == 0 ? "ok" : SDL_GetError());
    return rc;
}

/* Destination rectangle: fit the render target inside the window, preserving
 * aspect ratio (integer mode snaps to whole multiples of the source). */
static void dest_rect(int *x, int *y, int *w, int *h) {
    if (s_fbo_w <= 0 || s_fbo_h <= 0 || s_cfg.window_w <= 0 || s_cfg.window_h <= 0) {
        *x = *y = 0;
        *w = s_cfg.window_w > 0 ? s_cfg.window_w : 1;
        *h = s_cfg.window_h > 0 ? s_cfg.window_h : 1;
        return;
    }

    float sx = (float)s_cfg.window_w / (float)s_fbo_w;
    float sy = (float)s_cfg.window_h / (float)s_fbo_h;
    float s = sx < sy ? sx : sy;

    if (s_cfg.scaler == AB_SCALER_INTEGER) {
        s = (float)(int)s;
        if (s < 1.0f) s = 1.0f;
    }

    int dw = (int)(s_fbo_w * s + 0.5f);
    int dh = (int)(s_fbo_h * s + 0.5f);
    if (dw > s_cfg.window_w) dw = s_cfg.window_w;
    if (dh > s_cfg.window_h) dh = s_cfg.window_h;
    *w = dw; *h = dh;
    *x = (s_cfg.window_w - dw) / 2;
    *y = (s_cfg.window_h - dh) / 2;
}

void present_end(void) {
    if (!present_active()) return;
    if (!SDL_GL_GetCurrentContext()) return;
    if (s_cfg.window_w <= 0 || s_cfg.window_h <= 0) return;

    static int traced;
    if (traced < 4) {
        GLint vp[4] = {0, 0, 0, 0};
        GLint fb = 0, tex = 0;
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        debugPrintf("[Present] end#%d engine left fbo=%d viewport=%dx%d tex=%d "
                    "scissor=%d glError=0x%x\n",
                    traced, (int)fb, (int)vp[2], (int)vp[3], (int)tex,
                    glIsEnabled(GL_SCISSOR_TEST) ? 1 : 0, (unsigned)glGetError());
        traced++;
    }

    GlBorrow st;
    gl_save(&st);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* AB_NO_BLIT=1 stops after unbinding, so the window keeps whatever the
     * engine itself drew into the default framebuffer -- that is how you tell
     * "the engine renders into the window anyway" from "the engine used our
     * offscreen target". */
    if (getenv("AB_NO_BLIT")) {
        gl_restore(&st, 0, 0);
        return;
    }

    glViewport(0, 0, s_cfg.window_w, s_cfg.window_h);

    int x, y, w, h;
    dest_rect(&x, &y, &w, &h);

    /* Letterbox: clear the whole drawable, then draw the frame quad inside it.
     * Same reason as present_begin: unclipped, and no engine blending leaking
     * into the blit (premultiplied/alpha engine state would darken it). */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float nx0 = 2.0f * (float)x / (float)s_cfg.window_w - 1.0f;
    float nx1 = 2.0f * (float)(x + w) / (float)s_cfg.window_w - 1.0f;
    float ny0 = 1.0f - 2.0f * (float)y / (float)s_cfg.window_h;
    float ny1 = 1.0f - 2.0f * (float)(y + h) / (float)s_cfg.window_h;

    /* GL renders with a bottom-left origin, so texture v=0 is the bottom row of
     * what the engine drew: it has to land on the bottom of the destination
     * rectangle, not the top. Getting this backwards is why the first version of
     * the scaler showed the game upside down. */
    const GLfloat verts[16] = {
        nx0, ny1, 0.0f, 0.0f,   /* dest bottom-left  <- src (0,0) */
        nx1, ny1, 1.0f, 0.0f,   /* dest bottom-right <- src (1,0) */
        nx0, ny0, 0.0f, 1.0f,   /* dest top-left     <- src (0,1) */
        nx1, ny0, 1.0f, 1.0f,   /* dest top-right    <- src (1,1) */
    };

    glUseProgram(s_prog);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray((GLuint)s_pos_loc);
    glVertexAttribPointer((GLuint)s_pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
    glEnableVertexAttribArray((GLuint)s_uv_loc);
    glVertexAttribPointer((GLuint)s_uv_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (void *)(2 * sizeof(GLfloat)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glUniform1i(s_tex_loc, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* Put the engine's world back exactly as we found it -- program, VBO,
     * vertex attribs, texture unit, blend/depth/scissor flags, its framebuffer
     * binding and its viewport. Only the pixels in the window are ours. */
    gl_restore(&st, 0, 0);
}
