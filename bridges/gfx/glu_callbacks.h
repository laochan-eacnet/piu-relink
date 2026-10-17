#pragma once
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>

/* Linux GLU objects are opaque guest handles wrapping native GLU objects.
 * Generated object-taking thunks must unwrap and enter/leave their handle;
 * callbacks registered by the guest are cdecl, never native CALLBACK/stdcall. */
typedef void (__cdecl *gfx_glu_callback)(void);
void *gfx_glu_native(void *object);
void *gfx_glu_enter(void *object);
void gfx_glu_leave(void *previous);

GLUtesselator *__cdecl lx_gluNewTess(void);
void __cdecl lx_gluDeleteTess(GLUtesselator *tess);
void __cdecl lx_gluTessCallback(GLUtesselator *tess, GLenum which, gfx_glu_callback fn);
GLUquadric *__cdecl lx_gluNewQuadric(void);
void __cdecl lx_gluDeleteQuadric(GLUquadric *quadric);
void __cdecl lx_gluQuadricCallback(GLUquadric *quadric, GLenum which, gfx_glu_callback fn);
GLUnurbs *__cdecl lx_gluNewNurbsRenderer(void);
void __cdecl lx_gluDeleteNurbsRenderer(GLUnurbs *nurbs);
void __cdecl lx_gluNurbsCallback(GLUnurbs *nurbs, GLenum which, gfx_glu_callback fn);
