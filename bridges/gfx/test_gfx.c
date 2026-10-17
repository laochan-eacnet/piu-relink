#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x11_abi.h"
#include "gl_generated.h"
#include "../../lxrt/lxabi.h"

static const lx_sym *symbols;
static unsigned symbol_count;
static void *find_symbol(const char *name)
{
    for (unsigned i = 0; i < symbol_count; ++i) if (!strcmp(symbols[i].name, name)) return symbols[i].addr;
    fprintf(stderr, "missing provider symbol: %s\n", name); exit(2);
}
#define LOAD(name) __typeof__(&lx_##name) name = (__typeof__(&lx_##name))find_symbol(#name)
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "FAIL: %s (line %d; Win32=%lu)\n", #expression, __LINE__, GetLastError()); exit(3); } } while (0)
int main(int argc, char **argv)
{
    unsigned frames = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 12;
    if (frames < 8 || frames > 10000) { fputs("frames must be 8..10000\n", stderr); return 1; }
    char dll_path[MAX_PATH]; CHECK(GetModuleFileNameA(NULL, dll_path, sizeof(dll_path)));
    char *slash = strrchr(dll_path, '\\'); CHECK(slash);
    CHECK((size_t)(slash - dll_path) + sizeof("\\win\\lxgfx.dll") <= sizeof(dll_path));
    strcpy(slash, "\\win\\lxgfx.dll");
    HMODULE dll = LoadLibraryA(dll_path);
    CHECK(dll);
    lx_provider_symbols_fn provider = (lx_provider_symbols_fn)GetProcAddress(dll, "lx_provider_symbols");
    CHECK(provider); symbols = provider(&symbol_count); printf("provider symbols=%u\n", symbol_count);
    LOAD(XOpenDisplay); LOAD(glXChooseVisual); LOAD(XCreateColormap); LOAD(XCreateWindow); LOAD(XMapRaised);
    LOAD(XSetNormalHints); LOAD(XSetStandardProperties); LOAD(XFree); LOAD(glXCreateContext); LOAD(glXMakeCurrent);
    LOAD(glXSwapBuffers); LOAD(XPending); LOAD(XNextEvent); LOAD(XLookupString); LOAD(XStringToKeysym);
    LOAD(XKeysymToKeycode); LOAD(XQueryKeymap); LOAD(XGrabKeyboard); LOAD(XWarpPointer);
    LOAD(XF86VidModeGetAllModeLines); LOAD(XF86VidModeSetViewPort);
    LOAD(glGetString); LOAD(glViewport); LOAD(glMatrixMode); LOAD(glLoadIdentity); LOAD(glOrtho);
    LOAD(glGenTextures); LOAD(glBindTexture); LOAD(glTexParameteri); LOAD(glTexImage2D); LOAD(glEnable);
    LOAD(glActiveTexture); LOAD(glClientActiveTexture); LOAD(glCompressedTexImage2D);
    LOAD(glClearColor); LOAD(glClear); LOAD(glBegin); LOAD(glEnd); LOAD(glTexCoord2f); LOAD(glVertex2f);
    LOAD(glColor4f); LOAD(glReadPixels); LOAD(glDeleteTextures);
    LOAD(gluPerspective); LOAD(gluLookAt); LOAD(glGetFloatv);
    LOAD(glPushClientAttrib); LOAD(glPopClientAttrib); LOAD(glPushAttrib); LOAD(glPopAttrib);
    LOAD(glGenLists); LOAD(glNewList); LOAD(glEndList); LOAD(glCallList); LOAD(glDeleteLists); LOAD(glIsList); LOAD(glNormal3d);
    LOAD(glGetIntegerv); LOAD(glReadBuffer); LOAD(glPixelStorei);
    LXDisplay *d = XOpenDisplay(NULL); CHECK(d && d->default_screen == 0 && d->nscreens == 1);
    LXScreen *s = &d->screens[d->default_screen];
    printf("Display: screen=%d root=%u desktop=%dx%d black=%u\n", d->default_screen, s->root, s->width, s->height, s->black_pixel);
    const int attrs[] = {1, 4, 5, 12, 1, 13, 1, 0};
    LXVisualInfo *v = glXChooseVisual(d, d->default_screen, attrs); CHECK(v && v->visual && v->depth >= 24);
    int modes_count = 0; LXVidModeInfo **modes = NULL;
    CHECK(XF86VidModeGetAllModeLines(d, 0, &modes_count, &modes));
    CHECK(modes_count == 1 && modes[0]->hdisplay == s->width && modes[0]->vdisplay == s->height);
    printf("VidMode: count=%d mode=%ux%u private=%d\n", modes_count, modes[0]->hdisplay, modes[0]->vdisplay, modes[0]->privsize);
    CHECK(XF86VidModeSetViewPort(d, 0, 0, 0)); CHECK(XFree(modes));
    CHECK(XGrabKeyboard(d, s->root, 0, 1, 1, 0) == 0);
    LXSetWindowAttributes wa = {0}; wa.background_pixel = wa.border_pixel = s->black_pixel;
    wa.colormap = XCreateColormap(d, s->root, v->visual, 0); wa.event_mask = 0x28003;
    LXWindow w = XCreateWindow(d, s->root, 40, 40, 320, 240, 0, v->depth, 1, v->visual, 0x280a, &wa);
    CHECK(w);
    CHECK(XWarpPointer(d, 0, w, 0, 0, 0, 0, 0, 0));
    LXSizeHints hints = {0}; hints.flags = 1; hints.x = 40; hints.y = 40;
    CHECK(XSetNormalHints(d, w, &hints));
    CHECK(XSetStandardProperties(d, w, "lxgfx textured quad", "lxgfx", 0, NULL, 0, &hints));
    /* Actual piu order: context, XFree(visual), map, make-current. Also accept
       --map-first to exercise clients that map before creating the context. */
    int map_first = argc > 2 && !strcmp(argv[2], "--map-first");
    if (map_first) CHECK(XMapRaised(d, w));
    void *rc = glXCreateContext(d, v, NULL, 1); CHECK(rc); CHECK(XFree(v));
    if (!map_first) CHECK(XMapRaised(d, w));
    CHECK(glXMakeCurrent(d, w, rc));
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    static const char *binding_names[] = {"Q","E","S","Z","C","U","O","K","M","period","F1","F2","5"};
    static const unsigned binding_codes[] = {24,26,39,52,54,30,32,45,58,60,67,68,14};
    for (unsigned i = 0; i < sizeof(binding_codes)/sizeof(binding_codes[0]); ++i) {
        unsigned code = XKeysymToKeycode(d, XStringToKeysym(binding_names[i]));
        CHECK(code == binding_codes[i]); printf("binding %s=%u%s", binding_names[i], code, i == 12 ? "\n" : " ");
    }
    CHECK(XKeysymToKeycode(d, XStringToKeysym("Up")) == 111);
    CHECK(XKeysymToKeycode(d, XStringToKeysym("Control_R")) == 105);
    GLfloat matrix[16];
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(60.0, 4.0/3.0, .1, 100.0);
    glGetFloatv(GL_PROJECTION_MATRIX, matrix); CHECK(matrix[0] > 1.29f && matrix[0] < 1.31f);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); gluLookAt(0, 0, 2, 0, 0, 0, 0, 1, 0);
    glGetFloatv(GL_MODELVIEW_MATRIX, matrix); CHECK(matrix[14] == -2.0f);
    puts("GLU cdecl/double forwarding=PASS");
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS); glPopClientAttrib();
    glPushAttrib(GL_ALL_ATTRIB_BITS); glPopAttrib();
    GLuint list = glGenLists(1); CHECK(list);
    glNewList(list, GL_COMPILE); glNormal3d(0, 0, 1); glEndList(); CHECK(glIsList(list));
    glCallList(list); glDeleteLists(list, 1); CHECK(!glIsList(list));
    CHECK(glGetError() == GL_NO_ERROR); puts("GL1.1 attrib/list forwarding=PASS");
    RECT client; CHECK(GetClientRect((HWND)(uintptr_t)w, &client));
    glViewport(0, 0, 320, 240); glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glActiveTexture(0x84c0); glClientActiveTexture(0x84c0); glEnable(GL_TEXTURE_2D); glColor4f(1, 1, 1, 1);
    GLuint texture[2]; glGenTextures(2, texture);
    const unsigned char pixels[16] = {64,160,224,255,64,160,224,255,64,160,224,255,64,160,224,255};
    for (unsigned i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, texture[i]); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (!i) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        else {
            const unsigned char dxt1[8] = {0,248,0,248,0,0,0,0};
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x83f1, 4, 4, 0, sizeof(dxt1), dxt1);
        }
    }
    CHECK(glGetError() == GL_NO_ERROR);
    unsigned key_events = 0, client_events = 0, unmap_events = 0, expose_events = 0, configure_events = 0;
    const GLenum pack_names[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};
    const GLint pack_values[] = {8, 327, 2, 3};
    const GLint pack_defaults[] = {4, 0, 0, 0};
    for (unsigned frame = 0; frame < frames; ++frame) {
        HWND hwnd = (HWND)(uintptr_t)w;
        if (frame == 2) CHECK(PostMessageA(hwnd, WM_KEYDOWN, 'Q', 1 | (0x10 << 16)));
        if (frame == 3) CHECK(PostMessageA(hwnd, WM_KEYUP, 'Q', 1 | (0x10 << 16) | (1u << 30) | (1u << 31)));
        if (frame == 4) { CHECK(PostMessageA(hwnd, WM_KEYDOWN, VK_CONTROL, 1 | (0x1d << 16) | (1u << 24))); CHECK(PostMessageA(hwnd, WM_KEYDOWN, VK_UP, 1 | (0x48 << 16) | (1u << 24))); }
        if (frame == 5) { CHECK(PostMessageA(hwnd, WM_KEYUP, VK_UP, 1 | (0x48 << 16) | (1u << 24) | (1u << 31))); CHECK(PostMessageA(hwnd, WM_KEYUP, VK_CONTROL, 1 | (0x1d << 16) | (1u << 24) | (1u << 31))); }
        if (frame == frames - 1) CHECK(PostMessageA(hwnd, WM_CLOSE, 0, 0));
        while (XPending(d)) {
            struct { uint32_t before; LXEvent event; uint32_t after; } guarded = {0x12345678, {0}, 0x87654321};
            CHECK(XNextEvent(d, &guarded.event) == 0); CHECK(guarded.before == 0x12345678 && guarded.after == 0x87654321);
            LXEvent *e = &guarded.event;
            if (e->type == 2 || e->type == 3) {
                char text[8] = {0}; uint32_t sym = 0;
                int length = XLookupString(&e->xkey, text, sizeof(text), &sym, NULL);
                printf("key type=%d code=%u keysym=0x%x state=0x%x text_len=%d text0=0x%02x\n", e->type, e->xkey.keycode, sym, e->xkey.state, length, (unsigned char)text[0]);
                ++key_events;
            } else if (e->type == 33) ++client_events;
            else if (e->type == 18) ++unmap_events;
            else if (e->type == 12) ++expose_events;
            else if (e->type == 22) ++configure_events;
        }
        char bitmap[32]; CHECK(XQueryKeymap(d, bitmap));
        unsigned q_down = ((unsigned char)bitmap[24 >> 3] >> (24 & 7)) & 1;
        unsigned ctrl_down = ((unsigned char)bitmap[105 >> 3] >> (105 & 7)) & 1;
        unsigned up_down = ((unsigned char)bitmap[111 >> 3] >> (111 & 7)) & 1;
        if (frame >= 2 && frame <= 5) printf("keymap frame=%u Q=%u Control_R=%u Up=%u\n", frame, q_down, ctrl_down, up_down);
        CHECK(q_down == (frame == 2)); CHECK(ctrl_down == (frame == 4)); CHECK(up_down == (frame == 4));
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glBindTexture(GL_TEXTURE_2D, texture[frame & 1]);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(-.8f, -.8f); glTexCoord2f(1, 0); glVertex2f(.8f, -.8f);
        glTexCoord2f(1, 1); glVertex2f(.8f, .8f); glTexCoord2f(0, 1); glVertex2f(-.8f, .8f); glEnd();
        unsigned char pixel[4] = {0};
        glReadPixels(160, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(pixel[0] || pixel[1] || pixel[2]);
        if (frame < 2) printf("frame=%u texture=%s pixel=%u,%u,%u,%u\n", frame, frame & 1 ? "DXT1" : "RGBA", pixel[0], pixel[1], pixel[2], pixel[3]);
        CHECK(glGetError() == GL_NO_ERROR);
        glReadBuffer(GL_FRONT);
        for (unsigned i = 0; i < 4; ++i) glPixelStorei(pack_names[i], pack_values[i]);
        glXSwapBuffers(d, w);
        GLint state;
        glGetIntegerv(GL_READ_BUFFER, &state); CHECK(state == GL_FRONT);
        for (unsigned i = 0; i < 4; ++i) {
            glGetIntegerv(pack_names[i], &state); CHECK(state == pack_values[i]);
            glPixelStorei(pack_names[i], pack_defaults[i]);
        }
        glReadBuffer(GL_BACK);
        CHECK(glGetError() == GL_NO_ERROR); Sleep(16);
    }
    CHECK(key_events == 6 && client_events == 1 && unmap_events == 1 && expose_events && configure_events);
    printf("PASS swap read/pack state restoration frames=%u\n", frames);
    printf("PASS frames=%u key_events=%u ConfigureNotify=%u Expose=%u ClientMessage=%u UnmapNotify=%u\n", frames, key_events, configure_events, expose_events, client_events, unmap_events);
    glDeleteTextures(2, texture); CHECK(glXMakeCurrent(d, 0, NULL)); CHECK(wglDeleteContext((HGLRC)rc));
    CHECK(DestroyWindow((HWND)(uintptr_t)w)); FreeLibrary(dll); return 0;
}
