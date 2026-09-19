// OpenGL ES imports. Most are forwarded 1:1 (see gl_thunks.inc); the ones
// below need pointer translation or redirect the default framebuffer.
#include <GLES2/gl2.h>

#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "PVRTDecompress.h"
#include "runtime.h"
#include "display.h"
#include "hle.h"

namespace {

void gles1_unsupported(Cpu &cpu, const char *name) {
    static std::set<std::string> reported;
    if (reported.insert(name).second) logf("[gl] GLES1-only call %s ignored (lr=%#x)", name, cpu.reg(14));
    cpu.reg(0) = 0;
}

#include "gl_thunks.inc"

addr_t hle_glGetString(GLenum name) {
    const GLubyte *s = glGetString(name);
    if (!s) return 0;
    std::string str = reinterpret_cast<const char *>(s);
    if (name == GL_VERSION) str = "OpenGL ES 2.0 " + str;
    return guest::intern(str);
}

void hle_glShaderSource(GLuint shader, GLsizei count, addr_t strings, const GLint *length) {
    std::vector<const GLchar *> ptrs(count > 0 ? count : 0);
    for (GLsizei i = 0; i < count; ++i) ptrs[i] = gptr_t<const GLchar>(guest::read32(strings + 4 * i));
    if (const char *dir = getenv("NFS_DUMP_SHADERS")) {
        static int index = 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/shader%03d_%u.glsl", dir, index++, shader);
        if (FILE *f = fopen(path, "w")) {
            for (GLsizei i = 0; i < count; ++i) {
                if (length && length[i] >= 0) fwrite(ptrs[i], 1, length[i], f);
                else fputs(ptrs[i], f);
            }
            fclose(f);
        }
    }
    glShaderSource(shader, count, ptrs.data(), length);
}

void hle_glGetShaderSource(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source) {
    glGetShaderSource(shader, bufsize, length, source);
}

void hle_glShaderBinary(GLsizei n, const GLuint *shaders, GLenum format, const void *binary, GLsizei length) {
    glShaderBinary(n, shaders, format, binary, length);
}

// The game's GL layer ignores the names returned by glGenFramebuffers /
// glGenRenderbuffers and counts from 1 itself. That worked on the N9 (GLES2
// creates objects on first bind) but clashes with the loader's own objects
// and is invalid in the GLES 3.2 context Mali hands out. Translate names.
struct NameMap {
    std::map<GLuint, GLuint> to_real;
    std::map<GLuint, GLuint> to_game;
    GLuint real(GLuint game, bool create, void (*gen)(GLsizei, GLuint *)) {
        auto it = to_real.find(game);
        if (it != to_real.end()) return it->second;
        if (!create) return 0;
        GLuint r;
        gen(1, &r);
        to_real[game] = r;
        to_game[r] = game;
        return r;
    }
    GLuint game(GLuint real) {
        auto it = to_game.find(real);
        return it == to_game.end() ? real : it->second;
    }
    void erase(GLuint game) {
        auto it = to_real.find(game);
        if (it == to_real.end()) return;
        to_game.erase(it->second);
        to_real.erase(it);
    }
};
NameMap g_fbs, g_rbs;
GLuint g_next_fb_name = 0x10000, g_next_rb_name = 0x10000;

void gen_fb(GLsizei n, GLuint *ids) { glGenFramebuffers(n, ids); }
void gen_rb(GLsizei n, GLuint *ids) { glGenRenderbuffers(n, ids); }

GLuint map_fb(GLuint fb) { return fb == 0 ? display::game_framebuffer() : g_fbs.real(fb, true, gen_fb); }

void hle_glGenFramebuffers(GLsizei n, GLuint *ids) {
    for (GLsizei i = 0; i < n; ++i) ids[i] = g_next_fb_name++;
}

void hle_glBindFramebuffer(GLenum target, GLuint fb) { glBindFramebuffer(target, map_fb(fb)); }

void hle_glDeleteFramebuffers(GLsizei n, const GLuint *fbs) {
    for (GLsizei i = 0; i < n; ++i) {
        if (!fbs[i]) continue;
        GLuint r = g_fbs.real(fbs[i], false, gen_fb);
        if (r) glDeleteFramebuffers(1, &r);
        g_fbs.erase(fbs[i]);
    }
}

GLboolean hle_glIsFramebuffer(GLuint fb) {
    GLuint r = g_fbs.real(fb, false, gen_fb);
    return r ? glIsFramebuffer(r) : GL_FALSE;
}

void hle_glGenRenderbuffers(GLsizei n, GLuint *ids) {
    for (GLsizei i = 0; i < n; ++i) ids[i] = g_next_rb_name++;
}

void hle_glBindRenderbuffer(GLenum target, GLuint rb) { glBindRenderbuffer(target, rb ? g_rbs.real(rb, true, gen_rb) : 0); }

void hle_glDeleteRenderbuffers(GLsizei n, const GLuint *rbs) {
    for (GLsizei i = 0; i < n; ++i) {
        if (!rbs[i]) continue;
        GLuint r = g_rbs.real(rbs[i], false, gen_rb);
        if (r) glDeleteRenderbuffers(1, &r);
        g_rbs.erase(rbs[i]);
    }
}

GLboolean hle_glIsRenderbuffer(GLuint rb) {
    GLuint r = g_rbs.real(rb, false, gen_rb);
    return r ? glIsRenderbuffer(r) : GL_FALSE;
}

void hle_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb ? g_rbs.real(rb, true, gen_rb) : 0);
}

