#pragma once

#include "vr_types.h"

#include <GLES3/gl3.h>

class GDiffuserBridge;

class RendererGLES {
public:
    bool Initialize();
    void Shutdown();
    bool RenderEye(VrEyeFrame& eye, GDiffuserBridge& game, const QuestGameInput& input);

    static VrMat4 MakeProjection(const XrFovf& fov, float nearZ, float farZ);
    static VrMat4 MakeView(const XrPosef& pose);

private:
    bool EnsureFramebuffer(uint32_t width, uint32_t height, uint32_t colorTexture);
    bool BuildFallbackScene();
    void DrawFallback(const VrEyeFrame& eye);

    GLuint framebuffer_ = 0;
    GLuint depthBuffer_ = 0;
    uint32_t depthWidth_ = 0;
    uint32_t depthHeight_ = 0;

    GLuint program_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint vertexArray_ = 0;
    GLint mvpLocation_ = -1;
    GLsizei vertexCount_ = 0;
};
