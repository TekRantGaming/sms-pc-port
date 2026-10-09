#include "gl_funcs.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "gx_internal.h"
#include "gx_glthread.h"
#include <type_traits>

namespace gx { namespace gl {
#define SMS_GX_DEFINE(type, name) type gx_##name = nullptr;
SMS_GX_GL_FUNCS(SMS_GX_DEFINE)
SMS_GX_GL_OPTIONAL_FUNCS(SMS_GX_DEFINE)
#undef SMS_GX_DEFINE

// SMS_GX_STATS counts the GL calls the renderer makes: each entry point is
// wrapped in a trampoline that bumps g_statGlCalls (not on 32-bit Windows,
// whose GL entry points are __stdcall).
uint64_t g_statGlCalls = 0;
#if !(defined(_WIN32) && !defined(_WIN64))
template <typename F> struct Counted;
template <typename R, typename... A> struct Counted<R (*)(A...)> {
    template <R (**Real)(A...), uint64_t* N> static R call(A... a) {
        g_statGlCalls++;
        (*N)++;
        return (*Real)(a...);
    }
};
struct CallCount {
    const char* name;
    uint64_t* n;
};
static CallCount s_counts[256];
static int s_nCounts = 0;
#endif

// Logs the entry points called most since the previous call (SMS_GX_STATS).
void logTopCalls(uint32_t frames) {
#if !(defined(_WIN32) && !defined(_WIN64))
    static uint64_t last[256];
    int order[256];
    for (int i = 0; i < s_nCounts; i++) order[i] = i;
    auto delta = [&](int i) { return *s_counts[i].n - last[i]; };
    for (int i = 1; i < s_nCounts; i++)  // insertion sort, descending
        for (int j = i; j > 0 && delta(order[j]) > delta(order[j - 1]); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    char line[1024];
    int len = 0;
    for (int k = 0; k < 12 && k < s_nCounts && delta(order[k]); k++)
        len += snprintf(line + len, sizeof line - size_t(len), "%s%s %.0f", k ? ", " : "", s_counts[order[k]].name,
                        double(delta(order[k])) / frames);
    if (len) gx::logmsg("stats: GL calls/frame by entry point: %s", line);
    for (int i = 0; i < s_nCounts; i++) last[i] = *s_counts[i].n;
#else
    (void)frames;
#endif
}

bool load(void* (*getProc)(const char*)) {
    bool ok = true;
#define SMS_GX_LOAD(type, name)                                   \
    gx_##name = reinterpret_cast<type>(getProc(#name));           \
    if (!gx_##name) { gx::logmsg("GL entry point missing: %s", #name); ok = false; }
    SMS_GX_GL_FUNCS(SMS_GX_LOAD)
#undef SMS_GX_LOAD
#define SMS_GX_LOAD_OPTIONAL(type, name) gx_##name = reinterpret_cast<type>(getProc(#name));
    SMS_GX_GL_OPTIONAL_FUNCS(SMS_GX_LOAD_OPTIONAL)
#undef SMS_GX_LOAD_OPTIONAL
#if !(defined(_WIN32) && !defined(_WIN64))
    if (getenv("SMS_GX_STATS")) {
#define SMS_GX_WRAP(type, name)                                   \
        {                                                         \
            static type real_##name;                              \
            static uint64_t n_##name;                             \
            real_##name = gx_##name;                              \
            gx_##name = &Counted<type>::template call<&real_##name, &n_##name>; \
            if (s_nCounts < 256) s_counts[s_nCounts++] = {#name, &n_##name}; \
        }
        SMS_GX_GL_FUNCS(SMS_GX_WRAP)
#undef SMS_GX_WRAP
    }
#endif
    return ok;
}
}}  // namespace gx::gl

// ---------------------------------------------------------------- GL thread proxies
// While the GL thread runs, every entry point above is replaced by a proxy
// (gx_glthread.h): calls without results or pointers are queued; calls with
// results, or with pointers the GL reads later or writes, wait for the GL
// thread, except those below that copy what their pointers point at (or whose
// pointers are offsets into bound buffers) and so are queued as well.
#if defined(_WIN32) && !defined(_WIN64)
// 32-bit Windows' GL entry points are __stdcall, which the proxies do not
// follow; the GL thread stays off there (gx_glthread.cpp).
void gx::glt::installProxies() {}
#else
namespace gx { namespace gl {
namespace {
// what the proxies need to know of state set through them (game thread)
GLint s_unpackAlign = 4, s_unpackRow = 0;
GLuint s_packBuffer = 0;

template <typename F> struct Px;
template <typename R, typename... A> struct Px<R (*)(A...)> {
    template <R (**Real)(A...)> static R call(A... a) {
        if (glt::onGlThread()) return (*Real)(a...);
        if constexpr (std::is_void<R>::value && !(std::is_pointer<A>::value || ...)) {
            R (*fn)(A...) = *Real;
            glt::post([=] { fn(a...); });
        } else if constexpr (std::is_void<R>::value) {
            glt::sync([&] { (*Real)(a...); });
        } else {
            R r{};
            glt::sync([&] { r = (*Real)(a...); });
            return r;
        }
    }
    // pointer arguments that are values here (buffer offsets, sync objects)
    template <R (**Real)(A...)> static R callValues(A... a) {
        static_assert(std::is_void<R>::value, "only calls without results are queued");
        if (glt::onGlThread()) return (*Real)(a...);
        R (*fn)(A...) = *Real;
        glt::post([=] { fn(a...); });
    }
};

#define SMS_GX_REAL(type, name) type real_##name = nullptr;
SMS_GX_GL_FUNCS(SMS_GX_REAL)
SMS_GX_GL_OPTIONAL_FUNCS(SMS_GX_REAL)
#undef SMS_GX_REAL

template <typename T, int N, void (**Real)(GLint, GLsizei, const T*)>
void uniformV(GLint loc, GLsizei count, const T* v) {
    if (glt::onGlThread()) return (*Real)(loc, count, v);
    auto fn = *Real;
    glt::postData(v, size_t(count) * N * sizeof(T), [=](const uint8_t* d) { fn(loc, count, reinterpret_cast<const T*>(d)); });
}

template <void (**Real)(GLsizei, const GLuint*)> void deleteNames(GLsizei n, const GLuint* names) {
    if (glt::onGlThread()) return (*Real)(n, names);
    auto fn = *Real;
    glt::postData(names, size_t(n) * sizeof(GLuint),
                  [=](const uint8_t* d) { fn(n, reinterpret_cast<const GLuint*>(d)); });
}

void pxBindBuffer(GLenum target, GLuint buffer) {
    if (glt::onGlThread()) return real_glBindBuffer(target, buffer);
    if (target == GL_PIXEL_PACK_BUFFER) s_packBuffer = buffer;
    auto fn = real_glBindBuffer;
    glt::post([=] { fn(target, buffer); });
}

void pxPixelStorei(GLenum pname, GLint param) {
    // kept on either thread: GL-thread code sets these only while the game's
    // thread waits for it (presenting)
    if (pname == GL_UNPACK_ALIGNMENT) s_unpackAlign = param;
    if (pname == GL_UNPACK_ROW_LENGTH) s_unpackRow = param;
    if (glt::onGlThread()) return real_glPixelStorei(pname, param);
    auto fn = real_glPixelStorei;
    glt::post([=] { fn(pname, param); });
}

void pxBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (glt::onGlThread()) return real_glBufferData(target, size, data, usage);
    auto fn = real_glBufferData;
    if (!data) return glt::post([=] { fn(target, size, nullptr, usage); });
    glt::postData(data, size_t(size), [=](const uint8_t* d) { fn(target, size, d, usage); });
}

void pxBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    if (glt::onGlThread()) return real_glBufferSubData(target, offset, size, data);
    auto fn = real_glBufferSubData;
    glt::postData(data, size_t(size), [=](const uint8_t* d) { fn(target, offset, size, d); });
}

// bytes a client-memory upload of w x h pixels reads (0: a layout not handled)
size_t uploadBytes(GLsizei w, GLsizei h, GLenum format, GLenum type) {
    size_t comps = format == GL_RED ? 1 : format == GL_RG ? 2 : format == GL_RGB ? 3
                 : format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_DEPTH_COMPONENT ? 1 : 0;
    size_t size = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT || type == GL_HALF_FLOAT ? 2
                : type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 : 0;
    if (!comps || !size || w <= 0 || h <= 0) return 0;
    size_t px = comps * size, align = size_t(s_unpackAlign > 0 ? s_unpackAlign : 1);
    size_t row = (size_t(s_unpackRow > 0 ? s_unpackRow : w) * px + align - 1) / align * align;
    return row * size_t(h - 1) + size_t(w) * px;
}

void pxTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                  GLenum type, const void* pixels) {
    if (glt::onGlThread()) return real_glTexImage2D(target, level, ifmt, w, h, border, format, type, pixels);
    auto fn = real_glTexImage2D;
    if (!pixels) return glt::post([=] { fn(target, level, ifmt, w, h, border, format, type, nullptr); });
    size_t n = uploadBytes(w, h, format, type);
    if (!n) return glt::sync([&] { fn(target, level, ifmt, w, h, border, format, type, pixels); });
    glt::postData(pixels, n, [=](const uint8_t* d) { fn(target, level, ifmt, w, h, border, format, type, d); });
}

void pxTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const void* pixels) {
    if (glt::onGlThread()) return real_glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
    auto fn = real_glTexSubImage2D;
    size_t n = pixels ? uploadBytes(w, h, format, type) : 0;
    if (!n) return glt::sync([&] { fn(target, level, x, y, w, h, format, type, pixels); });
    glt::postData(pixels, n, [=](const uint8_t* d) { fn(target, level, x, y, w, h, format, type, d); });
}

void pxCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                            GLsizei bytes, const void* data) {
    if (glt::onGlThread()) return real_glCompressedTexImage2D(target, level, ifmt, w, h, border, bytes, data);
    auto fn = real_glCompressedTexImage2D;
    glt::postData(data, size_t(bytes), [=](const uint8_t* d) { fn(target, level, ifmt, w, h, border, bytes, d); });
}

void pxReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    if (glt::onGlThread()) return real_glReadPixels(x, y, w, h, format, type, pixels);
    auto fn = real_glReadPixels;
    if (s_packBuffer) return glt::post([=] { fn(x, y, w, h, format, type, pixels); });  // into a buffer: an offset
    glt::sync([&] { fn(x, y, w, h, format, type, pixels); });
}

