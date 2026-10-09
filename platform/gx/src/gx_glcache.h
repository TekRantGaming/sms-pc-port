// Shadow of the GL state the draw path sets, so a batch flush only sends the
// state that changed since the previous one (a plaza frame flushes ~800
// batches; re-sending every state each time cost ~50 GL calls per batch).
//
// Only flushBatch and the texture binds go through these setters. Every other
// path that touches GL state directly (EFB copies, clears, peeks, presenting,
// the overlay, shader compilation) calls glcInvalidate() first, so the next
// flush re-sends everything; glcInvalidate also unbinds the sampler objects
// from the units those paths sample, since a bound sampler overrides a
// texture's own parameters.
#ifndef SMS_GX_GLCACHE_H
#define SMS_GX_GLCACHE_H

#include "gl_funcs.h"
#include <stdint.h>
#include <string.h>

namespace gx {

// glcInvalidate() sets every field to a value no real setting has (~0 for
// names and enums, -1 for flags, NaN for floats), so the next setter re-sends.
struct GlCache {
    GLuint fbo, prog;
    GLint vp[4], sc[4];
    int scissor, cull, depth, blend, logic, clip0, clip1, depthClamp;
    GLenum frontFace, cullFace, depthFunc, logicOp;
    GLenum beq[2], bf[4];
    GLboolean depthMask, cmask[4];
    float blendColor[4], pointSize, lineWidth;
    int activeUnit;
    GLuint tex[8], sampler[8];
    GLuint vao, arrayBuffer, uniformBuffer;
};
extern GlCache g_glc;

void glcInvalidate();
void glcForgetTexture(GLuint tex);  // a texture name was deleted

inline void glcCap(GLenum cap, int& shadow, bool on) {
    if (shadow == int(on)) return;
    shadow = on;
    if (on) glEnable(cap);
    else glDisable(cap);
}
inline void glcActiveUnit(int unit) {
    if (g_glc.activeUnit == unit) return;
    g_glc.activeUnit = unit;
    glActiveTexture(GL_TEXTURE0 + unit);
}
// Binds `tex` on `unit` (making it the active unit when a bind is needed).
inline void glcBindTexture(int unit, GLuint tex) {
    if (g_glc.tex[unit] == tex) return;
    glcActiveUnit(unit);
    g_glc.tex[unit] = tex;
    glBindTexture(GL_TEXTURE_2D, tex);
}
// Records a bind made directly (texture creation and upload bind on the
// active unit).
inline void glcNoteBound(int unit, GLuint tex) { g_glc.tex[unit] = tex; }
inline void glcBindSampler(int unit, GLuint s) {
    if (g_glc.sampler[unit] == s) return;
    g_glc.sampler[unit] = s;
    glBindSampler(GLuint(unit), s);
}
inline void glcBindVertexArray(GLuint v) {
    if (g_glc.vao == v) return;
    g_glc.vao = v;
    glBindVertexArray(v);
}
inline void glcBindArrayBuffer(GLuint b) {
    if (g_glc.arrayBuffer == b) return;
    g_glc.arrayBuffer = b;
    glBindBuffer(GL_ARRAY_BUFFER, b);
}
inline void glcBindUniformBuffer(GLuint b) {
    if (g_glc.uniformBuffer == b) return;
    g_glc.uniformBuffer = b;
    glBindBuffer(GL_UNIFORM_BUFFER, b);
}
inline void glcUseProgram(GLuint p) {
    if (g_glc.prog == p) return;
    g_glc.prog = p;
    glUseProgram(p);
}

}  // namespace gx

#endif