void hle_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
    glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
    if (pname == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
        GLint type;
        glGetFramebufferAttachmentParameteriv(target, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_RENDERBUFFER) params[0] = static_cast<GLint>(g_rbs.game(params[0]));
    }
}

void hle_glGetIntegerv(GLenum pname, GLint *params) {
    glGetIntegerv(pname, params);
    if (pname == GL_FRAMEBUFFER_BINDING) {
        GLuint r = static_cast<GLuint>(params[0]);
        params[0] = r == display::game_framebuffer() ? 0 : static_cast<GLint>(g_fbs.game(r));
    } else if (pname == GL_RENDERBUFFER_BINDING) {
        params[0] = static_cast<GLint>(g_rbs.game(static_cast<GLuint>(params[0])));
    }
}

void hle_glGetBooleanv(GLenum pname, GLboolean *params) { glGetBooleanv(pname, params); }
void hle_glGetFloatv(GLenum pname, GLfloat *params) { glGetFloatv(pname, params); }
void hle_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { glViewport(x, y, w, h); }
void hle_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { glScissor(x, y, w, h); }

// --- debugging (NFS_GL_DEBUG=1) -------------------------------------------------
std::map<std::string, int> g_gl_errors;
uint32_t g_draws, g_frames_dbg;

void gl_debug_hook(Cpu &cpu, uint32_t svc) {
    const char *name = hle::name_of(svc);
    if (name[0] != 'g' || name[1] != 'l') return;
    if (!strcmp(name, "glDrawElements") || !strcmp(name, "glDrawArrays")) ++g_draws;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR && g_gl_errors[name]++ < 5)
        logf("[gl] %s -> error %#x (lr=%#x)", name, err, cpu.reg(14));
}

// The N9 build ships PVRTC textures, which Mali GPUs cannot sample: decode
// them to RGBA8888 in software.
void hle_glCompressedTexImage2D(GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h, GLint border,
                                GLsizei size, const void *data) {
    static std::map<GLenum, int> seen;
    if (!seen[fmt]++) logf("[gl] compressed texture format %#x (%dx%d)", fmt, w, h);
    if (fmt >= 0x8c00 && fmt <= 0x8c03 && w > 0 && h > 0) {
        bool two_bit = fmt == 0x8c01 || fmt == 0x8c03;
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        pvr::PVRTDecompressPVRTC(data, two_bit ? 1 : 0, w, h, rgba.data());
        GLint align;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        return;
    }
    glCompressedTexImage2D(target, level, fmt, w, h, border, size, data);
}

