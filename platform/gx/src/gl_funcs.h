// Minimal OpenGL 3.3 core loader: every entry point used by the backend is
// fetched through the host's get-proc function (SDL_GL_GetProcAddress,
// eglGetProcAddress, ...), so the library links against no GL library itself.
#ifndef SMS_GX_GL_FUNCS_H
#define SMS_GX_GL_FUNCS_H

#include <GL/glcorearb.h>

#define SMS_GX_GL_FUNCS(X)                                                         \
    X(PFNGLGETERRORPROC, glGetError) X(PFNGLGETSTRINGPROC, glGetString)             \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) X(PFNGLENABLEPROC, glEnable)             \
    X(PFNGLDISABLEPROC, glDisable) X(PFNGLVIEWPORTPROC, glViewport)                 \
    X(PFNGLSCISSORPROC, glScissor) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                      \
    X(PFNGLBLENDCOLORPROC, glBlendColor) X(PFNGLLOGICOPPROC, glLogicOp)             \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) X(PFNGLDEPTHMASKPROC, glDepthMask)           \
    X(PFNGLCOLORMASKPROC, glColorMask) X(PFNGLCULLFACEPROC, glCullFace)             \
    X(PFNGLFRONTFACEPROC, glFrontFace) X(PFNGLCLEARCOLORPROC, glClearColor)         \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) X(PFNGLCLEARPROC, glClear)                 \
    X(PFNGLDEPTHRANGEPROC, glDepthRange) X(PFNGLLINEWIDTHPROC, glLineWidth)         \
    X(PFNGLPOINTSIZEPROC, glPointSize) X(PFNGLFINISHPROC, glFinish)                 \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)       \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                    \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                  \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                  \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                          \
    X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)                        \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                  \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) X(PFNGLDRAWARRAYSPROC, glDrawArrays)   \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                \
    X(PFNGLDELETESHADERPROC, glDeleteShader) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                        \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                              \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                            \
    X(PFNGLBINDFRAGDATALOCATIONPROC, glBindFragDataLocation)                        \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                            \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex)                        \
    X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding)                          \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1IVPROC, glUniform1iv)         \
    X(PFNGLUNIFORM4IVPROC, glUniform4iv) X(PFNGLUNIFORM2IVPROC, glUniform2iv) X(PFNGLUNIFORM2FVPROC, glUniform2fv)       \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv) X(PFNGLUNIFORM1FVPROC, glUniform1fv)       \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLTEXPARAMETERFPROC, glTexParameterf) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                  \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                  \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                        \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                    \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                            \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                          \
    X(PFNGLGENQUERIESPROC, glGenQueries) X(PFNGLBEGINQUERYPROC, glBeginQuery)       \
    X(PFNGLENDQUERYPROC, glEndQuery) X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries) X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLFENCESYNCPROC, glFenceSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                          \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                  \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers) X(PFNGLBINDSAMPLERPROC, glBindSampler)   \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                              \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                              \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                    \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex)                    \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                  \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f)                                    \
    X(PFNGLVERTEXATTRIBI4UIPROC, glVertexAttribI4ui)                                \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                                    \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D)                        \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                            \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)    \
    X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FPROC, glUniform2f)

#define SMS_GX_DECLARE(type, name) extern type gx_##name;
extern "C++" {
namespace gx { namespace gl {
SMS_GX_GL_FUNCS(SMS_GX_DECLARE)
bool load(void* (*getProc)(const char*));
}}  // namespace gx::gl
}
#undef SMS_GX_DECLARE

#define SMS_GX_ALIAS(type, name) using gx::gl::gx_##name;
SMS_GX_GL_FUNCS(SMS_GX_ALIAS)
#undef SMS_GX_ALIAS

