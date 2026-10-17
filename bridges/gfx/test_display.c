/* Runs copies of this EXE in a private directory so phoenixtools.ini never overwrites
 * the developer's config. Native GL calls inspect the real window framebuffer;
 * all game operations go through the provider's cdecl entry points. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x11_abi.h"
#include "gl_generated.h"
#include "../../lxrt/lxabi.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s (Win32=%lu GL=0x%x)\n", __FILE__, __LINE__, #x, GetLastError(), glGetError()); exit(3); } } while (0)
static const lx_sym *symbols;
static unsigned symbol_count;
static void *find_symbol(const char *name)
{
    for (unsigned i = 0; i < symbol_count; ++i) if (!strcmp(symbols[i].name, name)) return symbols[i].addr;
    fprintf(stderr, "missing %s\n", name); exit(2);
}
#define LOAD(name) __typeof__(&lx_##name) name = (__typeof__(&lx_##name))find_symbol(#name)
static void native_getint(GLenum name, GLint *value) { glGetIntegerv(name, value); }
static void native_readbuffer(GLenum mode) { glReadBuffer(mode); }
static void native_readpixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{ glReadPixels(x,y,width,height,format,type,pixels); }
static void check_black_window(HWND hwnd)
{
    RECT client; CHECK(GetClientRect(hwnd,&client));
    HDC dc = GetDC(hwnd); CHECK(dc);
    HDC memory = CreateCompatibleDC(dc); CHECK(memory);
    HBITMAP bitmap = CreateCompatibleBitmap(dc,client.right,client.bottom); CHECK(bitmap);
    HGDIOBJ old = SelectObject(memory,bitmap); CHECK(old);
    FillRect(memory,&client,(HBRUSH)GetStockObject(WHITE_BRUSH));
    CHECK(PrintWindow(hwnd,memory,PW_CLIENTONLY));
    CHECK(GetPixel(memory,0,0) == RGB(0,0,0));
    CHECK(GetPixel(memory,client.right/2,client.bottom/2) == RGB(0,0,0));
    CHECK(GetPixel(memory,client.right-1,client.bottom-1) == RGB(0,0,0));
    CHECK(SelectObject(memory,old)); CHECK(DeleteObject(bitmap)); CHECK(DeleteDC(memory)); CHECK(ReleaseDC(hwnd,dc));
}
static void drain(LXDisplay *d, int (*pending)(LXDisplay *), int (*next)(LXDisplay *, LXEvent *))
{
    while (pending(d)) {
        LXEvent event; CHECK(next(d, &event) == 0);
        if (event.type == 22) CHECK(event.xconfigure.width == 320 && event.xconfigure.height == 240);
        if (event.type == 12) CHECK(event.xexpose.x == 0 && event.xexpose.y == 0 && event.xexpose.width == 320 && event.xexpose.height == 240);
    }
}
static int child(const char *mode)
{
    CHECK(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    HMODULE dll = LoadLibraryA("lxgfx.dll"); CHECK(dll);
    lx_provider_symbols_fn provider = (lx_provider_symbols_fn)GetProcAddress(dll, "lx_provider_symbols"); CHECK(provider);
    symbols = provider(&symbol_count);
    LOAD(XOpenDisplay); LOAD(glXChooseVisual); LOAD(XCreateWindow); LOAD(XMapRaised); LOAD(XFree);
    LOAD(glXCreateContext); LOAD(glXMakeCurrent); LOAD(glXSwapBuffers); LOAD(XPending); LOAD(XNextEvent);
    LOAD(XF86VidModeSwitchToMode); LOAD(glClearColor); LOAD(glClear); LOAD(glReadBuffer); LOAD(glDrawBuffer);
    LOAD(glReadPixels); LOAD(glGetIntegerv); LOAD(glEnable); LOAD(glDisable); LOAD(glScissor); LOAD(glColorMask);
    LOAD(glPushAttrib); LOAD(glPopAttrib);
    LXDisplay *d = XOpenDisplay(NULL); CHECK(d);
    const int attrs[] = {1,4,5,12,1,13,1,0};
    LXVisualInfo *v = glXChooseVisual(d, 0, attrs); CHECK(v);
    MONITORINFOEXA monitor = {0}; monitor.cbSize = sizeof(monitor);
    POINT origin = {40,40}; CHECK(GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&monitor));
    DEVMODEA desktop = {0}; desktop.dmSize = sizeof(desktop); CHECK(EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &desktop));
    LXSetWindowAttributes wa = {0}; wa.event_mask = (1 << 15) | (1 << 17);
    LXWindow w = XCreateWindow(d, 1, 40, 40, 320, 240, 0, v->depth, 1, v->visual, 1 << 11, &wa); CHECK(w);
    HWND hwnd = (HWND)(uintptr_t)w;
    void *rc = glXCreateContext(d, v, NULL, 1); CHECK(rc); CHECK(XFree(v));
    CHECK(XMapRaised(d, w)); check_black_window(hwnd);
    CHECK(glXMakeCurrent(d, w, rc)); drain(d, XPending, XNextEvent);
    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    RECT client, outer; CHECK(GetClientRect(hwnd, &client)); CHECK(GetWindowRect(hwnd, &outer));
    int windowed = !strcmp(mode, "windowed") || !strcmp(mode, "invalid") || !strcmp(mode, "invalid-size");
    if (windowed) {
        CHECK((style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW);
        CHECK(client.right == (!strcmp(mode, "windowed") ? 960 : 320));
        CHECK(client.bottom == (!strcmp(mode, "windowed") ? 540 : 240));
    } else {
        CHECK((style & WS_POPUP) && !(style & WS_CAPTION));
        CHECK(GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&monitor));
        CHECK(EqualRect(&outer, &monitor.rcMonitor));
        if (!strcmp(mode, "fullscreen")) CHECK(GetWindowLongPtrA(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST);
    }
    LXVidModeInfo advisory = {0}; advisory.hdisplay = 640; advisory.vdisplay = 480;
    CHECK(XF86VidModeSwitchToMode(d, 0, &advisory));
    CHECK(GetWindowLongPtrA(hwnd, GWL_STYLE) == style); /* guest must not override config */
    typedef void (APIENTRY *BindFramebuffer)(GLenum, GLuint);
    BindFramebuffer bind = (BindFramebuffer)wglGetProcAddress("glBindFramebuffer"); CHECK(bind);
    GLint initial_fbo; native_getint(0x8ca6,&initial_fbo);
    bind(0x8d40,0); native_readbuffer(GL_FRONT);
    unsigned char initial[4]; native_readpixels(client.right/2,client.bottom/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,initial);
    CHECK(!initial[0] && !initial[1] && !initial[2]);
    const GLenum visual_names[] = {GL_DOUBLEBUFFER,GL_STEREO,GL_ALPHA_BITS,GL_DEPTH_BITS,GL_STENCIL_BITS,GL_ACCUM_RED_BITS};
    GLint visual_values[sizeof(visual_names)/sizeof(visual_names[0])];
    for (unsigned i = 0; i < sizeof(visual_names)/sizeof(visual_names[0]); ++i) native_getint(visual_names[i],&visual_values[i]);
    native_readbuffer(GL_BACK); bind(0x8d40,(GLuint)initial_fbo);
    printf("PASS %s boot PrintWindow=black native-front=black\n",mode);
    unsigned iterations = !strcmp(mode, "windowed") ? 4 : 1;
    for (unsigned i = 0; i < iterations; ++i) {
        if (i) {
            int width = i == 1 ? 640 : i == 2 ? 320 : 600;
            int height = i == 1 ? 480 : i == 2 ? 240 : 300;
            RECT r = {0,0,width,height}; CHECK(AdjustWindowRectEx(&r, (DWORD)style, FALSE, 0));
            CHECK(SetWindowPos(hwnd, NULL, 0,0,r.right-r.left,r.bottom-r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
            drain(d, XPending, XNextEvent); CHECK(GetClientRect(hwnd, &client));
            CHECK(client.right == width && client.bottom == height);
        }
        GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
        CHECK(viewport[0] == 0 && viewport[1] == 0 && viewport[2] == 320 && viewport[3] == 240);
        for (unsigned q = 0; q < sizeof(visual_names)/sizeof(visual_names[0]); ++q) {
            GLint value; glGetIntegerv(visual_names[q],&value); CHECK(value == visual_values[q]);
        }
        GLint depth, stencil; glGetIntegerv(GL_DEPTH_BITS, &depth); glGetIntegerv(GL_STENCIL_BITS, &stencil);
        CHECK(depth >= 24 && stencil >= 8);
        glReadBuffer(GL_BACK); glDrawBuffer(GL_BACK);
        glDisable(GL_SCISSOR_TEST); glColorMask(1,1,1,1); glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor(319,0,1,240); glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
        unsigned char pixel[4]; glReadPixels(319,120,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        CHECK(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0);
        glReadBuffer(GL_FRONT); glDrawBuffer(GL_FRONT);
        glPushAttrib(GL_COLOR_BUFFER_BIT | GL_PIXEL_MODE_BIT);
        glReadBuffer(GL_BACK); glDrawBuffer(GL_BACK); glPopAttrib();
        GLint state; glGetIntegerv(GL_READ_BUFFER, &state); CHECK(state == GL_FRONT);
        glGetIntegerv(GL_DRAW_BUFFER, &state); CHECK(state == GL_FRONT);
        glScissor(7,9,200,100); glColorMask(0,1,0,1); glClearColor(.25f,.5f,.75f,1);
        GLint read_fbo, draw_fbo; /* native calls, not guest wrappers */
        native_getint(0x8caa, &read_fbo); native_getint(0x8ca6, &draw_fbo);
        int scaled = client.right != 320 || client.bottom != 240;
        CHECK((read_fbo != 0) == scaled && read_fbo == draw_fbo);
        glXSwapBuffers(d,w);
        GLint restored; native_getint(0x8caa,&restored); CHECK(restored == read_fbo);
        native_getint(0x8ca6,&restored); CHECK(restored == draw_fbo);
        glGetIntegerv(GL_READ_BUFFER,&state); CHECK(state == GL_FRONT);
        glGetIntegerv(GL_DRAW_BUFFER,&state); CHECK(state == GL_FRONT);
        GLint scissor[4]; glGetIntegerv(GL_SCISSOR_BOX,scissor); CHECK(scissor[0] == 7 && scissor[1] == 9 && scissor[2] == 200 && scissor[3] == 100);
        CHECK(glIsEnabled(GL_SCISSOR_TEST));
        GLboolean mask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,mask); CHECK(!mask[0] && mask[1] && !mask[2] && mask[3]);
        GLfloat clear[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE,clear); CHECK(clear[0] == .25f && clear[1] == .5f && clear[2] == .75f);
        glReadBuffer(GL_BACK); glReadPixels(319,120,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel); CHECK(!pixel[0] && pixel[1] == 255 && !pixel[2]);
        bind(0x8d40,0); native_readbuffer(GL_BACK);
        native_readpixels(client.right/2,client.bottom/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        CHECK(pixel[0] == 255 && !pixel[1] && !pixel[2]);
        int content_width = client.right, content_height = client.bottom;
        if ((int64_t)client.right * 240 > (int64_t)client.bottom * 320) content_width = client.bottom * 320 / 240;
        else content_height = client.right * 240 / 320;
        if (content_width < client.right || content_height < client.bottom) {
            native_readpixels(content_width < client.right ? 0 : client.right/2,
                              content_height < client.bottom ? 0 : client.bottom/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            CHECK(!pixel[0] && !pixel[1] && !pixel[2]);
        }
        bind(0x8d40,(GLuint)read_fbo);
        CHECK(glGetError() == GL_NO_ERROR);
        char image[MAX_PATH]; CHECK(GetModuleFileNameA(NULL,image,sizeof(image)));
        char *slash = strrchr(image,'\\'); CHECK(slash);
        snprintf(slash + 1,sizeof(image) - (size_t)(slash + 1 - image),"snapshots/frame-%u.bmp",i+1);
        FILE *bmp = fopen(image,"rb"); CHECK(bmp); BITMAPFILEHEADER file_header; BITMAPINFOHEADER info;
        CHECK(fread(&file_header,sizeof(file_header),1,bmp) == 1 && fread(&info,sizeof(info),1,bmp) == 1);
        CHECK(!fclose(bmp)); CHECK(file_header.bfType == 0x4d42 && info.biWidth == 320 && info.biHeight == 240);
        printf("PASS %s output=%ldx%ld path=%s game-readback=green content=red bars=black state=restored\n", mode, client.right,client.bottom,scaled ? "FBO" : "direct");
    }
    if (!strcmp(mode,"fullscreen") || !strcmp(mode,"fullscreen-change")) {
        DEVMODEA active = {0}; active.dmSize = sizeof(active); CHECK(EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &active));
        SendMessageA(hwnd,WM_ACTIVATEAPP,FALSE,0);
        DEVMODEA paused = {0}; paused.dmSize = sizeof(paused); CHECK(EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &paused));
        CHECK(paused.dmPelsWidth == desktop.dmPelsWidth && paused.dmPelsHeight == desktop.dmPelsHeight);
        SendMessageA(hwnd,WM_ACTIVATEAPP,TRUE,0);
        DEVMODEA resumed = {0}; resumed.dmSize = sizeof(resumed); CHECK(EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &resumed));
        CHECK(resumed.dmPelsWidth == active.dmPelsWidth && resumed.dmPelsHeight == active.dmPelsHeight);
        printf("PASS %s deactivate/restore/reactivate desktop=%lux%lu active=%lux%lu%s\n",mode,desktop.dmPelsWidth,desktop.dmPelsHeight,active.dmPelsWidth,active.dmPelsHeight,
               active.dmPelsWidth == desktop.dmPelsWidth && active.dmPelsHeight == desktop.dmPelsHeight ? " (no physical mode change exercised)" : "");
    }
    CHECK(DestroyWindow(hwnd));
    DEVMODEA final = {0}; final.dmSize = sizeof(final); CHECK(EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &final));
    CHECK(final.dmPelsWidth == desktop.dmPelsWidth && final.dmPelsHeight == desktop.dmPelsHeight);
    CHECK(glXMakeCurrent(d,0,NULL)); CHECK(wglDeleteContext((HGLRC)rc)); FreeLibrary(dll);
    return 0;
}
static void run_child(const char *exe, const char *dir, const char *mode, const char *config)
{
    char path[MAX_PATH]; snprintf(path,sizeof(path),"%s\\phoenixtools.ini",dir);
    FILE *f = fopen(path,"wb"); CHECK(f); CHECK(fputs(config,f) >= 0);
    CHECK(fputs("[debug]\nno_present=On\nsnapshot_dir=snapshots\nsnapshot_every=1\n",f) >= 0);
    CHECK(!fclose(f));
    char command[MAX_PATH + 80]; snprintf(command,sizeof(command),"\"%s\" --child %s",exe,mode);
    STARTUPINFOA si = {0}; si.cb = sizeof(si); PROCESS_INFORMATION pi;
    CHECK(CreateProcessA(exe,command,NULL,NULL,TRUE,0,NULL,NULL,&si,&pi));
    CHECK(WaitForSingleObject(pi.hProcess,60000) == WAIT_OBJECT_0);
    DWORD status; CHECK(GetExitCodeProcess(pi.hProcess,&status)); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CHECK(!status);
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1],"--child")) return child(argv[2]);
    char source[MAX_PATH], temp[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], target[MAX_PATH];
    CHECK(GetModuleFileNameA(NULL,source,sizeof(source))); CHECK(GetTempPathA(sizeof(temp),temp)); CHECK(GetTempFileNameA(temp,"gfx",0,dir));
    CHECK(DeleteFileA(dir)); CHECK(CreateDirectoryA(dir,NULL));
    CHECK(strlen(dir) + 24 < MAX_PATH);
    snprintf(exe,sizeof(exe),"%s\\test_display.exe",dir); CHECK(CopyFileA(source,exe,FALSE));
    char *slash = strrchr(source,'\\'); CHECK(slash); *slash = 0;
    snprintf(dll,sizeof(dll),"%s\\win\\lxgfx.dll",source); snprintf(target,sizeof(target),"%s\\lxgfx.dll",dir); CHECK(CopyFileA(dll,target,FALSE));
    run_child(exe,dir,"windowed","[display]\nmode=windowed\nwidth=960\nheight=540\n");
    run_child(exe,dir,"borderless","[display]\nmode=borderless\nwidth=123\nheight=456\n");
    run_child(exe,dir,"fullscreen","[display]\nmode=fullscreen\n");
    DEVMODEA desktop = {0}, candidate = {0}; desktop.dmSize = sizeof(desktop); CHECK(EnumDisplaySettingsA(NULL,ENUM_CURRENT_SETTINGS,&desktop));
    candidate.dmSize = sizeof(candidate);
    int changed = 0;
    for (DWORD i = 0; EnumDisplaySettingsA(NULL,i,&candidate); ++i) {
        if (candidate.dmPelsWidth >= 640 && candidate.dmPelsHeight >= 480 &&
            (candidate.dmPelsWidth != desktop.dmPelsWidth || candidate.dmPelsHeight != desktop.dmPelsHeight)) {
            char config[160]; snprintf(config,sizeof(config),"[display]\nmode=fullscreen\nwidth=%lu\nheight=%lu\n",candidate.dmPelsWidth,candidate.dmPelsHeight);
            run_child(exe,dir,"fullscreen-change",config); changed = 1; break;
        }
        candidate.dmSize = sizeof(candidate);
    }
    if (!changed) puts("SKIP physical fullscreen change: Windows enumerated no alternate display mode");
    run_child(exe,dir,"fullscreen-failure","[display]\nmode=fullscreen\nwidth=1\nheight=1\n");
    run_child(exe,dir,"invalid","[display]\nmode=not-a-mode\n");
    run_child(exe,dir,"invalid-size","[display]\nmode=windowed\nwidth=-12\nheight=bad-size\n");
    char snapshot_dir[MAX_PATH], pattern[MAX_PATH]; snprintf(snapshot_dir,sizeof(snapshot_dir),"%s\\snapshots",dir); snprintf(pattern,sizeof(pattern),"%s\\*",snapshot_dir);
    WIN32_FIND_DATAA data; HANDLE search = FindFirstFileA(pattern,&data);
    if (search != INVALID_HANDLE_VALUE) {
        do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { char file[MAX_PATH]; snprintf(file,sizeof(file),"%s\\%s",snapshot_dir,data.cFileName); CHECK(DeleteFileA(file)); } } while (FindNextFileA(search,&data));
        FindClose(search);
    }
    CHECK(RemoveDirectoryA(snapshot_dir));
    char ini[MAX_PATH]; snprintf(ini,sizeof(ini),"%s\\phoenixtools.ini",dir); CHECK(DeleteFileA(ini)); CHECK(DeleteFileA(target)); CHECK(DeleteFileA(exe)); CHECK(RemoveDirectoryA(dir));
    puts("PASS display configuration, scaling, resize, readback, state, invalid values, fullscreen lifecycle");
    return 0;
}
