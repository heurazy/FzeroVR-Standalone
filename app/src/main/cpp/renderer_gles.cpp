#include "renderer_gles.h"
#include "gdiffuser_bridge.h"

#include <android/log.h>
#include <cmath>
#include <vector>

namespace {
constexpr const char* kTag = "FZeroXVR/Renderer";

VrMat4 Mul(const VrMat4& a, const VrMat4& b) {
    VrMat4 out{};
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out.m[c * 4 + r] =
                a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                a.m[3 * 4 + r] * b.m[c * 4 + 3];
        }
    }
    return out;
}

GLuint Compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}

VrMat4 RendererGLES::MakeProjection(const XrFovf& fov, float nearZ, float farZ) {
    const float tanLeft = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown = std::tan(fov.angleDown);
    const float tanUp = std::tan(fov.angleUp);
    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    VrMat4 m{};
    m.m[0] = 2.f / tanWidth;
    m.m[5] = 2.f / tanHeight;
    m.m[8] = (tanRight + tanLeft) / tanWidth;
    m.m[9] = (tanUp + tanDown) / tanHeight;
    m.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m.m[11] = -1.f;
    m.m[14] = -(2.f * farZ * nearZ) / (farZ - nearZ);
    return m;
}

VrMat4 RendererGLES::MakeView(const XrPosef& pose) {
    const float x = pose.orientation.x;
    const float y = pose.orientation.y;
    const float z = pose.orientation.z;
    const float w = pose.orientation.w;

    // Inverse rigid transform: conjugate rotation then translated world position.
    const float r00 = 1.f - 2.f * (y * y + z * z);
    const float r01 = 2.f * (x * y - z * w);
    const float r02 = 2.f * (x * z + y * w);
    const float r10 = 2.f * (x * y + z * w);
    const float r11 = 1.f - 2.f * (x * x + z * z);
    const float r12 = 2.f * (y * z - x * w);
    const float r20 = 2.f * (x * z - y * w);
    const float r21 = 2.f * (y * z + x * w);
    const float r22 = 1.f - 2.f * (x * x + y * y);

    const float px = pose.position.x;
    const float py = pose.position.y;
    const float pz = pose.position.z;

    VrMat4 m{};
    // transpose(R) in column-major storage
    m.m[0] = r00; m.m[1] = r01; m.m[2] = r02; m.m[3] = 0.f;
    m.m[4] = r10; m.m[5] = r11; m.m[6] = r12; m.m[7] = 0.f;
    m.m[8] = r20; m.m[9] = r21; m.m[10] = r22; m.m[11] = 0.f;
    m.m[12] = -(r00 * px + r10 * py + r20 * pz);
    m.m[13] = -(r01 * px + r11 * py + r21 * pz);
    m.m[14] = -(r02 * px + r12 * py + r22 * pz);
    m.m[15] = 1.f;
    return m;
}

bool RendererGLES::BuildFallbackScene() {
    static constexpr const char* kVs = R"GLSL(#version 300 es
layout(location=0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); }
)GLSL";
    static constexpr const char* kFs = R"GLSL(#version 300 es
precision mediump float;
out vec4 fragColor;
void main() { fragColor = vec4(0.15, 0.85, 1.0, 1.0); }
)GLSL";

    GLuint vs = Compile(GL_VERTEX_SHADER, kVs);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs) return false;

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return false;
    mvpLocation_ = glGetUniformLocation(program_, "uMvp");

    std::vector<float> vertices;
    auto line = [&vertices](float ax, float ay, float az, float bx, float by, float bz) {
        vertices.insert(vertices.end(), {ax, ay, az, bx, by, bz});
    };

    // Simple native stereo verification scene: a long F-Zero-like corridor and floor grid.
    for (int z = 0; z <= 24; ++z) {
        const float zz = -static_cast<float>(z) * 2.f - 2.f;
        line(-8.f, -1.6f, zz, 8.f, -1.6f, zz);
    }
    for (int x = -8; x <= 8; x += 2) {
        line(static_cast<float>(x), -1.6f, -2.f, static_cast<float>(x), -1.6f, -50.f);
    }
    line(-8.f, -1.6f, -2.f, -8.f, 2.f, -50.f);
    line(8.f, -1.6f, -2.f, 8.f, 2.f, -50.f);
    line(-8.f, 2.f, -50.f, 8.f, 2.f, -50.f);

    vertexCount_ = static_cast<GLsizei>(vertices.size() / 3);

    GLint oldVbo = 0;
    GLint oldVao = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldVbo);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);

    // Keep the diagnostic fallback completely isolated from Fast3D's VAO/VBO. The OpenXR
    // renderer and Fast3D share one GLES context, so binding attribute 0 on whatever VAO happened
    // to be current would silently rewrite Fast3D's vertex layout.
    glGenVertexArrays(1, &vertexArray_);
    glBindVertexArray(vertexArray_);
    glGenBuffers(1, &vertexBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), reinterpret_cast<void*>(0));

    glBindVertexArray(static_cast<GLuint>(oldVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldVbo));
    return true;
}