// Sync objects: glFenceSync returns at once a handle whose fence the GL thread
// makes when it gets there; the calls taking a sync object use that fence.
// Every sync object made through the proxies is such a handle (fences are
// only made after they are installed).
struct VirtualSync {
    GLsync real;  // set and read on the GL thread
};
GLsync realSync(GLsync s) { return s ? reinterpret_cast<VirtualSync*>(s)->real : nullptr; }

GLsync pxFenceSync(GLenum condition, GLbitfield flags) {
    VirtualSync* v = new VirtualSync{nullptr};
    auto fn = real_glFenceSync;
    glt::post([=] { v->real = fn(condition, flags); });
    return reinterpret_cast<GLsync>(v);
}

GLenum pxClientWaitSync(GLsync s, GLbitfield flags, GLuint64 timeout) {
    if (glt::onGlThread()) return real_glClientWaitSync(realSync(s), flags, timeout);
    GLenum r = GL_WAIT_FAILED;
    glt::sync([&] { r = real_glClientWaitSync(realSync(s), flags, timeout); });
    return r;
}

void pxDeleteSync(GLsync s) {
    if (!s) return;
    VirtualSync* v = reinterpret_cast<VirtualSync*>(s);
    auto fn = real_glDeleteSync;
    glt::post([=] {
        if (v->real) fn(v->real);
        delete v;
    });
}