// --- uniforms -----------------------------------------------------------------
// PowerVR accepted glUniform calls whose size did not match the declared type
// (e.g. glUniform4fv on a vec2); Mali rejects them. Remember the declared
// types at link time and convert.
struct UniformInfo {
    GLenum type;
};
std::map<GLuint, std::map<GLint, UniformInfo>> g_uniforms;

int float_components(GLenum type) {
    switch (type) {
    case GL_FLOAT: return 1;
    case GL_FLOAT_VEC2: return 2;
    case GL_FLOAT_VEC3: return 3;
    case GL_FLOAT_VEC4: return 4;
    default: return 0;
    }
}

int int_components(GLenum type) {
    switch (type) {
    case GL_INT: case GL_BOOL: case GL_SAMPLER_2D: case GL_SAMPLER_CUBE: return 1;
    case GL_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_INT_VEC4: case GL_BOOL_VEC4: return 4;
    default: return 0;
    }
}

void record_uniforms(GLuint program) {
    auto &m = g_uniforms[program];
    m.clear();
    GLint n = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &n);
    for (GLint i = 0; i < n; ++i) {
        char name[256];
        GLint size;
        GLenum type;
        glGetActiveUniform(program, i, sizeof(name), nullptr, &size, &type, name);
        std::string base = name;
        if (base.size() > 3 && base.compare(base.size() - 3, 3, "[0]") == 0) base.resize(base.size() - 3);
        GLint loc = glGetUniformLocation(program, name);
        if (loc < 0) continue;
        m[loc] = {type};
        for (GLint k = 1; k < size; ++k) {
            std::string elem = base + "[" + std::to_string(k) + "]";
            GLint l = glGetUniformLocation(program, elem.c_str());
            if (l >= 0) m[l] = {type};
        }
    }
}

GLenum uniform_type(GLint location) {
    GLint prog;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    auto p = g_uniforms.find(static_cast<GLuint>(prog));
    if (p == g_uniforms.end()) return 0;
    auto u = p->second.find(location);
    return u == p->second.end() ? 0 : u->second.type;
}

void set_float_uniform(GLint location, GLsizei count, int have, const GLfloat *v) {
    int want = float_components(uniform_type(location));
    if (location < 0 || want == 0 || want == have) {
        switch (have) {
        case 1: glUniform1fv(location, count, v); break;
        case 2: glUniform2fv(location, count, v); break;
        case 3: glUniform3fv(location, count, v); break;
        default: glUniform4fv(location, count, v); break;
        }
        return;
    }
    std::vector<GLfloat> conv(static_cast<size_t>(count) * want, 0.0f);
    for (GLsizei i = 0; i < count; ++i)
        for (int c = 0; c < std::min(have, want); ++c) conv[i * want + c] = v[i * have + c];
    switch (want) {
    case 1: glUniform1fv(location, count, conv.data()); break;
    case 2: glUniform2fv(location, count, conv.data()); break;
    case 3: glUniform3fv(location, count, conv.data()); break;
    default: glUniform4fv(location, count, conv.data()); break;
    }
}

void set_int_uniform(GLint location, GLsizei count, int have, const GLint *v) {
    GLenum type = uniform_type(location);
    int want = int_components(type);
    if (want == 0 && float_components(type)) {
        // Integer data for a float uniform.
        std::vector<GLfloat> f(static_cast<size_t>(count) * have);
        for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<GLfloat>(v[i]);
        set_float_uniform(location, count, have, f.data());
        return;
    }
    if (location < 0 || want == 0 || want == have) {
        switch (have) {
        case 1: glUniform1iv(location, count, v); break;
        case 2: glUniform2iv(location, count, v); break;
        case 3: glUniform3iv(location, count, v); break;
        default: glUniform4iv(location, count, v); break;
        }
        return;
    }
    std::vector<GLint> conv(static_cast<size_t>(count) * want, 0);
    for (GLsizei i = 0; i < count; ++i)
        for (int c = 0; c < std::min(have, want); ++c) conv[i * want + c] = v[i * have + c];
    switch (want) {
    case 1: glUniform1iv(location, count, conv.data()); break;
    case 2: glUniform2iv(location, count, conv.data()); break;
    case 3: glUniform3iv(location, count, conv.data()); break;
    default: glUniform4iv(location, count, conv.data()); break;
    }
}

