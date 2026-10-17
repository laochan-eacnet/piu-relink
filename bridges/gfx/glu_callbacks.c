/* Native GLU invokes callbacks as stdcall; Linux guests register cdecl callbacks.
 * Opaque handles keep callbacks per object, and every GLU entry brackets its
 * synchronous native call with a thread-local active-handle save/restore. This
 * supports nested callbacks and distinct objects on concurrent threads without
 * executable thunks or a global callback registry. Native GLU's same-object
 * concurrency rules still apply; guest exceptions/longjmp cannot cross GLU.
 * Windows GLU 1.2 supports all twelve tess signatures, plus GLU_ERROR for
 * quadrics/NURBS (not the additional GLU 1.3 NURBS tessellation callbacks). */
#include "glu_callbacks.h"
#include <stdlib.h>

/* Keep the native pointer first so generated thunks need no kind dispatch. */
typedef struct GfxGluObject { void *native; } GfxGluObject;
typedef struct GfxTess {
    GfxGluObject object;
    gfx_glu_callback callbacks[12];
} GfxTess;
typedef struct GfxErrorObject {
    GfxGluObject object;
    gfx_glu_callback error;
} GfxErrorObject;
static __declspec(thread) GfxGluObject *active;

typedef void (CALLBACK *NativeCallback)(void);
typedef void (__cdecl *GuestEnum)(GLenum);
typedef void (__cdecl *GuestBoolean)(GLboolean);
typedef void (__cdecl *GuestPointer)(void *);
typedef void (__cdecl *GuestCombine)(GLdouble *, void **, GLfloat *, void **);
typedef void (__cdecl *GuestEnumData)(GLenum, void *);
typedef void (__cdecl *GuestBooleanData)(GLboolean, void *);
typedef void (__cdecl *GuestPointerData)(void *, void *);
typedef void (__cdecl *GuestCombineData)(GLdouble *, void **, GLfloat *, void **, void *);

void *gfx_glu_native(void *object)
{
    return object ? ((GfxGluObject *)object)->native : NULL;
}
void *gfx_glu_enter(void *object)
{
    GfxGluObject *previous = active;
    active = (GfxGluObject *)object;
    return previous;
}
void gfx_glu_leave(void *previous)
{
    active = (GfxGluObject *)previous;
}
static gfx_glu_callback tess_callback(unsigned slot)
{
    /* A trampoline can only be called synchronously within a wrapped GLU API. */
    return ((GfxTess *)active)->callbacks[slot];
}
static void CALLBACK tess_begin(GLenum mode)
{
    gfx_glu_callback fn = tess_callback(0);
    if (fn) ((GuestEnum)fn)(mode);
}
static void CALLBACK tess_vertex(void *vertex)
{
    gfx_glu_callback fn = tess_callback(1);
    if (fn) ((GuestPointer)fn)(vertex);
}
static void CALLBACK tess_end(void)
{
    gfx_glu_callback fn = tess_callback(2);
    if (fn) fn();
}
static void CALLBACK tess_error(GLenum error)
{
    gfx_glu_callback fn = tess_callback(3);
    if (fn) ((GuestEnum)fn)(error);
}
static void CALLBACK tess_edge(GLboolean edge)
{
    gfx_glu_callback fn = tess_callback(4);
    if (fn) ((GuestBoolean)fn)(edge);
}
static void CALLBACK tess_combine(GLdouble coords[3], void *vertices[4], GLfloat weights[4], void **output)
{
    gfx_glu_callback fn = tess_callback(5);
    if (fn) ((GuestCombine)fn)(coords, vertices, weights, output);
}
static void CALLBACK tess_begin_data(GLenum mode, void *polygon)
{
    gfx_glu_callback fn = tess_callback(6);
    if (fn) ((GuestEnumData)fn)(mode, polygon);
}
static void CALLBACK tess_vertex_data(void *vertex, void *polygon)
{
    gfx_glu_callback fn = tess_callback(7);
    if (fn) ((GuestPointerData)fn)(vertex, polygon);
}
static void CALLBACK tess_end_data(void *polygon)
{
    gfx_glu_callback fn = tess_callback(8);
    if (fn) ((GuestPointer)fn)(polygon);
}
static void CALLBACK tess_error_data(GLenum error, void *polygon)
{
    gfx_glu_callback fn = tess_callback(9);
    if (fn) ((GuestEnumData)fn)(error, polygon);
}
static void CALLBACK tess_edge_data(GLboolean edge, void *polygon)
{
    gfx_glu_callback fn = tess_callback(10);
    if (fn) ((GuestBooleanData)fn)(edge, polygon);
}
static void CALLBACK tess_combine_data(GLdouble coords[3], void *vertices[4], GLfloat weights[4], void **output, void *polygon)
{
    gfx_glu_callback fn = tess_callback(11);
    if (fn) ((GuestCombineData)fn)(coords, vertices, weights, output, polygon);
}
static void CALLBACK object_error(GLenum error)
{
    gfx_glu_callback fn = ((GfxErrorObject *)active)->error;
    if (fn) ((GuestEnum)fn)(error);
}
static const NativeCallback tess_trampolines[12] = {
    (NativeCallback)tess_begin, (NativeCallback)tess_vertex, (NativeCallback)tess_end,
    (NativeCallback)tess_error, (NativeCallback)tess_edge, (NativeCallback)tess_combine,
    (NativeCallback)tess_begin_data, (NativeCallback)tess_vertex_data, (NativeCallback)tess_end_data,
    (NativeCallback)tess_error_data, (NativeCallback)tess_edge_data, (NativeCallback)tess_combine_data
};

