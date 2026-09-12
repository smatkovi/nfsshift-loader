// EGL imports. The real context is owned by SDL (display.cpp); the game only
// sees small integer handles and a synthetic list of configs.
#include <EGL/egl.h>

#include "display.h"
#include "hle.h"

namespace {
constexpr uint32_t DISPLAY = 1, SURFACE = 2, CONTEXT = 3;

struct FakeConfig {
    EGLint id, red, green, blue, alpha, depth, stencil, samples;
};
const FakeConfig kConfigs[] = {
    {1, 8, 8, 8, 8, 24, 8, 0},
    {2, 8, 8, 8, 0, 24, 8, 0},
    {3, 5, 6, 5, 0, 16, 0, 0},
    {4, 5, 6, 5, 0, 24, 8, 0},
};
const int kNumConfigs = sizeof(kConfigs) / sizeof(kConfigs[0]);

EGLint g_error = EGL_SUCCESS;
bool g_initialized = false;
bool g_current = false;
int g_client_version = 2;

uint32_t eglGetDisplay_(uint32_t native) { return DISPLAY; }

uint32_t eglInitialize_(uint32_t dpy, EGLint *major, EGLint *minor) {
    if (major) *major = 1;
    if (minor) *minor = 4;
    g_initialized = true;
    return EGL_TRUE;
}

uint32_t eglTerminate_(uint32_t dpy) {
    g_initialized = false;
    return EGL_TRUE;
}

uint32_t eglGetConfigs_(uint32_t dpy, uint32_t *configs, EGLint size, EGLint *num) {
    if (!configs) {
        if (num) *num = kNumConfigs;
        return EGL_TRUE;
    }
    int n = 0;
    for (; n < kNumConfigs && n < size; ++n) configs[n] = kConfigs[n].id;
    if (num) *num = n;
    return EGL_TRUE;
}

uint32_t eglGetConfigAttrib_(uint32_t dpy, uint32_t config, EGLint attr, EGLint *value) {
    if (config < 1 || config > static_cast<uint32_t>(kNumConfigs)) {
        g_error = EGL_BAD_CONFIG;
        return EGL_FALSE;
    }
    const FakeConfig &c = kConfigs[config - 1];
    EGLint v = 0;
    switch (attr) {
    case EGL_CONFIG_ID: v = c.id; break;
    case EGL_BUFFER_SIZE: v = c.red + c.green + c.blue + c.alpha; break;
    case EGL_RED_SIZE: v = c.red; break;
    case EGL_GREEN_SIZE: v = c.green; break;
    case EGL_BLUE_SIZE: v = c.blue; break;
    case EGL_ALPHA_SIZE: v = c.alpha; break;
    case EGL_DEPTH_SIZE: v = c.depth; break;
    case EGL_STENCIL_SIZE: v = c.stencil; break;
    case EGL_SAMPLES: v = c.samples; break;
    case EGL_SAMPLE_BUFFERS: v = c.samples ? 1 : 0; break;
    case EGL_SURFACE_TYPE: v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_RENDERABLE_TYPE: v = EGL_OPENGL_ES2_BIT; break;
    case EGL_CONFORMANT: v = EGL_OPENGL_ES2_BIT; break;
    case EGL_CONFIG_CAVEAT: v = EGL_NONE; break;
    case EGL_NATIVE_RENDERABLE: v = EGL_TRUE; break;
    case EGL_NATIVE_VISUAL_ID: v = 0; break;
    case EGL_LEVEL: v = 0; break;
    case EGL_TRANSPARENT_TYPE: v = EGL_NONE; break;
    case EGL_COLOR_BUFFER_TYPE: v = EGL_RGB_BUFFER; break;
    case EGL_MAX_PBUFFER_WIDTH: v = 4096; break;
    case EGL_MAX_PBUFFER_HEIGHT: v = 4096; break;
    case EGL_MAX_PBUFFER_PIXELS: v = 4096 * 4096; break;
    default: v = 0; break;
    }
    if (value) *value = v;
    return EGL_TRUE;
}

uint32_t eglCreateWindowSurface_(uint32_t dpy, uint32_t config, uint32_t win, const EGLint *attribs) {
    return SURFACE;
}

uint32_t eglDestroySurface_(uint32_t dpy, uint32_t surface) { return EGL_TRUE; }

uint32_t eglCreateContext_(uint32_t dpy, uint32_t config, uint32_t share, const EGLint *attribs) {
    g_client_version = 1;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION) g_client_version = a[1];
    logf("[egl] eglCreateContext config=%u client version %d", config, g_client_version);
    if (g_client_version < 2) logf("[egl] WARNING: game asked for a GLES%d context, only GLES2 is available", g_client_version);
    return CONTEXT;
}

