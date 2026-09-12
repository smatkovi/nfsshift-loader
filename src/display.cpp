#include "display.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include <GLES2/gl2.h>
#include <SDL.h>

#include "guest.h"

#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

namespace display {
namespace {
SDL_Window *g_window;
SDL_GLContext g_context;
int g_win_w, g_win_h;
int g_w, g_h;
bool g_rotate, g_flip;
GLuint g_fbo, g_color, g_depth;
GLuint g_program, g_vbo;
GLint g_u_tex;
uint32_t g_frames;
uint32_t g_fps_start;
uint64_t g_swap_ticks;

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fatal("blit shader: %s", log);
    }
    return s;
}

void create_blit() {
    static const char *vs = "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
                            "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
    static const char *fs = "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv;\n"
                            "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
    g_program = glCreateProgram();
    glAttachShader(g_program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_program, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_program, 0, "a_pos");
    glBindAttribLocation(g_program, 1, "a_uv");
    glLinkProgram(g_program);
    g_u_tex = glGetUniformLocation(g_program, "u_tex");

    // Triangle strip over the whole window; uv chosen per rotation.
    float px[4] = {-1, 1, -1, 1}, py[4] = {-1, -1, 1, 1};
    float quad[16];
    for (int i = 0; i < 4; ++i) {
        float u, v;
        if (!g_rotate) {
            u = (px[i] + 1) / 2;
            v = (py[i] + 1) / 2;
        } else if (!g_flip) {
            u = (1 - py[i]) / 2;
            v = (px[i] + 1) / 2;
        } else {
            u = (py[i] + 1) / 2;
            v = (1 - px[i]) / 2;
        }
        quad[i * 4] = px[i];
        quad[i * 4 + 1] = py[i];
        quad[i * 4 + 2] = u;
        quad[i * 4 + 3] = v;
    }
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void create_fbo() {
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, g_w, g_h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &g_fbo);
    if (const char *pad = getenv("NFS_FBO_PAD")) {
        std::vector<GLuint> dummy(atoi(pad));
        glGenFramebuffers(static_cast<GLsizei>(dummy.size()), dummy.data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) fatal("game framebuffer incomplete: %#x", status);
    glViewport(0, 0, g_w, g_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
}  // namespace

bool init(const Options &opt) {
    SDL_SetHint(SDL_HINT_QTWAYLAND_CONTENT_ORIENTATION, opt.flip ? "inverted-landscape" : "landscape");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        logf("[display] SDL_Init: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_DisplayMode mode;
    int mode_w = 0, mode_h = 0;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        mode_w = mode.w;
        mode_h = mode.h;
    }
    logf("[display] desktop mode %dx%d", mode_w, mode_h);
    // Sailfish windows are always native portrait; the reported mode follows
    // the current device orientation, so normalise it.
    if (opt.rotate && mode_w > mode_h) std::swap(mode_w, mode_h);
    g_window = SDL_CreateWindow("NFS Shift", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, mode_w, mode_h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (!g_window) {
        logf("[display] SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) {
        logf("[display] SDL_GL_CreateContext: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(getenv("NFS_VSYNC") ? atoi(getenv("NFS_VSYNC")) : 1);
    SDL_GL_GetDrawableSize(g_window, &g_win_w, &g_win_h);
    g_rotate = opt.rotate && g_win_h > g_win_w;
    g_flip = opt.flip;

    int native_w = std::max(g_win_w, g_win_h), native_h = std::min(g_win_w, g_win_h);
    if (!opt.rotate) native_w = g_win_w, native_h = g_win_h;
    g_h = opt.surface_height > 0 ? opt.surface_height : native_h;
    g_w = (native_w * g_h / native_h) & ~1;

    logf("[display] window %dx%d, GL %s / %s, surface %dx%d%s", g_win_w, g_win_h, glGetString(GL_VERSION),
         glGetString(GL_RENDERER), g_w, g_h, g_rotate ? " (rotated)" : "");
    create_blit();
    create_fbo();
    g_fps_start = SDL_GetTicks();
    return true;
}

void shutdown() {
    if (g_context) SDL_GL_DeleteContext(g_context);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

int width() { return g_w; }
int height() { return g_h; }
unsigned game_framebuffer() { return g_fbo; }
bool rotated() { return g_rotate; }
bool flipped() { return g_flip; }

// Debug aid: with NFS_SHOT_DIR set, dump the game framebuffer every few seconds.
void maybe_screenshot() {
    static const char *dir = getenv("NFS_SHOT_DIR");
    static uint32_t last = 0;
    static int index = 0;
    if (!dir) return;
    uint32_t now = SDL_GetTicks();
    static const uint32_t interval = getenv("NFS_SHOT_MS") ? atoi(getenv("NFS_SHOT_MS")) : 4000;
    if (now - last < interval) return;
    last = now;
    std::vector<uint8_t> px(static_cast<size_t>(g_w) * g_h * 4);
    glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    char path[512];
    snprintf(path, sizeof(path), "%s/shot%03d.ppm", dir, index++);
    if (FILE *f = fopen(path, "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
        for (int y = g_h - 1; y >= 0; --y)
            for (int x = 0; x < g_w; ++x) fwrite(&px[(static_cast<size_t>(y) * g_w + x) * 4], 1, 3, f);
        fclose(f);
    }
}

void present() {
    maybe_screenshot();
    struct AttribState {
        GLint enabled, size, type, normalized, stride, buffer;
        void *pointer;
    } attribs[2];
    for (GLuint i = 0; i < 2; ++i) {
        AttribState &a = attribs[i];
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
    }
    GLint fb, program, array_buffer, active_texture, texture, viewport[4];
    GLboolean color_mask[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    static const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST, GL_DITHER};
    GLboolean cap_state[6];
    for (int i = 0; i < 6; ++i) {
        cap_state[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    SDL_GL_GetDrawableSize(g_window, &g_win_w, &g_win_h);
    glViewport(0, 0, g_win_w, g_win_h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_program);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glUniform1i(g_u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void *>(8));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint64_t t0 = SDL_GetPerformanceCounter();
    SDL_GL_SwapWindow(g_window);
    g_swap_ticks += SDL_GetPerformanceCounter() - t0;

    for (GLuint i = 0; i < 2; ++i) {
        AttribState &a = attribs[i];
        glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
        glVertexAttribPointer(i, a.size, a.type, a.normalized, a.stride, a.pointer);
        if (a.enabled) glEnableVertexAttribArray(i);
        else glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active_texture);
    glUseProgram(program);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    for (int i = 0; i < 6; ++i)
        if (cap_state[i]) glEnable(caps[i]);

    ++g_frames;
    uint32_t now = SDL_GetTicks();
    if (now - g_fps_start >= 5000) {
        logf("[display] %.1f fps, swap %.1f ms/frame", g_frames * 1000.0 / (now - g_fps_start),
             g_swap_ticks * 1000.0 / SDL_GetPerformanceFrequency() / std::max<uint32_t>(g_frames, 1));
        g_swap_ticks = 0;
        g_frames = 0;
        g_fps_start = now;
    }
}

void window_to_surface(float nx, float ny, int &sx, int &sy) {
    float u, v;  // u: surface x (0..1, right), v: surface y (0..1, down)
    if (!g_rotate) {
        u = nx;
        v = ny;
    } else if (!g_flip) {
        u = ny;
        v = 1 - nx;
    } else {
        u = 1 - ny;
        v = nx;
    }
    sx = static_cast<int>(u * g_w);
    sy = static_cast<int>(v * g_h);
}

void pump(const EventSink &sink) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) sink(ev);
}
}  // namespace display