// call sites use the plain GL names
#define glGetError gx_glGetError
#define glRenderbufferStorageMultisample gx_glRenderbufferStorageMultisample
#define glUniform1f gx_glUniform1f
#define glUniform2f gx_glUniform2f
#define glGetString gx_glGetString
#define glGetIntegerv gx_glGetIntegerv
#define glEnable gx_glEnable
#define glDisable gx_glDisable
#define glViewport gx_glViewport
#define glScissor gx_glScissor
#define glBlendFuncSeparate gx_glBlendFuncSeparate
#define glBlendEquationSeparate gx_glBlendEquationSeparate
#define glBlendColor gx_glBlendColor
#define glLogicOp gx_glLogicOp
#define glDepthFunc gx_glDepthFunc
#define glDepthMask gx_glDepthMask
#define glColorMask gx_glColorMask
#define glCullFace gx_glCullFace
#define glFrontFace gx_glFrontFace
#define glClearColor gx_glClearColor
#define glClearDepth gx_glClearDepth
#define glClear gx_glClear
#define glDepthRange gx_glDepthRange
#define glLineWidth gx_glLineWidth
#define glPointSize gx_glPointSize
#define glFinish gx_glFinish
#define glGenBuffers gx_glGenBuffers
#define glBindBuffer gx_glBindBuffer
#define glBufferData gx_glBufferData
#define glBufferSubData gx_glBufferSubData
#define glBindBufferBase gx_glBindBufferBase
#define glGenVertexArrays gx_glGenVertexArrays
#define glBindVertexArray gx_glBindVertexArray
#define glVertexAttribPointer gx_glVertexAttribPointer
#define glVertexAttribIPointer gx_glVertexAttribIPointer
#define glEnableVertexAttribArray gx_glEnableVertexAttribArray
#define glDrawElements gx_glDrawElements
#define glDrawArrays gx_glDrawArrays
#define glCreateShader gx_glCreateShader
#define glShaderSource gx_glShaderSource
#define glCompileShader gx_glCompileShader
#define glGetShaderiv gx_glGetShaderiv
#define glGetShaderInfoLog gx_glGetShaderInfoLog
#define glDeleteShader gx_glDeleteShader
#define glCreateProgram gx_glCreateProgram
#define glAttachShader gx_glAttachShader
#define glLinkProgram gx_glLinkProgram
#define glGetProgramiv gx_glGetProgramiv
#define glGetProgramInfoLog gx_glGetProgramInfoLog
#define glUseProgram gx_glUseProgram
#define glDeleteProgram gx_glDeleteProgram
#define glBindAttribLocation gx_glBindAttribLocation
#define glBindFragDataLocation gx_glBindFragDataLocation
#define glGetUniformLocation gx_glGetUniformLocation
#define glGetUniformBlockIndex gx_glGetUniformBlockIndex
#define glUniformBlockBinding gx_glUniformBlockBinding
#define glUniform1i gx_glUniform1i
#define glUniform1iv gx_glUniform1iv
#define glUniform4iv gx_glUniform4iv
#define glUniform2iv gx_glUniform2iv
#define glUniform2fv gx_glUniform2fv
#define glUniform4fv gx_glUniform4fv
#define glUniform1fv gx_glUniform1fv
#define glGenTextures gx_glGenTextures
#define glDeleteTextures gx_glDeleteTextures
#define glBindTexture gx_glBindTexture
#define glActiveTexture gx_glActiveTexture
#define glTexImage2D gx_glTexImage2D
#define glTexSubImage2D gx_glTexSubImage2D
#define glTexParameteri gx_glTexParameteri
#define glTexParameterf gx_glTexParameterf
#define glGenFramebuffers gx_glGenFramebuffers
#define glBindFramebuffer gx_glBindFramebuffer
#define glFramebufferTexture2D gx_glFramebufferTexture2D
#define glCheckFramebufferStatus gx_glCheckFramebufferStatus
#define glDeleteFramebuffers gx_glDeleteFramebuffers
#define glBlitFramebuffer gx_glBlitFramebuffer
#define glReadPixels gx_glReadPixels
#define glPixelStorei gx_glPixelStorei
#define glGenQueries gx_glGenQueries
#define glBeginQuery gx_glBeginQuery
#define glEndQuery gx_glEndQuery
#define glGetQueryObjectuiv gx_glGetQueryObjectuiv
#define glDeleteQueries gx_glDeleteQueries
#define glDeleteBuffers gx_glDeleteBuffers
#define glMapBufferRange gx_glMapBufferRange
#define glUnmapBuffer gx_glUnmapBuffer
#define glFenceSync gx_glFenceSync
#define glClientWaitSync gx_glClientWaitSync
#define glDeleteSync gx_glDeleteSync
#define glGenRenderbuffers gx_glGenRenderbuffers
#define glBindRenderbuffer gx_glBindRenderbuffer
#define glRenderbufferStorage gx_glRenderbufferStorage
#define glFramebufferRenderbuffer gx_glFramebufferRenderbuffer
#define glGenSamplers gx_glGenSamplers
#define glBindSampler gx_glBindSampler
#define glSamplerParameteri gx_glSamplerParameteri
#define glSamplerParameterf gx_glSamplerParameterf
#define glDeleteSamplers gx_glDeleteSamplers
#define glDrawElementsBaseVertex gx_glDrawElementsBaseVertex
#define glBindBufferRange gx_glBindBufferRange
#define glVertexAttrib4f gx_glVertexAttrib4f
#define glVertexAttribI4ui gx_glVertexAttribI4ui
#define glDisableVertexAttribArray gx_glDisableVertexAttribArray
#define glGenerateMipmap gx_glGenerateMipmap
#define glCompressedTexImage2D gx_glCompressedTexImage2D
#define glGetStringi gx_glGetStringi

#endif