void u1f(GLint l, GLfloat a) { set_float_uniform(l, 1, 1, &a); }
void u2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[] = {a, b}; set_float_uniform(l, 1, 2, v); }
void u3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[] = {a, b, c}; set_float_uniform(l, 1, 3, v); }
void u4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[] = {a, b, c, d}; set_float_uniform(l, 1, 4, v); }
void u1fv(GLint l, GLsizei n, const GLfloat *v) { set_float_uniform(l, n, 1, v); }
void u2fv(GLint l, GLsizei n, const GLfloat *v) { set_float_uniform(l, n, 2, v); }
void u3fv(GLint l, GLsizei n, const GLfloat *v) { set_float_uniform(l, n, 3, v); }
void u4fv(GLint l, GLsizei n, const GLfloat *v) { set_float_uniform(l, n, 4, v); }
void u1i(GLint l, GLint a) { set_int_uniform(l, 1, 1, &a); }
void u1iv(GLint l, GLsizei n, const GLint *v) { set_int_uniform(l, n, 1, v); }
void u2iv(GLint l, GLsizei n, const GLint *v) { set_int_uniform(l, n, 2, v); }
void u3iv(GLint l, GLsizei n, const GLint *v) { set_int_uniform(l, n, 3, v); }
void u4iv(GLint l, GLsizei n, const GLint *v) { set_int_uniform(l, n, 4, v); }

void hle_glCompileShader(GLuint shader) {
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        logf("[gl] shader %u compile failed: %s", shader, log);
    }
}

void hle_glLinkProgram(GLuint program) {
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) record_uniforms(program);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        logf("[gl] program %u link failed: %s", program, log);
    }
}

GLenum hle_glCheckFramebufferStatus(GLenum target) {
    GLenum s = glCheckFramebufferStatus(target);
    GLint fb;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    logf("[gl] glCheckFramebufferStatus(fb=%d) = %#x", fb, s);
    return s;
}

void hle_glRenderbufferStorage(GLenum target, GLenum fmt, GLsizei w, GLsizei h) {
    logf("[gl] glRenderbufferStorage(%#x, %dx%d)", fmt, w, h);
    glRenderbufferStorage(target, fmt, w, h);
}

void hle_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
    logf("[gl] glFramebufferTexture2D(att=%#x, tex=%u, level=%d)", attachment, tex, level);
    glFramebufferTexture2D(target, attachment, textarget, tex, level);
}

void hle_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                      GLenum type, const void *data) {
    if (!data) logf("[gl] glTexImage2D(null data, ifmt=%#x fmt=%#x type=%#x %dx%d level %d)", ifmt, fmt, type, w, h, level);
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
}

// --- vertex attribute binding ------------------------------------------------------
// The game binds a new GL_ARRAY_BUFFER per mesh but skips glVertexAttribPointer
// when the vertex format is unchanged. PowerVR drivers let the pointer follow
// the bound buffer; conformant drivers keep the old one. Re-latch at draw time.
struct AttribState {
    bool enabled = false;
    bool set = false;
    GLint size = 0;
    GLenum type = 0;
    GLboolean norm = 0;
    GLsizei stride = 0;
    uintptr_t ptr = 0;
    GLuint buffer = 0;
};
AttribState g_attribs[16];
GLuint g_array_buffer;
GLuint g_element_buffer;

void track_glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) g_array_buffer = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = b;
    glBindBuffer(target, b);
}

// Deleting a bound buffer unbinds it (GLES 2.0, section 2.9). The tracked
// bindings have to follow, or a later client pointer would be taken for an
// offset into a buffer that no longer exists.
void track_glDeleteBuffers(GLsizei n, const GLuint *ids) {
    for (GLsizei k = 0; ids && k < n; ++k) {
        if (ids[k] && ids[k] == g_array_buffer) g_array_buffer = 0;
        if (ids[k] && ids[k] == g_element_buffer) g_element_buffer = 0;
    }
    glDeleteBuffers(n, ids);
}

