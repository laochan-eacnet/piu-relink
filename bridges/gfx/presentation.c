/* Game-sized default framebuffer emulation. The guest never binds FBOs; keep
 * its GL 1.x viewport, stencil, and read/draw-buffer semantics independent of
 * the native window size. Only GL 3.0 / ARB_framebuffer_object is supported. */
#include "gfx_internal.h"
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define GL_FRAMEBUFFER 0x8d40
#define GL_READ_FRAMEBUFFER 0x8ca8
#define GL_DRAW_FRAMEBUFFER 0x8ca9
#define GL_READ_FRAMEBUFFER_BINDING 0x8caa
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8ca6
#define GL_RENDERBUFFER 0x8d41
#define GL_RENDERBUFFER_BINDING 0x8ca7
#define GL_COLOR_ATTACHMENT0 0x8ce0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821a
#define GL_DEPTH24_STENCIL8 0x88f0
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_RGBA8 0x8058

typedef void (APIENTRY *GenObjects)(GLsizei, GLuint *);
typedef void (APIENTRY *DeleteObjects)(GLsizei, const GLuint *);
typedef void (APIENTRY *BindObject)(GLenum, GLuint);
typedef void (APIENTRY *RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *CheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static struct {
    GenObjects gen_framebuffers, gen_renderbuffers;
    DeleteObjects delete_framebuffers, delete_renderbuffers;
    BindObject bind_framebuffer, bind_renderbuffer;
    RenderbufferStorage storage;
    FramebufferRenderbuffer attach;
    CheckFramebufferStatus status;
    BlitFramebuffer blit;
    GLuint framebuffer, color, depth_stencil;
    int game_width, game_height, output_width, output_height;
    int initialized, available, failed;
    GLenum read_buffer, draw_buffer;
} render;
static struct AttribState { GLbitfield mask; GLenum read_buffer, draw_buffer; } *attrib;
static unsigned attrib_depth;
/* FBO queries must not expose a single-buffered, alpha-bearing offscreen
   surface where the guest expects the native window's selected visual. */
static const GLenum visual_names[] = {
    GL_DOUBLEBUFFER, GL_STEREO, GL_AUX_BUFFERS, GL_RED_BITS, GL_GREEN_BITS,
    GL_BLUE_BITS, GL_ALPHA_BITS, GL_INDEX_BITS, GL_DEPTH_BITS, GL_STENCIL_BITS,
    GL_ACCUM_RED_BITS, GL_ACCUM_GREEN_BITS, GL_ACCUM_BLUE_BITS, GL_ACCUM_ALPHA_BITS
};
static GLint visual_values[sizeof(visual_names) / sizeof(visual_names[0])];

static int has_extension(const char *extensions, const char *name)
{
    size_t n = strlen(name);
    const char *p = extensions;
    if (!p) return 0;
    while ((p = strstr(p, name))) {
        if ((p == extensions || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
        p += n;
    }
    return 0;
}
static void load_framebuffers(void)
{
    const char *version = (const char *)glGetString(GL_VERSION);
    int major = 0;
    if (version) sscanf(version, "%d", &major);
    if (major < 3 && !has_extension((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_framebuffer_object")) return;
#define LOAD(field, type, name) render.field = (type)gfx_extension(name); if (!render.field) return
    LOAD(gen_framebuffers, GenObjects, "glGenFramebuffers");
    LOAD(gen_renderbuffers, GenObjects, "glGenRenderbuffers");
    LOAD(delete_framebuffers, DeleteObjects, "glDeleteFramebuffers");
    LOAD(delete_renderbuffers, DeleteObjects, "glDeleteRenderbuffers");
    LOAD(bind_framebuffer, BindObject, "glBindFramebuffer");
    LOAD(bind_renderbuffer, BindObject, "glBindRenderbuffer");
    LOAD(storage, RenderbufferStorage, "glRenderbufferStorage");
    LOAD(attach, FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD(status, CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(blit, BlitFramebuffer, "glBlitFramebuffer");
#undef LOAD
    render.available = 1;
}
void gfx_render_destroy(void)
{
    if (!render.framebuffer) return;
    render.bind_framebuffer(GL_FRAMEBUFFER, 0);
    render.delete_framebuffers(1, &render.framebuffer);
    render.delete_renderbuffers(1, &render.color);
    render.delete_renderbuffers(1, &render.depth_stencil);
    render.framebuffer = render.color = render.depth_stencil = 0;
    glReadBuffer(render.read_buffer); glDrawBuffer(render.draw_buffer);
}
static void update_framebuffer(void)
{
    int scaled = render.game_width != render.output_width || render.game_height != render.output_height;
    if (!scaled) {
        int was_scaled = render.framebuffer != 0;
        gfx_render_destroy();
        if (was_scaled) gfx_render_black();
        return;
    }
    if (render.framebuffer || render.failed) return;
    if (!render.available) {
        fputs("lxgfx: resolution scaling requires OpenGL 3.0 or GL_ARB_framebuffer_object; using direct rendering\n", stderr);
        render.failed = 1;
        return;
    }
    GLint renderbuffer;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    GLint read_buffer, draw_buffer;
    glGetIntegerv(GL_READ_BUFFER, &read_buffer); glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
    render.read_buffer = (GLenum)read_buffer; render.draw_buffer = (GLenum)draw_buffer;
    render.gen_framebuffers(1, &render.framebuffer);
    render.bind_framebuffer(GL_FRAMEBUFFER, render.framebuffer);
    render.gen_renderbuffers(1, &render.color);
    render.bind_renderbuffer(GL_RENDERBUFFER, render.color);
    render.storage(GL_RENDERBUFFER, GL_RGBA8, render.game_width, render.game_height);
    render.attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, render.color);
    render.gen_renderbuffers(1, &render.depth_stencil);
    render.bind_renderbuffer(GL_RENDERBUFFER, render.depth_stencil);
    render.storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, render.game_width, render.game_height);
    render.attach(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, render.depth_stencil);
    render.bind_renderbuffer(GL_RENDERBUFFER, (GLuint)renderbuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0); glDrawBuffer(GL_COLOR_ATTACHMENT0);
    GLenum status = render.status(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "lxgfx: game framebuffer incomplete (0x%x); using direct rendering\n", status);
        gfx_render_destroy(); render.failed = 1;
        return;
    }
    fprintf(stderr, "lxgfx: scaling game framebuffer %dx%d to output %dx%d (RGBA8 + DEPTH24_STENCIL8)\n",
            render.game_width, render.game_height, render.output_width, render.output_height);
    gfx_render_black();
}
void gfx_render_size(int game_width, int game_height, int output_width, int output_height)
{
    render.game_width = game_width; render.game_height = game_height;
    render.output_width = output_width; render.output_height = output_height;
    /* WM_SIZE can precede make-current. GL work is deferred in that case. */
    if (render.initialized && wglGetCurrentContext()) update_framebuffer();
}
void gfx_render_current(void)
{
    int first = !render.initialized;
    if (!render.initialized) {
        render.initialized = 1;
        render.read_buffer = render.draw_buffer = GL_BACK;
        for (unsigned i = 0; i < sizeof(visual_names) / sizeof(visual_names[0]); ++i)
            glGetIntegerv(visual_names[i], &visual_values[i]);
        load_framebuffers();
        glViewport(0, 0, render.game_width, render.game_height);
        glScissor(0, 0, render.game_width, render.game_height);
        GLint depth; glGetIntegerv(GL_MAX_ATTRIB_STACK_DEPTH, &depth);
        attrib = (struct AttribState *)calloc((size_t)depth, sizeof(*attrib));
        if (!attrib) { fputs("lxgfx: cannot allocate GL attribute state\n", stderr); abort(); }
    }
    update_framebuffer();
    if (render.framebuffer) render.bind_framebuffer(GL_FRAMEBUFFER, render.framebuffer);
    if (first && !render.framebuffer) gfx_render_black();
}
int gfx_render_scaled(void) { return render.framebuffer != 0; }
struct PresentationState {
    GLint read_fbo, draw_fbo, read_buffer, draw_buffer, native_read, native_draw;
    GLfloat clear[4];
    GLboolean mask[4], scissor;
};
static void presentation_begin(struct PresentationState *state)
{
    state->read_fbo = state->draw_fbo = 0;
    state->scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (render.available) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_fbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_fbo);
    }
    glGetIntegerv(GL_READ_BUFFER, &state->read_buffer); glGetIntegerv(GL_DRAW_BUFFER, &state->draw_buffer);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, state->clear); glGetBooleanv(GL_COLOR_WRITEMASK, state->mask);
    if (render.available) render.bind_framebuffer(GL_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &state->native_read); glGetIntegerv(GL_DRAW_BUFFER, &state->native_draw);
    glDrawBuffer(GL_BACK); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
static void presentation_end(const struct PresentationState *state)
{
    if (render.available) render.bind_framebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer((GLenum)state->native_read); glDrawBuffer((GLenum)state->native_draw);
    if (render.available) {
        render.bind_framebuffer(GL_READ_FRAMEBUFFER, (GLuint)state->read_fbo);
        render.bind_framebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)state->draw_fbo);
        glReadBuffer((GLenum)state->read_buffer); glDrawBuffer((GLenum)state->draw_buffer);
    }
    glClearColor(state->clear[0], state->clear[1], state->clear[2], state->clear[3]);
    glColorMask(state->mask[0], state->mask[1], state->mask[2], state->mask[3]);
    if (state->scissor) glEnable(GL_SCISSOR_TEST);
}
void gfx_render_black(void)
{
    if (!render.initialized || !wglGetCurrentContext() || render.output_width <= 0 || render.output_height <= 0) return;
    struct PresentationState state;
    presentation_begin(&state);
    /* Prime both native buffers, even with [debug] no_present=true. This is not a game
       swap and must not advance snapshot numbering or FPS counters. */
    SwapBuffers(wglGetCurrentDC());
    glClear(GL_COLOR_BUFFER_BIT);
    presentation_end(&state);
}
void gfx_render_present(void)
{
    if (!render.framebuffer || render.output_width <= 0 || render.output_height <= 0) return;
    struct PresentationState state;
    presentation_begin(&state);
    render.bind_framebuffer(GL_READ_FRAMEBUFFER, render.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    int width = render.output_width, height = render.output_height;
    if ((int64_t)width * render.game_height > (int64_t)height * render.game_width)
        width = (int)((int64_t)height * render.game_width / render.game_height);
    else height = (int)((int64_t)width * render.game_height / render.game_width);
    int x = (render.output_width - width) / 2, y = (render.output_height - height) / 2;
    render.blit(0, 0, render.game_width, render.game_height, x, y, x + width, y + height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    presentation_end(&state);
}
void __cdecl lx_glReadBuffer(GLenum mode)
{
    if (render.framebuffer && (mode == GL_BACK || mode == GL_FRONT)) {
        render.read_buffer = mode; glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else { glReadBuffer(mode); if (!render.framebuffer || mode == GL_NONE) render.read_buffer = mode; }
}
void __cdecl lx_glDrawBuffer(GLenum mode)
{
    if (render.framebuffer && (mode == GL_BACK || mode == GL_FRONT || mode == GL_FRONT_AND_BACK)) {
        render.draw_buffer = mode; glDrawBuffer(GL_COLOR_ATTACHMENT0);
    } else { glDrawBuffer(mode); if (!render.framebuffer || mode == GL_NONE) render.draw_buffer = mode; }
}
static int guest_value(GLenum name, GLint *value)
{
    if (!render.framebuffer) return 0;
    for (unsigned i = 0; i < sizeof(visual_names) / sizeof(visual_names[0]); ++i)
        if (name == visual_names[i]) { *value = visual_values[i]; return 1; }
    if (name == GL_READ_BUFFER) *value = (GLint)render.read_buffer;
    else if (name == GL_DRAW_BUFFER) *value = (GLint)render.draw_buffer;
    else if (name == GL_READ_FRAMEBUFFER_BINDING || name == GL_DRAW_FRAMEBUFFER_BINDING) *value = 0;
    else return 0;
    return 1;
}
void __cdecl lx_glGetIntegerv(GLenum name, GLint *value)
{
    if (!guest_value(name, value)) glGetIntegerv(name, value);
}
void __cdecl lx_glGetFloatv(GLenum name, GLfloat *value)
{
    GLint v; if (guest_value(name, &v)) *value = (GLfloat)v; else glGetFloatv(name, value);
}
void __cdecl lx_glGetDoublev(GLenum name, GLdouble *value)
{
    GLint v; if (guest_value(name, &v)) *value = (GLdouble)v; else glGetDoublev(name, value);
}
void __cdecl lx_glGetBooleanv(GLenum name, GLboolean *value)
{
    GLint v; if (guest_value(name, &v)) *value = v ? GL_TRUE : GL_FALSE; else glGetBooleanv(name, value);
}
void __cdecl lx_glPushAttrib(GLbitfield mask)
{
    GLint before, after, read_buffer, draw_buffer;
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &before);
    if (render.framebuffer) {
        read_buffer = (GLint)render.read_buffer; draw_buffer = (GLint)render.draw_buffer;
    } else { glGetIntegerv(GL_READ_BUFFER, &read_buffer); glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer); }
    glPushAttrib(mask);
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &after);
    if (after > before) {
        attrib[attrib_depth].mask = mask;
        attrib[attrib_depth].read_buffer = (GLenum)read_buffer;
        attrib[attrib_depth].draw_buffer = (GLenum)draw_buffer;
        ++attrib_depth;
    }
}
void __cdecl lx_glPopAttrib(void)
{
    GLint before, after;
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &before);
    glPopAttrib();
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &after);
    if (after < before && attrib_depth) {
        --attrib_depth;
        if (attrib[attrib_depth].mask & GL_PIXEL_MODE_BIT) render.read_buffer = attrib[attrib_depth].read_buffer;
        if (attrib[attrib_depth].mask & GL_COLOR_BUFFER_BIT) render.draw_buffer = attrib[attrib_depth].draw_buffer;
    }
}
