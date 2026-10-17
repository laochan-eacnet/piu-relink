#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.lpszClassName = "lxwgltest"; wc.style = CS_OWNDC;
    if (!RegisterClassA(&wc)) return 1;
    HWND w = CreateWindowA(wc.lpszClassName, "x86 WGL probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 400, 300, NULL, NULL, NULL, NULL);
    HDC dc = GetDC(w); PIXELFORMATDESCRIPTOR p = {0}; p.nSize = sizeof(p); p.nVersion = 1;
    p.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; p.iPixelType = PFD_TYPE_RGBA; p.cColorBits = 24; p.cDepthBits = 24; p.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &p); if (!pf || !SetPixelFormat(dc, pf, &p)) { printf("pixel format failed: %lu\n", GetLastError()); return 2; }
    HGLRC rc = wglCreateContext(dc); if (!rc || !wglMakeCurrent(dc, rc)) { printf("context failed: %lu\n", GetLastError()); return 3; }
    printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    const char *e = (const char *)glGetString(GL_EXTENSIONS);
    printf("ARB_multitexture=%d ARB_texture_compression=%d S3TC=%d\n", e && strstr(e, "GL_ARB_multitexture") != NULL, e && strstr(e, "GL_ARB_texture_compression") != NULL, e && (strstr(e, "GL_EXT_texture_compression_s3tc") || strstr(e, "GL_S3_s3tc")));
    printf("wglGetProcAddress: glActiveTexture=%p glClientActiveTexture=%p glCompressedTexImage2D=%p\n", (void *)wglGetProcAddress("glActiveTexture"), (void *)wglGetProcAddress("glClientActiveTexture"), (void *)wglGetProcAddress("glCompressedTexImage2D"));
    HMODULE icd = GetModuleHandleA("OpenGLOn12.dll"); char path[MAX_PATH]; if (icd && GetModuleFileNameA(icd, path, sizeof(path))) printf("OpenGL ICD=%s\n", path);
    for (int i = 0; i < 8; ++i) { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } glClearColor(.2f, .4f, .7f, 1); glClear(GL_COLOR_BUFFER_BIT); SwapBuffers(dc); Sleep(16); }
    wglMakeCurrent(NULL, NULL); wglDeleteContext(rc); ReleaseDC(w, dc); DestroyWindow(w); return 0;
}