// With a buffer bound, the "pointer" of glVertexAttribPointer and the indices of
// glDrawElements are offsets into that buffer and must reach GL unchanged; only
// without one are they client pointers into guest memory. The generic argument
// conversion cannot know which, so both take the raw guest word.
static const void *gl_pointer_or_offset(addr_t word, GLuint bound) {
    if (bound) return reinterpret_cast<const void *>(static_cast<uintptr_t>(word));
    return word ? gptr(word) : nullptr;
}


void vertex_attrib_pointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr) {
    if (i < 16) {
        AttribState &a = g_attribs[i];
        a.set = true;
        a.size = size;
        a.type = type;
        a.norm = norm;
        a.stride = stride;
        a.ptr = reinterpret_cast<uintptr_t>(ptr);
        a.buffer = g_array_buffer;
    }
    glVertexAttribPointer(i, size, type, norm, stride, ptr);
}

void track_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, addr_t word) {
    vertex_attrib_pointer(i, size, type, norm, stride, gl_pointer_or_offset(word, g_array_buffer));
}

void track_glEnableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = true;
    glEnableVertexAttribArray(i);
}

void track_glDisableVertexAttribArray(GLuint i) {
    if (i < 16) g_attribs[i].enabled = false;
    glDisableVertexAttribArray(i);
}

void relatch_attribs() {
    if (!g_array_buffer) return;
    for (GLuint i = 0; i < 16; ++i) {
        AttribState &a = g_attribs[i];
        if (a.enabled && a.set && a.buffer && a.buffer != g_array_buffer) {
            a.buffer = g_array_buffer;
            glVertexAttribPointer(i, a.size, a.type, a.norm, a.stride, reinterpret_cast<const void *>(a.ptr));
        }
    }
}

void draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    relatch_attribs();
    glDrawElements(mode, count, type, indices);
}

void track_glDrawElements(GLenum mode, GLsizei count, GLenum type, addr_t word) {
    draw_elements(mode, count, type, gl_pointer_or_offset(word, g_element_buffer));
}

void track_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    relatch_attribs();
    glDrawArrays(mode, first, count);
}

// --- geometry debugging (NFS_GL_DEBUG=2) -------------------------------------
struct BufInfo {
    GLsizeiptr size = 0;
    std::vector<uint8_t> data;
};
std::map<GLuint, BufInfo> g_bufs;
GLuint g_bound_array, g_bound_element;

bool seq_logging() {
    static const uint64_t start = getenv("NFS_GL_SEQ_AT") ? strtoull(getenv("NFS_GL_SEQ_AT"), nullptr, 10) : ~0ull;
    static int n = 0;
    return runtime::now_ms() >= start && n++ < 400;
}

void dbg_glBindBuffer(GLenum target, GLuint b) {
    if (seq_logging()) logf("[seq] glBindBuffer(%s, %u)", target == GL_ARRAY_BUFFER ? "ARRAY" : "ELEMENT", b);
    if (target == GL_ARRAY_BUFFER) g_bound_array = b;
    else g_bound_element = b;
    track_glBindBuffer(target, b);
}

void dbg_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    BufInfo &bi = g_bufs[target == GL_ARRAY_BUFFER ? g_bound_array : g_bound_element];
    bi.size = size;
    bi.data.assign(static_cast<size_t>(size), 0);
    if (data) memcpy(bi.data.data(), data, size);
    glBufferData(target, size, data, usage);
}

void dbg_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    BufInfo &bi = g_bufs[target == GL_ARRAY_BUFFER ? g_bound_array : g_bound_element];
    static int logged = 0;
    if (logged++ < 10) logf("[gl] glBufferSubData(%s off=%ld size=%ld of %ld)", target == GL_ARRAY_BUFFER ? "vbo" : "ebo", (long)off, (long)size, (long)bi.size);
    if (off + size <= bi.size && data) memcpy(bi.data.data() + off, data, size);
    glBufferSubData(target, off, size, data);
}

struct AttribDbg {
    GLint size;
    GLenum type;
    GLsizei stride;
    uintptr_t ptr;
    GLuint buffer;
};
AttribDbg g_attr[8];