GLUtesselator *__cdecl lx_gluNewTess(void)
{
    GfxTess *object = (GfxTess *)calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->object.native = gluNewTess();
    if (!object->object.native) { free(object); return NULL; }
    return (GLUtesselator *)object;
}
void __cdecl lx_gluDeleteTess(GLUtesselator *tess)
{
    if (!tess) return;
    void *previous = gfx_glu_enter(tess);
    gluDeleteTess((GLUtesselator *)gfx_glu_native(tess));
    gfx_glu_leave(previous);
    free(tess);
}
void __cdecl lx_gluTessCallback(GLUtesselator *tess, GLenum which, gfx_glu_callback fn)
{
    GfxTess *object = (GfxTess *)tess;
    NativeCallback callback = NULL;
    unsigned slot = which - GLU_TESS_BEGIN;
    if (slot < 12) {
        object->callbacks[slot] = fn;
        if (fn) callback = tess_trampolines[slot];
    }
    void *previous = gfx_glu_enter(object);
    gluTessCallback((GLUtesselator *)object->object.native, which, callback);
    gfx_glu_leave(previous);
}
GLUquadric *__cdecl lx_gluNewQuadric(void)
{
    GfxErrorObject *object = (GfxErrorObject *)calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->object.native = gluNewQuadric();
    if (!object->object.native) { free(object); return NULL; }
    return (GLUquadric *)object;
}
void __cdecl lx_gluDeleteQuadric(GLUquadric *quadric)
{
    if (!quadric) return;
    void *previous = gfx_glu_enter(quadric);
    gluDeleteQuadric((GLUquadric *)gfx_glu_native(quadric));
    gfx_glu_leave(previous);
    free(quadric);
}
void __cdecl lx_gluQuadricCallback(GLUquadric *quadric, GLenum which, gfx_glu_callback fn)
{
    GfxErrorObject *object = (GfxErrorObject *)quadric;
    NativeCallback callback = NULL;
    if (which == GLU_ERROR) {
        object->error = fn;
        if (fn) callback = (NativeCallback)object_error;
    }
    void *previous = gfx_glu_enter(object);
    gluQuadricCallback((GLUquadric *)object->object.native, which, callback);
    gfx_glu_leave(previous);
}
GLUnurbs *__cdecl lx_gluNewNurbsRenderer(void)
{
    GfxErrorObject *object = (GfxErrorObject *)calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->object.native = gluNewNurbsRenderer();
    if (!object->object.native) { free(object); return NULL; }
    return (GLUnurbs *)object;
}
void __cdecl lx_gluDeleteNurbsRenderer(GLUnurbs *nurbs)
{
    if (!nurbs) return;
    void *previous = gfx_glu_enter(nurbs);
    gluDeleteNurbsRenderer((GLUnurbs *)gfx_glu_native(nurbs));
    gfx_glu_leave(previous);
    free(nurbs);
}
void __cdecl lx_gluNurbsCallback(GLUnurbs *nurbs, GLenum which, gfx_glu_callback fn)
{
    GfxErrorObject *object = (GfxErrorObject *)nurbs;
    NativeCallback callback = NULL;
    if (which == GLU_ERROR) {
        object->error = fn;
        if (fn) callback = (NativeCallback)object_error;
    }
    void *previous = gfx_glu_enter(object);
    gluNurbsCallback((GLUnurbs *)object->object.native, which, callback);
    gfx_glu_leave(previous);
}