uint32_t eglDestroyContext_(uint32_t dpy, uint32_t ctx) { return EGL_TRUE; }

uint32_t eglMakeCurrent_(uint32_t dpy, uint32_t draw, uint32_t read, uint32_t ctx) {
    g_current = ctx != 0;
    return EGL_TRUE;
}

uint32_t eglSwapBuffers_(uint32_t dpy, uint32_t surface) {
    display::present();
    return EGL_TRUE;
}

uint32_t eglQueryContext_(uint32_t dpy, uint32_t ctx, EGLint attr, EGLint *value) {
    if (!value) return EGL_FALSE;
    switch (attr) {
    case EGL_CONTEXT_CLIENT_VERSION: *value = 2; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLint eglGetError_() {
    EGLint e = g_error;
    g_error = EGL_SUCCESS;
    return e;
}

uint32_t eglBindAPI_(EGLenum api) { return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE; }
uint32_t eglGetCurrentContext_() { return g_current ? CONTEXT : 0; }
uint32_t eglGetCurrentDisplay_() { return g_current ? DISPLAY : 0; }
uint32_t eglGetCurrentSurface_(EGLint which) { return g_current ? SURFACE : 0; }

addr_t eglGetProcAddress_(const char *name) {
    if (!name || !hle::lookup(name)) return 0;
    return hle::stub_for(name);
}

struct Register {
    Register() {
        hle::reg("eglGetDisplay", HLE_WRAP(eglGetDisplay_));
        hle::reg("eglInitialize", HLE_WRAP(eglInitialize_));
        hle::reg("eglTerminate", HLE_WRAP(eglTerminate_));
        hle::reg("eglGetConfigs", HLE_WRAP(eglGetConfigs_));
        hle::reg("eglGetConfigAttrib", HLE_WRAP(eglGetConfigAttrib_));
        hle::reg("eglCreateWindowSurface", HLE_WRAP(eglCreateWindowSurface_));
        hle::reg("eglDestroySurface", HLE_WRAP(eglDestroySurface_));
        hle::reg("eglCreateContext", HLE_WRAP(eglCreateContext_));
        hle::reg("eglDestroyContext", HLE_WRAP(eglDestroyContext_));
        hle::reg("eglMakeCurrent", HLE_WRAP(eglMakeCurrent_));
        hle::reg("eglSwapBuffers", HLE_WRAP(eglSwapBuffers_));
        hle::reg("eglQueryContext", HLE_WRAP(eglQueryContext_));
        hle::reg("eglGetError", HLE_WRAP(eglGetError_));
        hle::reg("eglBindAPI", HLE_WRAP(eglBindAPI_));
        hle::reg("eglGetCurrentContext", HLE_WRAP(eglGetCurrentContext_));
        hle::reg("eglGetCurrentDisplay", HLE_WRAP(eglGetCurrentDisplay_));
        hle::reg("eglGetCurrentSurface", HLE_WRAP(eglGetCurrentSurface_));
        hle::reg("eglGetProcAddress", HLE_WRAP(eglGetProcAddress_));
    }
} g_register;
}  // namespace