void hle_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, addr_t word) {
    const void *ptr = gl_pointer_or_offset(word, g_array_buffer);
    if (seq_logging()) logf("[seq] glVertexAttribPointer(%u, %d, %#x, stride %d, off %lu) with ARRAY=%u", index, size, type, stride, (unsigned long)word, g_bound_array);
    if (index < 8) g_attr[index] = {size, type, stride, reinterpret_cast<uintptr_t>(ptr), g_bound_array};
    vertex_attrib_pointer(index, size, type, norm, stride, ptr);
}

void hle_glDrawElements(GLenum mode, GLsizei count, GLenum type, addr_t word) {
    const void *indices = gl_pointer_or_offset(word, g_element_buffer);
    if (seq_logging()) logf("[seq] glDrawElements(count %d) ELEMENT=%u", count, g_bound_element);
    static int logged = 0;
    if (logged < 3000 && g_bound_element) {
        uint32_t maxi = 0, mini = 0xffffffff;
        const uint8_t *src = nullptr;
        size_t avail = 0;
        if (g_bound_element) {
            BufInfo &bi = g_bufs[g_bound_element];
            uintptr_t off = reinterpret_cast<uintptr_t>(indices);
            if (off < bi.data.size()) {
                src = bi.data.data() + off;
                avail = bi.data.size() - off;
            }
        } else {
            src = static_cast<const uint8_t *>(indices);
            avail = SIZE_MAX;
        }
        int isz = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1;
        for (GLsizei i = 0; src && i < count && size_t(i + 1) * isz <= avail; ++i) {
            uint32_t v = isz == 2 ? (src[i * 2] | src[i * 2 + 1] << 8) : isz == 1 ? src[i] : (src[i*4] | src[i*4+1] << 8 | src[i*4+2] << 16 | uint32_t(src[i*4+3]) << 24);
            maxi = std::max(maxi, v);
            mini = std::min(mini, v);
        }
        GLint prog;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        GLint ploc = glGetAttribLocation(prog, "vs_Position");
        if (ploc < 0) ploc = glGetAttribLocation(prog, "inVert");
        if (ploc < 0 || ploc >= 8) ploc = 0;
        const AttribDbg &p = g_attr[ploc];
        long vbo_size = p.buffer ? (long)g_bufs[p.buffer].size : -1;
        long verts = p.stride && vbo_size > 0 ? (vbo_size - (long)p.ptr) / p.stride : -1;
        logf("[gl] draw prog=%d posloc=%d mode=%#x count=%d ebo=%u(size %ld) idx[%u..%u] | pos vbo=%u(size %ld) off=%lu stride=%d -> %ld verts%s",
             prog, ploc, mode, count, g_bound_element, g_bound_element ? (long)g_bufs[g_bound_element].size : 0L, mini, maxi, p.buffer,
             vbo_size, (unsigned long)p.ptr, p.stride, verts, (verts >= 0 && maxi >= (unsigned long)verts) ? "  <-- OUT OF RANGE" : "");
        ++logged;
    }
    draw_elements(mode, count, type, indices);
}