bool RendererGLES::Initialize() {
    glGenFramebuffers(1, &framebuffer_);
    glGenRenderbuffers(1, &depthBuffer_);
    return framebuffer_ != 0 && depthBuffer_ != 0 && BuildFallbackScene();
}

bool RendererGLES::EnsureFramebuffer(uint32_t width, uint32_t height, uint32_t colorTexture) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

    if (width != depthWidth_ || height != depthHeight_) {
        glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        depthWidth_ = width;
        depthHeight_ = height;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer_);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "OpenXR framebuffer incomplete: 0x%x", status);
        return false;
    }
    return true;
}

void RendererGLES::DrawFallback(const VrEyeFrame& eye) {
    const VrMat4 mvp = Mul(eye.projection, eye.view);
    glUseProgram(program_);
    glUniformMatrix4fv(mvpLocation_, 1, GL_FALSE, mvp.m.data());
    glBindVertexArray(vertexArray_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glDrawArrays(GL_LINES, 0, vertexCount_);
}

bool RendererGLES::RenderEye(VrEyeFrame& eye, GDiffuserBridge& game, const QuestGameInput& input) {
    eye.view = MakeView(eye.pose);
    eye.projection = MakeProjection(eye.fov, 0.05f, 4000.f);

    // Normal F-Zero presentation is already a texture-to-XR-image blit performed by the game host.
    // It owns dedicated read/draw FBOs and restores the tiny amount of GL state it mutates. Do not
    // build/clear a second depth framebuffer or synchronously query ~15 shared-context state values
    // around that blit. On Adreno those glGet* calls can serialize the driver twice per VR frame.
    // Keep the heavy state-preserving path below only for the diagnostic fallback scene.
    if (game.RenderEye(eye, input)) {
        glFlush();
        return true;
    }

    // Fallback only: OpenXR presentation and Fast3D share the same EGL context. Preserve every
    // state item this diagnostic renderer mutates because Fast3D caches GL state internally.
    GLint oldProgram = 0;
    GLint oldArrayBuffer = 0;
    GLint oldVertexArray = 0;
    GLint oldDrawFramebuffer = 0;
    GLint oldReadFramebuffer = 0;
    GLint oldRenderbuffer = 0;
    GLint oldViewport[4] = {};
    GLint oldScissor[4] = {};
    GLint oldDepthFunc = GL_LESS;
    GLfloat oldClearColor[4] = {};
    GLboolean oldDepthMask = GL_TRUE;
    const GLboolean oldDepthTest = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean oldBlend = glIsEnabled(GL_BLEND);
    const GLboolean oldScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean oldPolygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);

    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVertexArray);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &oldRenderbuffer);
    glGetIntegerv(GL_VIEWPORT, oldViewport);
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetIntegerv(GL_DEPTH_FUNC, &oldDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);

    auto restoreState = [&]() {
        glUseProgram(static_cast<GLuint>(oldProgram));
        glBindVertexArray(static_cast<GLuint>(oldVertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(oldRenderbuffer));
        glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
        glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
        glDepthFunc(static_cast<GLenum>(oldDepthFunc));
        glDepthMask(oldDepthMask);
        glClearColor(oldClearColor[0], oldClearColor[1], oldClearColor[2], oldClearColor[3]);

        if (oldDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        if (oldScissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (oldPolygonOffset) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
    };

    if (!EnsureFramebuffer(eye.width, eye.height, eye.colorTexture)) {
        restoreState();
        return false;
    }

    glViewport(0, 0, static_cast<GLsizei>(eye.width), static_cast<GLsizei>(eye.height));
    // The shared Fast3D context may leave a smaller UI scissor enabled. Clear and compositor blits
    // must cover the whole OpenXR swapchain image or stale fallback/grid pixels survive outside
    // the menu region.
    glDisable(GL_SCISSOR_TEST);
    glScissor(0, 0, static_cast<GLsizei>(eye.width), static_cast<GLsizei>(eye.height));
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glClearColor(0.005f, 0.008f, 0.015f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    DrawFallback(eye);

    glFlush();
    restoreState();
    return true;
}

void RendererGLES::Shutdown() {
    if (vertexArray_) glDeleteVertexArrays(1, &vertexArray_);
    if (vertexBuffer_) glDeleteBuffers(1, &vertexBuffer_);
    if (program_) glDeleteProgram(program_);
    if (depthBuffer_) glDeleteRenderbuffers(1, &depthBuffer_);
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    vertexBuffer_ = program_ = depthBuffer_ = framebuffer_ = 0;
    depthWidth_ = depthHeight_ = 0;
}