GLboolean pxUnmapBuffer(GLenum target) {  // no caller looks at the result
    if (glt::onGlThread()) return real_glUnmapBuffer(target);
    auto fn = real_glUnmapBuffer;
    glt::post([=] { fn(target); });
    return GL_TRUE;
}

void pxMultiDrawElementsBaseVertex(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                   GLsizei drawcount, const GLint* basevertex) {
    if (glt::onGlThread()) return real_glMultiDrawElementsBaseVertex(mode, count, type, indices, drawcount, basevertex);
    const size_t n = size_t(drawcount);
    static std::vector<uint8_t> buf;  // indices, counts, base vertices, one after another
    buf.resize(n * (sizeof(GLsizei) + sizeof(void*) + sizeof(GLint)));
    memcpy(buf.data(), indices, n * sizeof(void*));
    memcpy(buf.data() + n * sizeof(void*), count, n * sizeof(GLsizei));
    memcpy(buf.data() + n * (sizeof(void*) + sizeof(GLsizei)), basevertex, n * sizeof(GLint));
    auto fn = real_glMultiDrawElementsBaseVertex;
    glt::postData(buf.data(), buf.size(), [=](const uint8_t* d) {
        const void* const* ind = reinterpret_cast<const void* const*>(d);
        const GLsizei* cnt = reinterpret_cast<const GLsizei*>(d + n * sizeof(void*));
        const GLint* bv = reinterpret_cast<const GLint*>(d + n * (sizeof(void*) + sizeof(GLsizei)));
        fn(mode, cnt, type, ind, GLsizei(n), bv);
    });
}
}  // namespace
}}  // namespace gx::gl