struct Register {
    Register() {
        register_gl_thunks();
        hle::reg("glBindBuffer", HLE_WRAP(track_glBindBuffer));
        hle::reg("glDeleteBuffers", HLE_WRAP(track_glDeleteBuffers));
        hle::reg("glVertexAttribPointer", HLE_WRAP(track_glVertexAttribPointer));
        hle::reg("glEnableVertexAttribArray", HLE_WRAP(track_glEnableVertexAttribArray));
        hle::reg("glDisableVertexAttribArray", HLE_WRAP(track_glDisableVertexAttribArray));
        hle::reg("glDrawElements", HLE_WRAP(track_glDrawElements));
        hle::reg("glDrawArrays", HLE_WRAP(track_glDrawArrays));
        if (getenv("NFS_GL_GEOM")) {
            hle::reg("glVertexAttribPointer", HLE_WRAP(hle_glVertexAttribPointer));
            hle::reg("glDrawElements", HLE_WRAP(hle_glDrawElements));
            hle::reg("glBindBuffer", HLE_WRAP(dbg_glBindBuffer));
            hle::reg("glBufferData", HLE_WRAP(dbg_glBufferData));
            hle::reg("glBufferSubData", HLE_WRAP(dbg_glBufferSubData));
        }
        if (getenv("NFS_GL_DEBUG")) {
            hle::reg("glCheckFramebufferStatus", HLE_WRAP(hle_glCheckFramebufferStatus));
            hle::reg("glRenderbufferStorage", HLE_WRAP(hle_glRenderbufferStorage));
            hle::reg("glFramebufferTexture2D", HLE_WRAP(hle_glFramebufferTexture2D));
            hle::reg("glTexImage2D", HLE_WRAP(hle_glTexImage2D));
        }
        hle::reg("glCompressedTexImage2D", HLE_WRAP(hle_glCompressedTexImage2D));
        hle::reg("glUniform1f", HLE_WRAP(u1f));
        hle::reg("glUniform2f", HLE_WRAP(u2f));
        hle::reg("glUniform3f", HLE_WRAP(u3f));
        hle::reg("glUniform4f", HLE_WRAP(u4f));
        hle::reg("glUniform1fv", HLE_WRAP(u1fv));
        hle::reg("glUniform2fv", HLE_WRAP(u2fv));
        hle::reg("glUniform3fv", HLE_WRAP(u3fv));
        hle::reg("glUniform4fv", HLE_WRAP(u4fv));
        hle::reg("glUniform1i", HLE_WRAP(u1i));
        hle::reg("glUniform1iv", HLE_WRAP(u1iv));
        hle::reg("glUniform2iv", HLE_WRAP(u2iv));
        hle::reg("glUniform3iv", HLE_WRAP(u3iv));
        hle::reg("glUniform4iv", HLE_WRAP(u4iv));
        hle::reg("glCompileShader", HLE_WRAP(hle_glCompileShader));
        hle::reg("glLinkProgram", HLE_WRAP(hle_glLinkProgram));
        if (getenv("NFS_GL_DEBUG")) hle::post_call_hook = gl_debug_hook;
        hle::reg("glGetString", HLE_WRAP(hle_glGetString));
        hle::reg("glShaderSource", HLE_WRAP(hle_glShaderSource));
        hle::reg("glGetShaderSource", HLE_WRAP(hle_glGetShaderSource));
        hle::reg("glShaderBinary", HLE_WRAP(hle_glShaderBinary));
        hle::reg("glBindFramebuffer", HLE_WRAP(hle_glBindFramebuffer));
        hle::reg("glDeleteFramebuffers", HLE_WRAP(hle_glDeleteFramebuffers));
        hle::reg("glIsFramebuffer", HLE_WRAP(hle_glIsFramebuffer));
        hle::reg("glGetIntegerv", HLE_WRAP(hle_glGetIntegerv));
        hle::reg("glGenFramebuffers", HLE_WRAP(hle_glGenFramebuffers));
        hle::reg("glGenRenderbuffers", HLE_WRAP(hle_glGenRenderbuffers));
        hle::reg("glBindRenderbuffer", HLE_WRAP(hle_glBindRenderbuffer));
        hle::reg("glDeleteRenderbuffers", HLE_WRAP(hle_glDeleteRenderbuffers));
        hle::reg("glIsRenderbuffer", HLE_WRAP(hle_glIsRenderbuffer));
        hle::reg("glFramebufferRenderbuffer", HLE_WRAP(hle_glFramebufferRenderbuffer));
        hle::reg("glGetFramebufferAttachmentParameteriv", HLE_WRAP(hle_glGetFramebufferAttachmentParameteriv));
        hle::reg("glGetBooleanv", HLE_WRAP(hle_glGetBooleanv));
        hle::reg("glGetFloatv", HLE_WRAP(hle_glGetFloatv));
        hle::reg("glViewport", HLE_WRAP(hle_glViewport));
        hle::reg("glScissor", HLE_WRAP(hle_glScissor));
    }
} g_register;

}  // namespace