void gx::glt::installProxies() {
    using namespace gx::gl;
#define SMS_GX_PROXY(type, name)                                  \
    real_##name = gx_##name;                                      \
    gx_##name = &Px<type>::template call<&real_##name>;
    SMS_GX_GL_FUNCS(SMS_GX_PROXY)
#undef SMS_GX_PROXY
    gx_glVertexAttribPointer = &Px<PFNGLVERTEXATTRIBPOINTERPROC>::callValues<&real_glVertexAttribPointer>;
    gx_glVertexAttribIPointer = &Px<PFNGLVERTEXATTRIBIPOINTERPROC>::callValues<&real_glVertexAttribIPointer>;
    gx_glDrawElements = &Px<PFNGLDRAWELEMENTSPROC>::callValues<&real_glDrawElements>;
    gx_glDrawElementsBaseVertex = &Px<PFNGLDRAWELEMENTSBASEVERTEXPROC>::callValues<&real_glDrawElementsBaseVertex>;
    gx_glFenceSync = &pxFenceSync;
    gx_glClientWaitSync = &pxClientWaitSync;
    gx_glDeleteSync = &pxDeleteSync;
    gx_glUnmapBuffer = &pxUnmapBuffer;
    gx_glUniform1iv = &uniformV<GLint, 1, &real_glUniform1iv>;
    gx_glUniform2iv = &uniformV<GLint, 2, &real_glUniform2iv>;
    gx_glUniform4iv = &uniformV<GLint, 4, &real_glUniform4iv>;
    gx_glUniform1fv = &uniformV<GLfloat, 1, &real_glUniform1fv>;
    gx_glUniform2fv = &uniformV<GLfloat, 2, &real_glUniform2fv>;
    gx_glUniform4fv = &uniformV<GLfloat, 4, &real_glUniform4fv>;
    gx_glDeleteTextures = &deleteNames<&real_glDeleteTextures>;
    gx_glDeleteBuffers = &deleteNames<&real_glDeleteBuffers>;
    gx_glDeleteFramebuffers = &deleteNames<&real_glDeleteFramebuffers>;
    gx_glDeleteQueries = &deleteNames<&real_glDeleteQueries>;
    gx_glDeleteSamplers = &deleteNames<&real_glDeleteSamplers>;
    gx_glDeleteVertexArrays = &deleteNames<&real_glDeleteVertexArrays>;
    gx_glBindBuffer = &pxBindBuffer;
    gx_glPixelStorei = &pxPixelStorei;
    gx_glBufferData = &pxBufferData;
    gx_glBufferSubData = &pxBufferSubData;
    gx_glTexImage2D = &pxTexImage2D;
    gx_glTexSubImage2D = &pxTexSubImage2D;
    gx_glCompressedTexImage2D = &pxCompressedTexImage2D;
    gx_glReadPixels = &pxReadPixels;
    gx_glMultiDrawElementsBaseVertex = &pxMultiDrawElementsBaseVertex;
#define SMS_GX_PROXY_OPTIONAL(type, name)                         \
    if (gx_##name) {                                              \
        real_##name = gx_##name;                                  \
        gx_##name = &Px<type>::template call<&real_##name>;       \
    }
    SMS_GX_GL_OPTIONAL_FUNCS(SMS_GX_PROXY_OPTIONAL)
#undef SMS_GX_PROXY_OPTIONAL
}
#endif
