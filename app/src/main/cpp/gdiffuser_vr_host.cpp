#include "quest_fast3d_window.h"

#include <android/log.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>

#include <fast/interpreter.h>
#include <ship/Context.h>
#include <ship/audio/AudioPlayer.h>
#include <ship/config/Config.h>
#include <ship/config/ConsoleVariable.h>
#include <libultraship/bridge/audiobridge.h>
#include <ship/resource/ResourceManager.h>

#include "resource/ResourceFactories.h"
#include "gdx_camera_pose.h"
#include "gdx_dev_gates.h"
#include "diorama_course_culling.h"
#include "fzx_game.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

namespace Fast {
extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx);
}

extern "C" {
// Fast3D reads this directly in its per-vertex aspect helper. A plain load avoids an out-of-line
// host callback for every transformed vertex while preserving the exact same VR/non-VR decision.
int gdx_vr_fast3d_eye_active_flag = 0;
}

namespace {
constexpr const char* kTag = "FZeroXVR/GameHost";
constexpr float kPi = 3.14159265358979323846f;
// mario64VRStandalone uses 100 game units/metre for its life-size first-person preset and a much
// smaller-world 1200 u/m third-person diorama. F-Zero is a chase-camera game, so keep the proven
// stereo baseline conservative while making physical head translation substantially stronger.
// This first calibration uses 300 u/m for room-scale: a 20 cm lean becomes 60 F-Zero units.
// Stereo remains at 60 u/m until the direct OpenXR projection below is validated in-headset.
constexpr float kStereoMetersToGameUnits = 300.0f;
constexpr float kHeadMetersToGameUnits = 300.0f;
// Diorama preset modelled after mario64VRStandalone. The game camera becomes the origin of a
// miniature world that is anchored in LOCAL space, while OpenXR supplies native per-eye view/IPD.
constexpr float kDioramaScaleGameUnitsPerMeter = 1376.0f;
constexpr float kDioramaDistanceMeters = 0.65f;
constexpr float kDioramaHeightMeters = -0.35f;
constexpr float kDioramaStereoDepthScale = 1.35f;
constexpr float kDioramaHeadMotionScale = 1.50f; // extra miniature parallax; native Quest pose still submitted to OpenXR.
constexpr uint32_t kRaceHudMarker = 0x56524844u; // 'VRHD' inserted immediately before Menus_Draw().
constexpr uint32_t kRaceSkyMarker = 0x5652534Bu; // 'VRSK' immediately before each Background_Draw().
constexpr uint32_t kRaceWorldMarker = 0x56523344u; // 'VR3D' immediately after each Background_Draw().
constexpr uint32_t kHudWidth = 2048;
constexpr uint32_t kHudHeight = 1536;
// Fast3D previously rasterized every eye at the full 2800x2933 Quest swapchain extent and then
// immediately blitted that image again. Rasterize the 3D world at 80% linear resolution instead:
// ~36% fewer shaded pixels while OpenXR still receives a native-size image. The compositor/HUD
// stay full resolution, so text remains sharp and peripheral loss is mostly hidden by Quest FFR.
constexpr float kWorldRenderScale = 0.80f;
uint32_t WorldRenderDim(uint32_t value) {
    return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<float>(value) * kWorldRenderScale)));
}

struct HostEye {
    int eye;
    uint32_t width;
    uint32_t height;
    uint32_t colorTexture;
    float view[16];
    float projection[16];
    XrPosef xrPose;
    XrFovf xrFov;
};

struct GdxVrHostInput {
    float stickX,stickY,rightStickX,rightStickY;
    float leftTrigger,rightTrigger,leftSqueeze,rightSqueeze;
    uint32_t n64Buttons;
    XrPosef leftGrip,rightGrip;
    uint32_t handActiveMask;
};

// Narrow C ABI shared with the generated camera_quest.c. Keeping decomp Camera/Mtx/Gfx types out
// of this C++ translation unit avoids collisions with libultraship's Fast3D types.
struct GdxVrCameraIo {
    int id;
    int numPlayers;
    float eyeX,eyeY,eyeZ;
    float atX,atY,atZ;
    float upX,upY,upZ;
    float fov;
    float nearZ,farZ;
    float fovScaleX,fovScaleY;
    float frustrumCenterX,frustrumCenterY;
};

struct Vec3 { float x, y, z; };

Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 Mul(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
float Length(Vec3 a) { return std::sqrt(std::max(Dot(a,a), 0.0f)); }
Vec3 Normalize(Vec3 a) { const float n=Length(a); return n>1e-6f ? Mul(a,1.0f/n) : Vec3{0,0,0}; }

XrQuaternionf Conjugate(XrQuaternionf q) { return {-q.x,-q.y,-q.z,q.w}; }
XrQuaternionf QNormalize(XrQuaternionf q) {
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (n<1e-6f) return {0,0,0,1};
    return {q.x/n,q.y/n,q.z/n,q.w/n};
}
float QDot(XrQuaternionf a,XrQuaternionf b) { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
std::array<HostEye,2> gEyes{};
XrQuaternionf HeadOrientationFromEyes() {
    XrQuaternionf a=gEyes[0].xrPose.orientation;
    XrQuaternionf b=gEyes[1].xrPose.orientation;
    if (QDot(a,b)<0.f) b={-b.x,-b.y,-b.z,-b.w};
    return QNormalize({a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w});
}
XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    };
}
Vec3 QRotate(XrQuaternionf q, Vec3 v) {
    XrQuaternionf p{v.x,v.y,v.z,0.f};
    XrQuaternionf r=QMul(QMul(q,p),Conjugate(q));
    return {r.x,r.y,r.z};
}

bool gEyesValid=false;
bool gHeadBaseValid=false;
XrQuaternionf gHeadBaseOrientation{0,0,0,1};
XrVector3f gHeadBasePosition{0,0,0};
// Race tracking is recentered when entering a driving mode. Menu head motion must not make the
// first race frame start sideways. The decomp camera receives this center-head transform BEFORE
// it builds culling/background/model state; per-eye replay later adds only the physical eye offset.
bool gRaceTrackingActive=false;
XrQuaternionf gRaceBaseOrientation{0,0,0,1};
XrVector3f gRaceBasePosition{0,0,0};
bool gDioramaEnabled=false;
bool gDioramaToggleWasDown=false;
bool gDioramaAnchorValid=false;
XrVector3f gDioramaAnchorPosition{0,0,0};
XrVector3f gDioramaHeadRestPosition{0,0,0};
Vec3 gDioramaAnchorForward{0,0,-1};
XrQuaternionf gVrCenterHeadOrientation{0,0,0,1};
XrVector3f gVrCenterHeadPosition{0,0,0};
GdxCameraPose gVrCenterPose{};
bool gVrCenterPoseValid=false;
struct CourseCullState {
    bool valid=false;
    Vec3 eye{};
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    float tanX=1.0f;
    float tanY=1.0f;
};
CourseCullState gCourseCullState{};
struct DioramaSortState {
    bool valid=false;
    bool dirty=true;
    std::array<float,16> gameToEye{};
};
DioramaSortState gDioramaSortState{};
struct VrCameraBasisCache {
    uint64_t generation=0;
    GdxCameraPose pose{};
    Vec3 gameEye{};
    Vec3 baseForward{};
    Vec3 baseUp{};
    Vec3 baseRight{};
    Vec3 worldForward{};
    Vec3 worldUp{};
    Vec3 worldRight{};
    float lookDistance=1.0f;
    XrQuaternionf currentHead{0,0,0,1};
};
VrCameraBasisCache gVrCameraBasisCache{};
uint64_t gVrMatrixGeneration=1;
struct XrProjectionCache {
    bool valid=false;
    float l=0.0f;
    float r=0.0f;
    float d=0.0f;
    float u=0.0f;
    float w=0.0f;
    float h=0.0f;
};
XrProjectionCache gXrProjectionCache[2]{};
// Fast3D-native VR override, modelled after mario64VRStandalone: the interpreter keeps the game's
// model-view stack but substitutes this per-eye combined view-projection only when a 3D eye replay
// is active. This bypasses G-Diffuser's converted/scratch G_MTX pointer indirection entirely.
std::array<float,16> gFastVrWorldMatrix{};
std::array<float,16> gFastVrSkyMatrix{};
enum class FastVrSection : uint8_t { Off = 0, Sky = 1, World = 2 };
FastVrSection gFastVrSection=FastVrSection::Off;
int gFastVrEyeIndex=-1;
bool gFastVrEyeActive=false;
std::shared_ptr<QuestFast3dWindow> gWindow;
// libultraship's process-wide Context singleton is backed by a weak_ptr. Keep the Quest host's
// Context strongly owned for the complete native session so CVar/console/resource helpers called
// later by the decomp scheduler can safely resolve Context::GetInstance().
std::shared_ptr<Ship::Context> gContext;
bool gBooted=false;
std::string gFilesDir;
GLuint gCacheTextures[2]={0,0};
GLuint gReadFbo=0;
GLuint gDrawFbo=0;
uint32_t gCacheWidth[2]={0,0};
uint32_t gCacheHeight[2]={0,0};
bool gCacheValid[2]={false,false};
// Pose/FOV that were actually used when each persistent eye texture was rendered. The game
// produces new Fast3D frames at ~60 Hz while OpenXR presents at 72+ Hz, so the compositor must be
// told the render pose of a reused texture rather than the newer head pose from the current XR frame.
XrPosef gCacheRenderPose[2]{};
XrFovf gCacheRenderFov[2]{};
bool gCacheRenderViewValid[2]={false,false};
GLuint gHudTexture=0;
GLuint gHudMatteTextures[2]={0,0};
GLuint gHudCompositeProgram=0;
GLuint gHudCompositeVao=0;
GLint gHudBlackSampler=-1;
GLint gHudWhiteSampler=-1;
uint32_t gHudTextureWidth=0;
uint32_t gHudTextureHeight=0;
bool gHudValid=false;
bool gRaceHudActive=false;
bool gFlatUiActive=true;
int gHudMatteClearMode=0; // 0=normal Fast3D clear, 1=black, 2=white, 3=transparent RGBA HUD.

void QuestAfterFast3dClear(Fast::Interpreter*) {
    if (gHudMatteClearMode==0) return;
    GLint oldScissor[4]={};
    GLfloat oldClear[4]={};
    const GLboolean scissorEnabled=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX,oldScissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,oldClear);
    glDisable(GL_SCISSOR_TEST);
    const float matte=(gHudMatteClearMode==2) ? 1.f : 0.f;
    const float alpha=(gHudMatteClearMode==3) ? 0.f : 1.f;
    glClearColor(matte,matte,matte,alpha);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(oldClear[0],oldClear[1],oldClear[2],oldClear[3]);
    glScissor(oldScissor[0],oldScissor[1],oldScissor[2],oldScissor[3]);
    if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

void QuestPortLogTap(const char* message) {
    if (message != nullptr && message[0] != '\0') {
        __android_log_write(ANDROID_LOG_INFO, "FZeroXVR/GDX", message);
    }
}

struct FastReplayState {
    Fast::RSP rsp{};
    Fast::RDP rdp{};
    Fast::RenderingState rendering{};
    std::array<uintptr_t, 16> segments{};
    Fast::F3dex2Variant variant = Fast::F3dex2Variant::Standard;
};

FastReplayState CaptureReplayState(const std::shared_ptr<Fast::Interpreter>& interp) {
    FastReplayState state{};
    if (!interp || !interp->mRsp || !interp->mRdp) return state;
    state.rsp=*interp->mRsp;
    state.rdp=*interp->mRdp;
    state.rendering=interp->mRenderingState;
    for (size_t i=0;i<state.segments.size();++i) state.segments[i]=interp->mSegmentPointers[i];
    state.variant=interp->mF3dex2Variant;
    return state;
}

void RestoreReplayState(const std::shared_ptr<Fast::Interpreter>& interp,const FastReplayState& state) {
    if (!interp || !interp->mRsp || !interp->mRdp) return;
    *interp->mRsp=state.rsp;
    *interp->mRdp=state.rdp;
    interp->mRenderingState=state.rendering;
    for (size_t i=0;i<state.segments.size();++i) interp->mSegmentPointers[i]=state.segments[i];
    interp->mF3dex2Variant=state.variant;
}

void EnsureCache(int eye, uint32_t width, uint32_t height) {
    if (eye<0 || eye>1) return;
    const bool create=(gCacheTextures[eye]==0);
    const bool resize=(gCacheWidth[eye]!=width || gCacheHeight[eye]!=height);
    if (create) glGenTextures(1,&gCacheTextures[eye]);
    if (create || resize) {
        glBindTexture(GL_TEXTURE_2D,gCacheTextures[eye]);
        if (create) {
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        if (resize) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,static_cast<GLsizei>(width),static_cast<GLsizei>(height),0,
                         GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            gCacheWidth[eye]=width; gCacheHeight[eye]=height; gCacheValid[eye]=false;
        }
        glBindTexture(GL_TEXTURE_2D,0);
    }
    if (!gReadFbo) glGenFramebuffers(1,&gReadFbo);
    if (!gDrawFbo) glGenFramebuffers(1,&gDrawFbo);
}

GLuint CompileHudShader(GLenum type,const char* source) {
    GLuint shader=glCreateShader(type);
    glShaderSource(shader,1,&source,nullptr);
    glCompileShader(shader);
    GLint ok=GL_FALSE;
    glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        char log[1024]={};
        glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
        __android_log_print(ANDROID_LOG_ERROR,kTag,"HUD matte shader compile failed: %s",log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool EnsureHudCompositeProgram() {
    if (gHudCompositeProgram) return true;
    static const char* kVs=R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));
    vUv=p;
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
})";
    static const char* kFs=R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uBlack;
uniform sampler2D uWhite;
out vec4 fragColor;
void main() {
    vec2 uv=vec2(vUv.x,1.0-vUv.y);
    vec3 black=texture(uBlack,uv).rgb;
    vec3 white=texture(uWhite,uv).rgb;
    vec3 transmission=clamp(white-black,0.0,1.0);
    float oneMinusAlpha=clamp((transmission.r+transmission.g+transmission.b)/3.0,0.0,1.0);
    float alpha=1.0-oneMinusAlpha;
    // Some Fast3D HUD setup paths force an opaque black clear in both matte replays. In that case
    // the mathematically reconstructed alpha is 1 even though the pixel is just the empty panel.
    // Treat only an equal, near-black pair as empty. This intentionally sacrifices pure-black HUD
    // outline pixels in the pathological path, while preserving every colored/white HUD element.
    float matteDelta=max(max(abs(white.r-black.r),abs(white.g-black.g)),abs(white.b-black.b));
    float blackLevel=max(max(black.r,black.g),black.b);
    if (matteDelta < 0.015 && blackLevel < 0.025) alpha=0.0;
    // black is already foreground*alpha from source-over rendering on a black matte, i.e. exactly
    // the premultiplied color OpenXR expects when UNPREMULTIPLIED_ALPHA is not set.
    fragColor=vec4(black,alpha);
})";
    GLuint vs=CompileHudShader(GL_VERTEX_SHADER,kVs);
    GLuint fs=CompileHudShader(GL_FRAGMENT_SHADER,kFs);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    gHudCompositeProgram=glCreateProgram();
    glAttachShader(gHudCompositeProgram,vs);
    glAttachShader(gHudCompositeProgram,fs);
    glLinkProgram(gHudCompositeProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok=GL_FALSE;
    glGetProgramiv(gHudCompositeProgram,GL_LINK_STATUS,&ok);
    if (!ok) {
        char log[1024]={};
        glGetProgramInfoLog(gHudCompositeProgram,sizeof(log),nullptr,log);
        __android_log_print(ANDROID_LOG_ERROR,kTag,"HUD matte program link failed: %s",log);
        glDeleteProgram(gHudCompositeProgram);
        gHudCompositeProgram=0;
        return false;
    }
    glGenVertexArrays(1,&gHudCompositeVao);
    gHudBlackSampler=glGetUniformLocation(gHudCompositeProgram,"uBlack");
    gHudWhiteSampler=glGetUniformLocation(gHudCompositeProgram,"uWhite");
    return true;
}

void ConfigureHudTexture(GLuint texture,uint32_t width,uint32_t height,bool allocate) {
    glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if (allocate) {
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,static_cast<GLsizei>(width),static_cast<GLsizei>(height),0,
                     GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    }
}

void EnsureHudTexture(uint32_t width, uint32_t height) {
    const bool createHud=(gHudTexture==0);
    if (createHud) glGenTextures(1,&gHudTexture);
    const bool createMattes=(gHudMatteTextures[0]==0 || gHudMatteTextures[1]==0);
    if (createMattes) {
        if (gHudMatteTextures[0] || gHudMatteTextures[1]) {
            glDeleteTextures(2,gHudMatteTextures);
            gHudMatteTextures[0]=gHudMatteTextures[1]=0;
        }
        glGenTextures(2,gHudMatteTextures);
    }
    const bool resize=(gHudTextureWidth!=width || gHudTextureHeight!=height);
    if (createHud || resize) ConfigureHudTexture(gHudTexture,width,height,true);
    if (createMattes || resize) {
        ConfigureHudTexture(gHudMatteTextures[0],width,height,true);
        ConfigureHudTexture(gHudMatteTextures[1],width,height,true);
    }
    if (createHud || createMattes || resize) glBindTexture(GL_TEXTURE_2D,0);
    if (resize) {
        gHudTextureWidth=width;
        gHudTextureHeight=height;
        gHudValid=false;
    }
    if (!gReadFbo) glGenFramebuffers(1,&gReadFbo);
    if (!gDrawFbo) glGenFramebuffers(1,&gDrawFbo);
}

bool CompositeHudMattes(uint32_t width,uint32_t height) {
    if (!gHudTexture || !gHudMatteTextures[0] || !gHudMatteTextures[1] || !EnsureHudCompositeProgram()) return false;
    GLint oldDraw=0,oldProgram=0,oldVao=0,oldViewport[4]={};
    GLint oldActiveTexture=0;
    const GLboolean oldScissor=glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean oldBlend=glIsEnabled(GL_BLEND);
    const GLboolean oldDepth=glIsEnabled(GL_DEPTH_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glGetIntegerv(GL_CURRENT_PROGRAM,&oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&oldVao);
    glGetIntegerv(GL_VIEWPORT,oldViewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&oldActiveTexture);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,gHudTexture,0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
        return false;
    }
    glViewport(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height));
    glUseProgram(gHudCompositeProgram);
    glBindVertexArray(gHudCompositeVao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,gHudMatteTextures[0]);
    glUniform1i(gHudBlackSampler,0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,gHudMatteTextures[1]);
    glUniform1i(gHudWhiteSampler,1);
    glDrawArrays(GL_TRIANGLES,0,3);
    glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    glBindVertexArray(static_cast<GLuint>(oldVao));
    glUseProgram(static_cast<GLuint>(oldProgram));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (oldBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (oldDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    return true;
}

void ClearRenderTargetTransparent(GLuint texture, uint32_t width, uint32_t height) {
    GLint oldDraw=0, oldViewport[4]={};
    GLfloat oldClear[4]={};
    const GLboolean oldScissor=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glGetIntegerv(GL_VIEWPORT,oldViewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,oldClear);
    glDisable(GL_SCISSOR_TEST);
    if (texture) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    } else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    }
    glViewport(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height));
    glClearColor(0.f,0.f,0.f,0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    glViewport(oldViewport[0],oldViewport[1],oldViewport[2],oldViewport[3]);
    glClearColor(oldClear[0],oldClear[1],oldClear[2],oldClear[3]);
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

size_t FindRaceHudMarker(const Fast::F3DGfx* commands) {
    if (!commands) return static_cast<size_t>(-1);
    constexpr size_t kMaxCommands=250000;
    for (size_t i=0;i<kMaxCommands;++i) {
        const uintptr_t w0=commands[i].words.w0;
        const uintptr_t w1=commands[i].words.w1;
        const uint8_t opcode=static_cast<uint8_t>((w0>>24)&0xFFu);
        if ((opcode==0x00u || opcode==0xC0u) && static_cast<uint32_t>(w1)==kRaceHudMarker) return i;
        if (opcode==0xDFu) break; // F3DEX2 G_ENDDL
    }
    return static_cast<size_t>(-1);
}

bool PatchFirstProjectionMatrix(Fast::F3DGfx* commands,size_t commandCount,const void* matrix64) {
    if (!commands || !matrix64) return false;
    for (size_t i=0;i<commandCount;++i) {
        const uintptr_t w0=commands[i].words.w0;
        const uint8_t opcode=static_cast<uint8_t>((w0>>24)&0xFFu);
        if (opcode==0xDFu) break;
        // F3DEX2 G_MTX = 0xDA. Bit 0x04 in the parameter byte is G_MTX_PROJECTION.
        // G-Diffuser can reroute this operand into an interpolation scratch slot during ConvertRoot,
        // so changing the original GfxPool matrix after conversion does not necessarily affect Run().
        // Point the converted command itself at the per-eye matrix to guarantee real stereo/6DoF.
        if (opcode==0xDAu && (w0&0x04u)!=0u) {
            commands[i].words.w1=reinterpret_cast<uintptr_t>(matrix64);
            return true;
        }
    }
    return false;
}

bool BlitFramebuffer(GLuint srcFbo, uint32_t srcW, uint32_t srcH,
                     GLuint dst, uint32_t dstW, uint32_t dstH) {
    if (!dst) return false;
    GLint oldRead=0, oldDraw=0;
    const GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    // Fast3D can leave a native-resolution UI scissor enabled. glBlitFramebuffer obeys that
    // destination scissor, which updates only a lower-left subsection of the Quest target and
    // leaves stale fallback/grid pixels around the menu. Compositor copies must always be full-frame.
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,srcFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
    glBlitFramebuffer(0,0,static_cast<GLint>(srcW),static_cast<GLint>(srcH),
                      0,0,static_cast<GLint>(dstW),static_cast<GLint>(dstH),
                      GL_COLOR_BUFFER_BIT,GL_LINEAR);
    const bool ok=true;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return ok;
}

bool CopyTexture(GLuint src, uint32_t srcW, uint32_t srcH, GLuint dst, uint32_t dstW, uint32_t dstH) {
    if (!src || !dst) return false;
    GLint oldRead=0,oldDraw=0;
    const GLboolean oldScissor=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,gReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,src,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
    glBlitFramebuffer(0,0,static_cast<GLint>(srcW),static_cast<GLint>(srcH),
                      0,0,static_cast<GLint>(dstW),static_cast<GLint>(dstH),
                      GL_COLOR_BUFFER_BIT,GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return true;
}

bool BlitFramebufferFlippedY(GLuint srcFbo, uint32_t srcW, uint32_t srcH,
                             GLuint dst, uint32_t dstW, uint32_t dstH) {
    if (!dst) return false;
    GLint oldRead=0, oldDraw=0;
    const GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,srcFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
    glBlitFramebuffer(0,static_cast<GLint>(srcH),static_cast<GLint>(srcW),0,
                      0,0,static_cast<GLint>(dstW),static_cast<GLint>(dstH),
                      GL_COLOR_BUFFER_BIT,GL_LINEAR);
    const bool ok=true;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return ok;
}

bool CopyTextureFlippedY(GLuint src, uint32_t srcW, uint32_t srcH,
                         GLuint dst, uint32_t dstW, uint32_t dstH) {
    if (!src || !dst) return false;
    GLint oldRead=0, oldDraw=0;
    const GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,gReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,src,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,gDrawFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
    // Fast3D's offscreen texture is sampled by the desktop presentation path with an implicit
    // Y inversion. Quest copies that texture directly into an OpenXR swapchain, so flat 2D UI
    // must apply the missing inversion here or text/menu geometry appears upside-down.
    glBlitFramebuffer(0,static_cast<GLint>(srcH),static_cast<GLint>(srcW),0,
                      0,0,static_cast<GLint>(dstW),static_cast<GLint>(dstH),
                      GL_COLOR_BUFFER_BIT,GL_LINEAR);
    const bool ok=true;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));
    if (oldScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return ok;
}

bool IsRaceVrMode(int mode) {
    switch (mode) {
        case GAMEMODE_GP_RACE:
        case GAMEMODE_PRACTICE:
        case GAMEMODE_VS_2P:
        case GAMEMODE_VS_3P:
        case GAMEMODE_VS_4P:
        case GAMEMODE_TIME_ATTACK:
        case GAMEMODE_DEATH_RACE:
            return true;
        default:
            return false;
    }
}

bool IsFlatMenuMode(int mode) {
    switch (mode) {
        case GAMEMODE_TITLE:
        case GAMEMODE_RECORDS:
        case GAMEMODE_MAIN_MENU:
        case GAMEMODE_MACHINE_SELECT:
        case GAMEMODE_MACHINE_SETTINGS:
        case GAMEMODE_COURSE_SELECT:
        case GAMEMODE_SKIPPABLE_CREDITS:
        case GAMEMODE_UNSKIPPABLE_CREDITS:
        case GAMEMODE_GP_RACE_NEXT_COURSE:
        case GAMEMODE_CREATE_MACHINE:
        case GAMEMODE_GP_RACE_NEXT_MACHINE_SETTINGS:
        case GAMEMODE_RECORDS_COURSE_SELECT:
        case GAMEMODE_OPTIONS_MENU:
            return true;
        default:
            return false;
    }
}

Vec3 MapHeadLocalToGame(Vec3 local, Vec3 right, Vec3 up, Vec3 forward) {
    // OpenXR local: +X right, +Y up, -Z forward.
    return Add(Add(Mul(right,local.x),Mul(up,local.y)),Mul(forward,-local.z));
}

void RebuildCourseCullState() {
    gCourseCullState.valid=false;
    if (!gEyesValid || !gVrCenterPoseValid || !gRaceTrackingActive || gDioramaEnabled) return;

    const GdxCameraPose& pose=gVrCenterPose;
    const Vec3 gameEye{pose.eyeX,pose.eyeY,pose.eyeZ};
    const Vec3 gameAt{pose.atX,pose.atY,pose.atZ};
    const Vec3 baseForward=Normalize(Sub(gameAt,gameEye));
    Vec3 baseUp=Normalize(Vec3{pose.upX,pose.upY,pose.upZ});
    Vec3 baseRight=Normalize(Cross(baseForward,baseUp));
    baseUp=Normalize(Cross(baseRight,baseForward));

    const XrQuaternionf relOrientation=QMul(Conjugate(gRaceBaseOrientation),gVrCenterHeadOrientation);
    const Vec3 localForward=QRotate(relOrientation,{0,0,-1});
    const Vec3 localUp=QRotate(relOrientation,{0,1,0});
    const Vec3 worldForward=Normalize(MapHeadLocalToGame(localForward,baseRight,baseUp,baseForward));
    Vec3 worldUp=Normalize(MapHeadLocalToGame(localUp,baseRight,baseUp,baseForward));
    const Vec3 worldRight=Normalize(Cross(worldForward,worldUp));
    worldUp=Normalize(Cross(worldRight,worldForward));

    const XrVector3f centerDeltaWorld{
        gVrCenterHeadPosition.x-gRaceBasePosition.x,
        gVrCenterHeadPosition.y-gRaceBasePosition.y,
        gVrCenterHeadPosition.z-gRaceBasePosition.z};
    const Vec3 centerDeltaLocal=QRotate(Conjugate(gRaceBaseOrientation),
                                        {centerDeltaWorld.x,centerDeltaWorld.y,centerDeltaWorld.z});
    const Vec3 centerOffsetGame=Mul(MapHeadLocalToGame(centerDeltaLocal,baseRight,baseUp,baseForward),
                                    kHeadMetersToGameUnits);

    float tanX=1.0f;
    float tanY=1.0f;
    for (int i=0;i<2;++i) {
        tanX=std::max(tanX,std::fabs(std::tan(gEyes[i].xrFov.angleLeft)));
        tanX=std::max(tanX,std::fabs(std::tan(gEyes[i].xrFov.angleRight)));
        tanY=std::max(tanY,std::fabs(std::tan(gEyes[i].xrFov.angleUp)));
        tanY=std::max(tanY,std::fabs(std::tan(gEyes[i].xrFov.angleDown)));
    }

    gCourseCullState.eye=Add(gameEye,centerOffsetGame);
    gCourseCullState.forward=worldForward;
    gCourseCullState.right=worldRight;
    gCourseCullState.up=worldUp;
    gCourseCullState.tanX=tanX;
    gCourseCullState.tanY=tanY;
    gCourseCullState.valid=true;
}

void MatMul4(std::array<float,16>& out,const std::array<float,16>& a,const std::array<float,16>& b) {
    std::array<float,16> tmp{};
    for (int i=0;i<4;++i) {
        for (int j=0;j<4;++j) {
            tmp[i*4+j]=a[i*4+0]*b[0*4+j]+a[i*4+1]*b[1*4+j]+
                         a[i*4+2]*b[2*4+j]+a[i*4+3]*b[3*4+j];
        }
    }
    out=tmp;
}

bool BuildLookAtRowMajor(std::array<float,16>& out,Vec3 eye,Vec3 at,Vec3 up) {
    const Vec3 backward=Normalize(Sub(eye,at));
    const Vec3 right=Normalize(Cross(up,backward));
    const Vec3 trueUp=Normalize(Cross(backward,right));
    if (Length(backward)<1e-6f || Length(right)<1e-6f || Length(trueUp)<1e-6f) return false;
    out={};
    out[0*4+0]=right.x;    out[1*4+0]=right.y;    out[2*4+0]=right.z;
    out[0*4+1]=trueUp.x;   out[1*4+1]=trueUp.y;   out[2*4+1]=trueUp.z;
    out[0*4+2]=backward.x; out[1*4+2]=backward.y; out[2*4+2]=backward.z;
    out[3*4+0]=-Dot(eye,right);
    out[3*4+1]=-Dot(eye,trueUp);
    out[3*4+2]=-Dot(eye,backward);
    out[3*4+3]=1.0f;
    return true;
}

bool BuildOpenXrProjection(std::array<float,16>& out,int eyeIndex,float zn,float zf) {
    if (!gEyesValid || eyeIndex<0 || eyeIndex>1) return false;
    XrProjectionCache& c=gXrProjectionCache[eyeIndex];
    if (!c.valid) {
        const XrFovf& fov=gEyes[eyeIndex].xrFov;
        c.l=std::tan(fov.angleLeft);
        c.r=std::tan(fov.angleRight);
        c.d=std::tan(fov.angleDown);
        c.u=std::tan(fov.angleUp);
        c.w=c.r-c.l;
        c.h=c.u-c.d;
        c.valid=(c.w>1e-5f) && (c.h>1e-5f);
    }
    if (!c.valid) return false;
    zn=std::max(zn,0.01f);
    zf=std::max(zf,zn+1.0f);
    out={};
    // Same row-vector/OpenGL [-1,+1] projection used by mario64VRStandalone. FOV tangents are
    // cached per xrLocateViews result; only near/far terms differ between WORLD and SKY.
    out[0*4+0]=2.0f/c.w;
    out[1*4+1]=2.0f/c.h;
    out[2*4+0]=(c.r+c.l)/c.w;
    out[2*4+1]=(c.u+c.d)/c.h;
    out[2*4+2]=-(zf+zn)/(zf-zn);
    out[2*4+3]=-1.0f;
    out[3*4+2]=-(2.0f*zf*zn)/(zf-zn);
    return true;
}

bool BuildXrView(std::array<float,16>& out,const XrPosef& pose) {
    const float x=pose.orientation.x, y=pose.orientation.y;
    const float z=pose.orientation.z, w=pose.orientation.w;
    const float r00=1.0f-2.0f*(y*y+z*z);
    const float r01=2.0f*(x*y+z*w);
    const float r02=2.0f*(x*z-y*w);
    const float r10=2.0f*(x*y-z*w);
    const float r11=1.0f-2.0f*(x*x+z*z);
    const float r12=2.0f*(y*z+x*w);
    const float r20=2.0f*(x*z+y*w);
    const float r21=2.0f*(y*z-x*w);
    const float r22=1.0f-2.0f*(x*x+y*y);
    out={};
    // Rigid inverse of the OpenXR eye pose, row-vector convention (same as Mario standalone).
    out[0]=r00; out[1]=r10; out[2]=r20;
    out[4]=r01; out[5]=r11; out[6]=r21;
    out[8]=r02; out[9]=r12; out[10]=r22;
    const float px=pose.position.x, py=pose.position.y, pz=pose.position.z;
    out[12]=-(px*out[0]+py*out[4]+pz*out[8]);
    out[13]=-(px*out[1]+py*out[5]+pz*out[9]);
    out[14]=-(px*out[2]+py*out[6]+pz*out[10]);
    out[15]=1.0f;
    return true;
}

void CaptureDioramaAnchor() {
    Vec3 forward=QRotate(gVrCenterHeadOrientation,{0,0,-1});
    forward.y=0.0f;
    forward=Normalize(forward);
    if (Length(forward)<1e-5f) forward={0,0,-1};
    gDioramaAnchorForward=forward;
    gDioramaHeadRestPosition=gVrCenterHeadPosition;
    gDioramaAnchorPosition={
        gVrCenterHeadPosition.x+forward.x*kDioramaDistanceMeters,
        gVrCenterHeadPosition.y+kDioramaHeightMeters,
        gVrCenterHeadPosition.z+forward.z*kDioramaDistanceMeters};
    gDioramaAnchorValid=true;
    __android_log_print(ANDROID_LOG_INFO,kTag,
                        "Diorama anchor captured: scale=%.0f u/m distance=%.2fm height=%.2fm",
                        kDioramaScaleGameUnitsPerMeter,kDioramaDistanceMeters,kDioramaHeightMeters);
}

bool BuildDioramaGameToEye(int eyeIndex,std::array<float,16>& out) {
    if (!gEyesValid || !gVrCenterPoseValid || eyeIndex<0 || eyeIndex>1) return false;
    if (!gDioramaAnchorValid) CaptureDioramaAnchor();

    const GdxCameraPose& pose=gVrCenterPose;
    std::array<float,16> gameView{}, anchor{}, eyeView{};
    if (!BuildLookAtRowMajor(gameView,
                             {pose.eyeX,pose.eyeY,pose.eyeZ},
                             {pose.atX,pose.atY,pose.atZ},
                             {pose.upX,pose.upY,pose.upZ})) return false;

    // Camera-space -> LOCAL-space miniature transform. The camera origin is the center of the
    // diorama, exactly the same conceptual A matrix used by mario64VRStandalone.
    const Vec3 worldUp{0,1,0};
    const Vec3 right=Normalize(Cross(gDioramaAnchorForward,worldUp));
    const Vec3 up=Normalize(Cross(right,gDioramaAnchorForward));
    const Vec3 back=Mul(gDioramaAnchorForward,-1.0f);
    const float s=1.0f/kDioramaScaleGameUnitsPerMeter;
    anchor={};
    anchor[0]=right.x*s; anchor[1]=right.y*s; anchor[2]=right.z*s;
    anchor[4]=up.x*s;    anchor[5]=up.y*s;    anchor[6]=up.z*s;
    anchor[8]=back.x*s;  anchor[9]=back.y*s;  anchor[10]=back.z*s;
    anchor[12]=gDioramaAnchorPosition.x;
    anchor[13]=gDioramaAnchorPosition.y;
    anchor[14]=gDioramaAnchorPosition.z;
    anchor[15]=1.0f;

    const HostEye& e=gEyes[eyeIndex];
    BuildXrView(eyeView,e.xrPose);

    // Strengthen only binocular disparity for the miniature world. Keep the actual OpenXR eye pose
    // untouched for composition/timewarp; like Mario's stereo comfort path, counter-translate the
    // WORLD before the native eye view. This gives stronger depth without inventing a fake submitted
    // camera or amplifying room-scale head movement.
    const XrVector3f center=gVrCenterHeadPosition;
    const XrVector3f eyeDelta{
        e.xrPose.position.x-center.x,
        e.xrPose.position.y-center.y,
        e.xrPose.position.z-center.z};
    const XrVector3f headDelta{
        center.x-gDioramaHeadRestPosition.x,
        center.y-gDioramaHeadRestPosition.y,
        center.z-gDioramaHeadRestPosition.z};
    const XrVector3f desiredCenter{
        gDioramaHeadRestPosition.x+headDelta.x*kDioramaHeadMotionScale,
        gDioramaHeadRestPosition.y+headDelta.y*kDioramaHeadMotionScale,
        gDioramaHeadRestPosition.z+headDelta.z*kDioramaHeadMotionScale};
    const XrVector3f desiredEye{
        desiredCenter.x+eyeDelta.x*kDioramaStereoDepthScale,
        desiredCenter.y+eyeDelta.y*kDioramaStereoDepthScale,
        desiredCenter.z+eyeDelta.z*kDioramaStereoDepthScale};
    std::array<float,16> trackingComp{};
    trackingComp[0]=trackingComp[5]=trackingComp[10]=trackingComp[15]=1.0f;
    trackingComp[12]=e.xrPose.position.x-desiredEye.x;
    trackingComp[13]=e.xrPose.position.y-desiredEye.y;
    trackingComp[14]=e.xrPose.position.z-desiredEye.z;

    std::array<float,16> gameToLocal{}, trackedLocal{};
    MatMul4(gameToLocal,gameView,anchor);
    MatMul4(trackedLocal,gameToLocal,trackingComp);
    MatMul4(out,trackedLocal,eyeView);
    return true;
}

bool BuildDioramaWorldMatrix(int eyeIndex,std::array<float,16>& out) {
    std::array<float,16> gameToEye{}, projection{};
    if (!BuildDioramaGameToEye(eyeIndex,gameToEye)) return false;
    if (!BuildOpenXrProjection(projection,eyeIndex,0.02f,100.0f)) return false;
    MatMul4(out,gameToEye,projection);
    return true;
}

bool GetVrCameraBasis(VrCameraBasisCache*& out) {
    if (!gEyesValid || !gVrCenterPoseValid) return false;
    if (gVrCameraBasisCache.generation != gVrMatrixGeneration) {
        auto& c=gVrCameraBasisCache;
        c.pose=gVrCenterPose; // always the untouched F-Zero chase camera
        c.gameEye={c.pose.eyeX,c.pose.eyeY,c.pose.eyeZ};
        const Vec3 gameAt{c.pose.atX,c.pose.atY,c.pose.atZ};
        c.baseForward=Normalize(Sub(gameAt,c.gameEye));
        c.baseUp=Normalize(Vec3{c.pose.upX,c.pose.upY,c.pose.upZ});
        c.baseRight=Normalize(Cross(c.baseForward,c.baseUp));
        c.baseUp=Normalize(Cross(c.baseRight,c.baseForward));
        c.lookDistance=std::max(Length(Sub(gameAt,c.gameEye)),1.0f);

        // Render-only tracking, matching mario64VRStandalone's model: gameplay/culling keep the
        // native game camera while physical HMD orientation affects only the render basis.
        c.currentHead=gVrCenterHeadOrientation;
        const XrQuaternionf relOrientation=QMul(Conjugate(gRaceBaseOrientation),c.currentHead);
        const Vec3 localForward=QRotate(relOrientation,{0,0,-1});
        const Vec3 localUp=QRotate(relOrientation,{0,1,0});
        c.worldForward=Normalize(MapHeadLocalToGame(localForward,c.baseRight,c.baseUp,c.baseForward));
        c.worldUp=Normalize(MapHeadLocalToGame(localUp,c.baseRight,c.baseUp,c.baseForward));
        c.worldRight=Normalize(Cross(c.worldForward,c.worldUp));
        c.worldUp=Normalize(Cross(c.worldRight,c.worldForward));
        c.generation=gVrMatrixGeneration;
    }
    out=&gVrCameraBasisCache;
    return true;
}

bool BuildVrCameraMatrix(int eyeIndex, bool skyOnly, std::array<float,16>& out) {
    if (eyeIndex<0 || eyeIndex>1) return false;
    VrCameraBasisCache* basis=nullptr;
    if (!GetVrCameraBasis(basis) || basis==nullptr) return false;
    const GdxCameraPose& pose=basis->pose;
    const Vec3 gameEye=basis->gameEye;
    const Vec3 baseForward=basis->baseForward;
    const Vec3 baseUp=basis->baseUp;
    const Vec3 baseRight=basis->baseRight;
    const float lookDistance=basis->lookDistance;
    const XrQuaternionf currentHead=basis->currentHead;
    const Vec3 worldForward=basis->worldForward;
    const Vec3 worldUp=basis->worldUp;
    const Vec3 worldRight=basis->worldRight;

    Vec3 vrEye=gameEye;
    if (!skyOnly) {
        const XrVector3f centerDeltaWorld{
            gVrCenterHeadPosition.x-gRaceBasePosition.x,
            gVrCenterHeadPosition.y-gRaceBasePosition.y,
            gVrCenterHeadPosition.z-gRaceBasePosition.z};
        const Vec3 centerDeltaLocal=QRotate(Conjugate(gRaceBaseOrientation),
                                            {centerDeltaWorld.x,centerDeltaWorld.y,centerDeltaWorld.z});
        const Vec3 centerOffsetGame=Mul(MapHeadLocalToGame(centerDeltaLocal,baseRight,baseUp,baseForward),
                                        kHeadMetersToGameUnits);
        vrEye=Add(vrEye,centerOffsetGame);

        // Sparse room-scale diagnostic: log only when the largest lean grows by another 5 cm.
        // This proves whether physical translation reaches the renderer without adding per-frame spam.
        if (eyeIndex==0) {
            const float leanMeters=Length({centerDeltaWorld.x,centerDeltaWorld.y,centerDeltaWorld.z});
            static float sLoggedLeanMeters=0.0f;
            if (leanMeters >= sLoggedLeanMeters + 0.05f) {
                sLoggedLeanMeters=std::floor(leanMeters/0.05f)*0.05f;
                __android_log_print(ANDROID_LOG_INFO,kTag,
                                    "roomscale lean %.2fm -> game offset (%.1f %.1f %.1f)",
                                    leanMeters,centerOffsetGame.x,centerOffsetGame.y,centerOffsetGame.z);
            }
        }

        // IPD is an eye-local offset, so remove the CURRENT head orientation first and then apply
        // it in the rotated F-Zero camera basis. This keeps the two eyes rigid when yaw/pitch changes.
        const HostEye& e=gEyes[eyeIndex];
        const XrVector3f eyeDeltaWorld{
            e.xrPose.position.x-gVrCenterHeadPosition.x,
            e.xrPose.position.y-gVrCenterHeadPosition.y,
            e.xrPose.position.z-gVrCenterHeadPosition.z};
        const Vec3 eyeDeltaHeadLocal=QRotate(Conjugate(currentHead),
                                             {eyeDeltaWorld.x,eyeDeltaWorld.y,eyeDeltaWorld.z});
        vrEye=Add(vrEye,Mul(MapHeadLocalToGame(eyeDeltaHeadLocal,worldRight,worldUp,worldForward),
                            kStereoMetersToGameUnits));
    }

    const Vec3 vrAt=Add(vrEye,Mul(worldForward,lookDistance));

    // Build the final eye matrix entirely in float. The previous path converted the Quest's four
    // asymmetric FOV tangents back into F-Zero's fov/fovScale/frustumCenter representation and then
    // quantized through the N64 angle table + 16.16 Mtx. That approximation produces visible radial
    // "bubble" warping on a wide/canted Quest frustum. This is the exact OpenXR projection used by
    // mario64VRStandalone: game-space LookAt * native per-eye OpenXR projection.
    std::array<float,16> view{};
    std::array<float,16> projection{};
    if (!BuildLookAtRowMajor(view,vrEye,vrAt,worldUp)) return false;
    const float nearZ=skyOnly ? 1.0f : std::max(pose.nearZ,0.1f);
    const float farZ=skyOnly ? 50000.0f : std::max(pose.farZ,nearZ+100.0f);
    if (!BuildOpenXrProjection(projection,eyeIndex,nearZ,farZ)) return false;
    MatMul4(out,view,projection);
    return true;
}

bool ActivateFastVrEye(int eyeIndex) {
    const bool worldOk=gDioramaEnabled
        ? BuildDioramaWorldMatrix(eyeIndex,gFastVrWorldMatrix)
        : BuildVrCameraMatrix(eyeIndex,false,gFastVrWorldMatrix);
    if (!worldOk || !BuildVrCameraMatrix(eyeIndex,true,gFastVrSkyMatrix)) {
        gFastVrEyeActive=false;
        gdx_vr_fast3d_eye_active_flag=0;
        gFastVrSection=FastVrSection::Off;
        return false;
    }
    gFastVrEyeActive=true;
    gdx_vr_fast3d_eye_active_flag=1;
    gFastVrEyeIndex=eyeIndex;
    gFastVrSection=FastVrSection::Off; // VRSK/VR3D tags switch sections during this single Run().
    return true;
}

void DeactivateFastVrEye() {
    gFastVrEyeActive=false;
    gdx_vr_fast3d_eye_active_flag=0;
    gFastVrEyeIndex=-1;
    gFastVrSection=FastVrSection::Off;
}

std::string FindRom(const char* filesDir) {
    const char* names[]={"baserom.us.rev0.z64","fzerox.z64","f-zero-x.z64","F-Zero X (USA).z64"};
    for (const char* n:names) {
        std::filesystem::path p=std::filesystem::path(filesDir)/n;
        if (std::filesystem::is_regular_file(p)) return p.string();
    }
    return {};
}

bool LoadStartDioramaSetting(const char* filesDir) {
    if (filesDir==nullptr || filesDir[0]=='\0') return false;
    std::filesystem::path path=std::filesystem::path(filesDir)/"vr_settings.cfg";
    FILE* file=std::fopen(path.string().c_str(),"rb");
    if (file==nullptr) return false;
    bool enabled=false;
    char line[128]{};
    while (std::fgets(line,sizeof(line),file)!=nullptr) {
        int value=0;
        if (std::sscanf(line,"start_diorama=%d",&value)==1) enabled=(value!=0);
    }
    std::fclose(file);
    return enabled;
}

std::vector<std::string> FindArchives(const char* filesDir) {
    std::vector<std::string> out;
    const char* names[]={"gdiffuser.o2r","gdiffuser.otr","fzerox.o2r","assets.o2r"};
    for (const char* n:names) {
        std::filesystem::path p=std::filesystem::path(filesDir)/n;
        if (std::filesystem::exists(p)) out.push_back(p.string());
    }
    return out;
}
}

std::string GdxQuestInstallBuiltinResources(const char* filesDir);

extern "C" {
void GDiffuser_LoadAllAssets(void);
void gdx_sched_init(void);
void gdx_sched_drain_deferred_wakes(void);
void gdx_vi_tick(void);
void gdx_dispatch(void);
void gdx_rdram_init(void);
void gdx_init_rom(int argc,char** argv,int archivesValidated);
void gdx_controller_poll(void);
void gdx_fixed_aspect_tick(void);
void gdx_boot_warm_asset_segments(void);
unsigned int gdx_segment_source_fallback_total(void);
int GdxSegmentSourcePreload(uint32_t romBase);
int GdxSegmentSourcePayload(uint32_t romBase, void** outPayload, uint32_t* outSize);
void gdx_register_host_range(void* ptr, size_t size);
void gdx_register_main_module_range(void);
void gdx_audio_thread_start(int argc, char** argv);
void gdx_audio_thread_stop(void);
void gdx_audio_thread_notify_frame(void);
int gdx_audio_thread_active(void);
void bootproc(void);
extern unsigned char* gdx_rom_buffer;
extern size_t gdx_rom_size;
extern void* gGfxPool;
extern int gGameMode;
}

extern "C" int gdx_vr_fast3d_is_eye_active(void) {
    return (gFastVrEyeActive && gFastVrSection != FastVrSection::Off) ? 1 : 0;
}

extern "C" int gdx_vr_fast3d_get_eye_matrix(float* out16) {
    if (!gFastVrEyeActive || out16==nullptr) return 0;
    const std::array<float,16>* matrix=nullptr;
    if (gFastVrSection==FastVrSection::Sky) matrix=&gFastVrSkyMatrix;
    else if (gFastVrSection==FastVrSection::World) matrix=&gFastVrWorldMatrix;
    else return 0;
    std::memcpy(out16,matrix->data(),sizeof(float)*16);
    return 1;
}

extern "C" void gdx_vr_fast3d_noop_tag(uintptr_t tag) {
    if (!gFastVrEyeActive) return;
    if (tag==kRaceSkyMarker) gFastVrSection=FastVrSection::Sky;
    else if (tag==kRaceWorldMarker) gFastVrSection=FastVrSection::World;
    else return;
    static bool logged[2][2]={{false,false},{false,false}};
    const int eye=(gFastVrEyeIndex>=0 && gFastVrEyeIndex<2) ? gFastVrEyeIndex : 0;
    const int section=(gFastVrSection==FastVrSection::Sky) ? 0 : 1;
    if (!logged[eye][section]) {
        logged[eye][section]=true;
        __android_log_print(ANDROID_LOG_INFO,kTag,"Fast3D eye %d section -> %s",
                            eye,section==0 ? "SKY(rotation-only)" : "WORLD(6DoF+IPD)");
    }
}

extern "C" int gdx_vr_host_apply_center_camera(GdxVrCameraIo* camera) {
    if (camera==nullptr || camera->id!=0 || !gEyesValid || !IsRaceVrMode(GET_MODE(gGameMode))) {
        gVrCenterPoseValid=false;
        gRaceTrackingActive=false;
        gCourseCullState.valid=false;
        gVrCameraBasisCache.generation=0;
        ++gVrMatrixGeneration;
        gXrProjectionCache[0].valid=gXrProjectionCache[1].valid=false;
        gDioramaSortState.valid=false;
        gDioramaSortState.dirty=true;
        return 0;
    }

    const XrQuaternionf headOrientation=HeadOrientationFromEyes();
    const XrVector3f headCenter{
        0.5f*(gEyes[0].xrPose.position.x+gEyes[1].xrPose.position.x),
        0.5f*(gEyes[0].xrPose.position.y+gEyes[1].xrPose.position.y),
        0.5f*(gEyes[0].xrPose.position.z+gEyes[1].xrPose.position.z)};
    if (!gRaceTrackingActive) {
        gRaceTrackingActive=true;
        gRaceBaseOrientation=headOrientation;
        gRaceBasePosition=headCenter;
        __android_log_print(ANDROID_LOG_INFO,kTag,
                            "race render-only VR tracking recentered (head=%.1f u/m stereo=%.1f u/m)",
                            kHeadMetersToGameUnits,kStereoMetersToGameUnits);
    }

    // Store the untouched F-Zero camera. Unlike the previous implementation, NEVER write the HMD
    // transform back into gCameras: the game simulation, background setup and culling all continue
    // from their native camera. Head rotation/translation and IPD are applied only in Fast3D.
    gVrCenterHeadOrientation=headOrientation;
    gVrCenterHeadPosition=headCenter;
    gVrCenterPose={};
    gVrCenterPose.eyeX=camera->eyeX; gVrCenterPose.eyeY=camera->eyeY; gVrCenterPose.eyeZ=camera->eyeZ;
    gVrCenterPose.atX=camera->atX; gVrCenterPose.atY=camera->atY; gVrCenterPose.atZ=camera->atZ;
    gVrCenterPose.upX=camera->upX; gVrCenterPose.upY=camera->upY; gVrCenterPose.upZ=camera->upZ;
    gVrCenterPose.fov=camera->fov;
    gVrCenterPose.nearZ=camera->nearZ; gVrCenterPose.farZ=camera->farZ;
    gVrCenterPose.fovScaleX=camera->fovScaleX; gVrCenterPose.fovScaleY=camera->fovScaleY;
    gVrCenterPose.frustrumCenterX=camera->frustrumCenterX;
    gVrCenterPose.frustrumCenterY=camera->frustrumCenterY;
    gVrCenterPose.numPlayers=camera->numPlayers;
    gVrCenterPose.resolvedFov=camera->fov;
    gVrCenterPose.fovIsResolved=1;
    gVrCenterPose.id=camera->id;
    gVrCenterPose.valid=1;
    gVrCenterPoseValid=true;
    ++gVrMatrixGeneration;
    RebuildCourseCullState();
    gDioramaSortState.dirty=true;

    // Returning 0 deliberately tells camera_quest.c NOT to mutate the game camera.
    return 0;
}

extern "C" int gdx_vr_host_course_chunk_query(float x,float y,float z,float depth,float farDistance,float* sortDepth) {
    if (sortDepth!=nullptr) *sortDepth=depth;

    // Diorama keeps every chunk already streamed by Course_SegmentsInit (the proven no-hole path),
    // but its painter sorting must use the ACTUAL miniature eye rather than the native chase-camera
    // depth. Build the exact game->eye matrix used by the diorama renderer once per pose update and
    // convert eye-space metres back to F-Zero-like game units so the stock grouping thresholds keep
    // their intended scale. OpenXR looks down -Z, hence the sign flip.
    if (gDioramaEnabled) {
        if (gDioramaSortState.dirty) {
            gDioramaSortState.valid=BuildDioramaGameToEye(0,gDioramaSortState.gameToEye);
            gDioramaSortState.dirty=false;
        }
        if (gDioramaSortState.valid && sortDepth!=nullptr) {
            const auto& m=gDioramaSortState.gameToEye;
            const float eyeZ=x*m[2]+y*m[6]+z*m[10]+m[14];
            *sortDepth=(-eyeZ)*kDioramaScaleGameUnitsPerMeter;
        }
        return 1;
    }

    // Non-diorama fallback used only while tracking/camera state is not yet ready.
    if (!gCourseCullState.valid) {
        return std::fabs(depth) <= std::max(farDistance,1.0f) ? 1 : 0;
    }

    const Vec3 toChunk=Sub(Vec3{x,y,z},gCourseCullState.eye);
    const float forward=Dot(toChunk,gCourseCullState.forward);
    const float side=std::fabs(Dot(toChunk,gCourseCullState.right));
    const float vertical=std::fabs(Dot(toChunk,gCourseCullState.up));
    const float distance=Length(toChunk);

    // SegmentChunk positions are chunk centres, not bounding boxes. A generous world-space margin
    // keeps neighbouring geometry alive during fast head turns/leans while still rejecting the vast
    // majority of the old 360-degree set. The cached FOV includes both physical Quest eyes and the
    // same 25% safety margin used before this optimization.
    constexpr float kChunkMargin=700.0f;
    constexpr float kFrustumSafety=1.25f;
    const float farLimit=std::max(farDistance,1000.0f)+kChunkMargin;
    if (distance>farLimit || forward < -kChunkMargin) return 0;

    const float projected=std::max(forward,0.0f);
    if (side > projected*gCourseCullState.tanX*kFrustumSafety+kChunkMargin) return 0;
    if (vertical > projected*gCourseCullState.tanY*kFrustumSafety+kChunkMargin) return 0;
    return 1;
}

extern "C" int gdx_vr_host_bootstrap(const char* filesDir) {
    Dl_info questDlInfo{};
    if (dladdr(reinterpret_cast<const void*>(&gdx_vr_host_bootstrap), &questDlInfo) != 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "module base=%p bootstrap=%p image=%s",
                            questDlInfo.dli_fbase,
                            reinterpret_cast<const void*>(&gdx_vr_host_bootstrap),
                            questDlInfo.dli_fname ? questDlInfo.dli_fname : "?");
    }
    if (gBooted) return 1;
    if (filesDir==nullptr || filesDir[0]=='\0') return 0;
    gFilesDir=filesDir;
    gDioramaEnabled=LoadStartDioramaSetting(filesDir);
    __android_log_print(ANDROID_LOG_INFO,kTag,"Launcher VR settings: start diorama=%d",gDioramaEnabled ? 1 : 0);
    setenv("SHIP_HOME",filesDir,1);
    gdx_port_log_tap = &QuestPortLogTap;
    gdx_dev_gates_init_env();
    // Keep G-Diffuser's deep per-phase telemetry disabled on standalone Quest. It performs many
    // clock reads, vector updates and audio-thread synchronization points every frame. The native
    // OpenXR loop already reports FPS + average RenderFrame time every two seconds, which measures
    // the real 72 Hz budget without perturbing the game/render hot paths.
    gdx_dev_gate_force(GDX_GATE_PERF, 0);

    gContext=Ship::Context::CreateUninitializedInstance("F-Zero X VR","fzerox-vr","gdiffuser.cfg.json");
    auto& ctx=gContext;
    if (!ctx || !ctx->InitConfiguration() || !ctx->InitConsoleVariables()) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"libultraship context/config init failed");
        return 0;
    }
    auto cvars=ctx->GetConsoleVariables();
    if (cvars) {
        cvars->SetInteger("gEnhancements.Graphics.Interpolation",0);
        cvars->SetInteger("gEnhancements.Graphics.Widescreen",1);
        cvars->SetInteger("gEnhancements.Graphics.MSAA",1);
        // Quest standalone: use G-Diffuser's accurate cxd4 LLE mixer. The HLE path was producing
        // valid-sized AI buffers containing only zero PCM on Quest, so performance does not matter
        // until correctness is restored. We can profile/optimise the LLE path after audio works.
        cvars->SetInteger("gEnhancements.Audio.LLE",1);
    }

    auto archives=FindArchives(filesDir);
    const std::string builtinResources=GdxQuestInstallBuiltinResources(filesDir);
    if (!builtinResources.empty()) {
        // Mount first so libultraship's own shader template is available even when the user only
        // supplied a legal raw ROM and no desktop gdiffuser.o2r archive.
        archives.insert(archives.begin(),builtinResources);
        __android_log_print(ANDROID_LOG_INFO,kTag,"mounted built-in Fast3D resources from %s",builtinResources.c_str());
    } else {
        __android_log_print(ANDROID_LOG_WARN,kTag,"failed to install built-in Fast3D resources");
    }
    for (const auto& archive : archives) {
        __android_log_print(ANDROID_LOG_INFO,kTag,"resource archive: %s",archive.c_str());
    }
    if (!ctx->InitResourceManager(archives,{},1,true)) {
        __android_log_print(ANDROID_LOG_WARN,kTag,"resource manager init failed; raw-ROM fallback will be used");
    }

    // Fast3D constructs its GUI during Window::Init(). ConsoleWindow registers commands there,
    // so libultraship's Console must exist first (same ordering as Context::Init()).
    if (!ctx->InitConsole()) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"libultraship console init failed");
        return 0;
    }

    gWindow=std::make_shared<QuestFast3dWindow>(2048,2048);
    if (!ctx->InitWindow(gWindow)) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"Quest Fast3D window init failed");
        return 0;
    }
    Fast::Interpreter::SetPortAfterClearHook(&QuestAfterFast3dClear);
    // Quest NativeActivity: use SDL's native AAudio driver explicitly. libultraship persists a
    // failed SDL initialization by switching Window.AudioBackend to "null"; once that happens,
    // every later boot silently discards PCM even though the dedicated producer thread is ACTIVE.
    // Force the known-good Android path on every standalone Quest boot before InitAudio().
    ::setenv("SDL_AUDIODRIVER","aaudio",1);
    if (auto config=ctx->GetConfig()) {
        config->SetString("Window.AudioBackend","sdl");
        config->Save();
    }
    Ship::AudioSettings audioSettings{};
    audioSettings.DesiredBuffered = 4096;
    const bool audioInitOk=ctx->InitAudio(audioSettings);
    __android_log_print(audioInitOk ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,kTag,
                        "audio init=%s backend=%s desired=%d buffered=%d",
                        audioInitOk ? "OK" : "FAILED",AudioPlayerBackendName(),
                        AudioPlayerGetDesiredBuffered(),AudioPlayerBuffered());
    ctx->InitEventSystem();

    const std::string rom=FindRom(filesDir);
    if (rom.empty()) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,
                            "ROM missing. Put baserom.us.rev0.z64 or fzerox.z64 in %s",filesDir);
        return 0;
    }
    std::string arg0="fzerox-vr";
    std::string arg1=rom;
    char* argv[]={arg0.data(),arg1.data()};
    // Quest correctness first: keep F-Zero's own cooperative Audio_ThreadEntry as the producer.
    // The standalone std::thread producer can outrun / desynchronise the game's audio task state
    // during boot and, in the failing headset build, never reached osAiSetNextBuffer at all.
    // AAudio remains the actual device backend; only task PRODUCTION returns to the known game path.
    std::string audioLegacyArg="--no-audio-thread";
    char* audioArgv[]={arg0.data(),arg1.data(),audioLegacyArg.data()};
    gdx_audio_thread_start(3,audioArgv);
    __android_log_print(ANDROID_LOG_INFO,kTag,"audio producer: %s",
                        gdx_audio_thread_active() ? "dedicated thread" : "legacy F-Zero fiber");
    gdx_init_rom(2,argv,archives.empty()?0:1);
    if (gdx_rom_buffer==nullptr) {
        __android_log_print(ANDROID_LOG_ERROR,kTag,"ROM rejected by G-Diffuser loader: %s",rom.c_str());
        return 0;
    }

    if (ctx->GetResourceManager()) {
        GDiffuser::RegisterResourceFactories(ctx->GetResourceManager()->GetResourceLoader());
        auto probe = ctx->GetResourceManager()->LoadResource("common_assets_compressed/D_F264110");
        __android_log_print(ANDROID_LOG_INFO,kTag,"fzerox.o2r asset probe: %s",probe ? "OK" : "MISS");
    }
    GDiffuser_LoadAllAssets();
    gdx_sched_init();
    gdx_rdram_init();

    // Match G-Diffuser desktop's 64-bit address-registration window before bootproc(). Audio command
    // lists retain N64-sized/low32 address tokens; without these host ranges the Quest build can
    // successfully DMA buffers yet resolve sequence/sample pointers to zeros.
    gdx_register_host_range(gdx_rom_buffer,gdx_rom_size);
    gdx_register_main_module_range();
    {
        static const uint32_t kAudioBlobBases[3]={0x00524D60u,0x00527AF0u,0x00528730u};
        static const char* kAudioBlobNames[3]={"audio_bank","audio_seq","audio_table"};
        for (int i=0;i<3;++i) {
            void* payload=nullptr;
            uint32_t payloadSize=0;
            if (GdxSegmentSourcePreload(kAudioBlobBases[i]) &&
                GdxSegmentSourcePayload(kAudioBlobBases[i],&payload,&payloadSize) &&
                payload!=nullptr && payloadSize!=0) {
                gdx_register_host_range(payload,payloadSize);
                __android_log_print(ANDROID_LOG_INFO,kTag,
                                    "audio blob registered: %s base=%08x payload=%p low32=%08x size=%u",
                                    kAudioBlobNames[i],kAudioBlobBases[i],payload,
                                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(payload)&0xFFFFFFFFu),
                                    payloadSize);
            } else {
                __android_log_print(ANDROID_LOG_WARN,kTag,
                                    "audio blob preload miss: %s base=%08x (raw ROM fallback)",
                                    kAudioBlobNames[i],kAudioBlobBases[i]);
            }
        }
    }
    gdx_boot_warm_asset_segments();
    __android_log_print(ANDROID_LOG_INFO,kTag,"raw-ROM fallback reads after warmup: %u",
                        gdx_segment_source_fallback_total());
    bootproc();
    gBooted=true;
    __android_log_print(ANDROID_LOG_INFO,kTag,"F-Zero X decomp booted from %s",rom.c_str());
    return 1;
}

extern "C" void gdx_vr_host_set_stereo_views(const HostEye* eyes,int count) {
    if (eyes==nullptr || count<2) {
        gEyesValid=false;
        gCourseCullState.valid=false;
        gVrCameraBasisCache.generation=0;
        ++gVrMatrixGeneration;
        gXrProjectionCache[0].valid=gXrProjectionCache[1].valid=false;
        gDioramaSortState.valid=false;
        gDioramaSortState.dirty=true;
        return;
    }
    gEyes[0]=eyes[0]; gEyes[1]=eyes[1];
    gEyesValid=true;
    gXrProjectionCache[0].valid=gXrProjectionCache[1].valid=false;

    // xrLocateViews runs at the compositor's predicted display time (72 Hz on the current Quest
    // session). Publish the center-head pose HERE, not only from the 60 Hz game camera callback.
    // Rendering and XrCompositionLayerProjectionView now consume the same predicted pose, removing
    // the 60->72 Hz stair-step that showed up as a small shake when rotating the head.
    gVrCenterHeadOrientation=HeadOrientationFromEyes();
    gVrCenterHeadPosition={
        0.5f*(gEyes[0].xrPose.position.x+gEyes[1].xrPose.position.x),
        0.5f*(gEyes[0].xrPose.position.y+gEyes[1].xrPose.position.y),
        0.5f*(gEyes[0].xrPose.position.z+gEyes[1].xrPose.position.z)};
    ++gVrMatrixGeneration;
    gDioramaSortState.dirty=true;

    // Keep Fast3D latched to the internal world resolution between game ticks. Publishing a new
    // 72 Hz OpenXR pose used to bounce the renderer back to full 2800x2933 before every 60 Hz task,
    // forcing framebuffer reconfiguration just before it was immediately reduced again.
    if (gWindow) {
        if (gFlatUiActive) gWindow->SetEyeDimensions(2048,1536);
        else gWindow->SetEyeDimensions(WorldRenderDim(gEyes[0].width),WorldRenderDim(gEyes[0].height));
    }
    if (!gHeadBaseValid) {
        gHeadBaseOrientation=HeadOrientationFromEyes();
        gHeadBasePosition={
            0.5f*(gEyes[0].xrPose.position.x+gEyes[1].xrPose.position.x),
            0.5f*(gEyes[0].xrPose.position.y+gEyes[1].xrPose.position.y),
            0.5f*(gEyes[0].xrPose.position.z+gEyes[1].xrPose.position.z)};
        gHeadBaseValid=true;
    }
}

extern "C" void gdx_vr_host_tick(const void* inputRaw) {
    if (!gBooted) return;
    const bool inRace=IsRaceVrMode(GET_MODE(gGameMode));
    const GdxVrHostInput* input=static_cast<const GdxVrHostInput*>(inputRaw);
    const bool dioramaButtonDown=input!=nullptr && (input->n64Buttons & 0x0800u)!=0; // right stick click -> D-Pad Up
    if (inRace && dioramaButtonDown && !gDioramaToggleWasDown) {
        gDioramaEnabled=!gDioramaEnabled;
        gDioramaAnchorValid=false;
        gCourseCullState.valid=false;
        gDioramaSortState.valid=false;
        gDioramaSortState.dirty=true;
        __android_log_print(ANDROID_LOG_INFO,kTag,"Diorama camera %s",gDioramaEnabled ? "ENABLED" : "DISABLED");
    }
    gDioramaToggleWasDown=dioramaButtonDown;
    if (!inRace) {
        gRaceTrackingActive=false;
        gVrCenterPoseValid=false;
        gCourseCullState.valid=false;
        gDioramaAnchorValid=false;
        gDioramaSortState.valid=false;
        gDioramaSortState.dirty=true;
    }
    // Standalone Quest has no live Dev Tools UI, so refreshing every CVar-backed diagnostic gate
    // on every ~60 Hz game tick is pure overhead. Gates are initialized once during bootstrap.
    gdx_controller_poll();
    gdx_fixed_aspect_tick();
    gdx_vi_tick();
    gdx_audio_thread_notify_frame();
    gdx_sched_drain_deferred_wakes();
    gdx_dispatch();
}

extern "C" int gdx_vr_host_render_converted(void* interpreterRaw,void* convertedRaw,int taskVariant) {
    if (!gBooted || !gEyesValid || interpreterRaw==nullptr || convertedRaw==nullptr || !gWindow) return 0;
    auto leftInterp=gWindow->InterpreterShared();
    auto rightInterp=leftInterp; // one prepared Fast3D interpreter, deterministically replayed twice
    if (!leftInterp) return 0;
    auto* commands=reinterpret_cast<Gfx*>(convertedRaw);
    auto* fastCommands=reinterpret_cast<Fast::F3DGfx*>(convertedRaw);

    // 2D boot logos, menus and several transitions submit perfectly valid display lists before
    // F-Zero has published a 3D camera pose. The old Quest hook returned 0 in that case, so the
    // bridge rendered one desktop pass while BOTH OpenXR eye caches stayed invalid. Always replay
    // the converted list into the eye caches; only override the camera matrix when a valid 3D
    // camera slot actually exists.
    size_t offset=0,stride=0,count=0;
    gdx_gfxpool_camera_mtx_layout(&offset,&stride,&count);
    GdxCameraPose live{};
    unsigned char* cameraMtx=nullptr;
    unsigned char original[64]{};
    const bool haveCamera = gGfxPool!=nullptr &&
        gdx_camera_pose_read(0,&live) && live.valid && live.id>=0 &&
        static_cast<size_t>(live.id)<count && stride>=64;
    if (haveCamera) {
        cameraMtx=static_cast<unsigned char*>(gGfxPool)+offset+stride*static_cast<size_t>(live.id);
        std::memcpy(original,cameraMtx,64);
    }

    const int gameMode = GET_MODE(gGameMode);
    // F-Zero's menus are authored as one 2D screen, not as world geometry. Re-projecting that
    // screen with a different OpenXR eye matrix creates two non-fusing copies in the headset.
    // Render both persistent interpreters to keep their RDP/TMEM state synchronized, but publish
    // ONLY the left pass to both eyes for flat UI modes (and boot frames that have no camera yet).
    const bool flatUi = IsFlatMenuMode(gameMode) || !haveCamera;
    gFlatUiActive = flatUi;

    // In races the generated race_quest.c inserts a VRHD no-op immediately before Menus_Draw().
    // Split at that exact command: everything before it is world geometry and receives the real
    // per-eye camera; everything after it is flat HUD/minimap/speed/position UI and is rendered
    // once to a transparent 4:3 texture for an OpenXR quad layer.
    const size_t hudMarker = (!flatUi && haveCamera) ? FindRaceHudMarker(fastCommands) : static_cast<size_t>(-1);
    const bool splitRaceHud = hudMarker != static_cast<size_t>(-1);
    gRaceHudActive = splitRaceHud;
    if (splitRaceHud) {
        static bool sHudSplitLogged=false;
        if (!sHudSplitLogged) {
            sHudSplitLogged=true;
            const float dx=gEyes[1].xrPose.position.x-gEyes[0].xrPose.position.x;
            const float dy=gEyes[1].xrPose.position.y-gEyes[0].xrPose.position.y;
            const float dz=gEyes[1].xrPose.position.z-gEyes[0].xrPose.position.z;
            const float ipd=std::sqrt(dx*dx+dy*dy+dz*dz);
            __android_log_print(ANDROID_LOG_INFO,kTag,
                                "race VRHD split command=%zu physicalIPD=%.3fm virtualBaseline=%.3f game units",
                                hudMarker,ipd,ipd*kStereoMetersToGameUnits);
        }

        // VRHD itself is a no-op delimiter. Turn that one command into G_ENDDL while replaying the
        // world instead of copying every pre-HUD command into a temporary vector each graphics task.
        // Restore it immediately after the stereo world passes so the converted task remains intact.
        const Fast::F3DGfx savedHudMarker=fastCommands[hudMarker];
        fastCommands[hudMarker].words.w0=static_cast<uintptr_t>(0xDFu)<<24; // F3DEX2 G_ENDDL
        fastCommands[hudMarker].words.w1=0;
        Fast::F3DGfx* hudCommands=fastCommands+hudMarker+1;

        gHudValid=false;

        // Pass A: true stereo world through ONE task-prepared interpreter. Snapshot the logical
        // Fast3D state before eye 0, restore it before eye 1, and finally restore eye 0's end state
        // so the task advances exactly once. This removes left->right leakage of geometry_mode,
        // TMEM/RDP state, segment pointers and microcode variant while keeping one shared texture cache.
        const FastReplayState worldStart=CaptureReplayState(leftInterp);
        FastReplayState worldEnd=worldStart;
        for (int eye=0;eye<2;++eye) {
            auto interp=leftInterp;
            RestoreReplayState(interp,worldStart);
            gWindow->SetRendererUCode(ucode_f3dex2);
            interp->SetF3dex2Variant(static_cast<Fast::F3dex2Variant>(taskVariant));
            gWindow->SetEyeDimensions(WorldRenderDim(gEyes[eye].width),WorldRenderDim(gEyes[eye].height));
            // Keep the game's center-head packed matrix untouched. Fast3D receives the eye-specific
            // combined view-projection through gdx_vr_fast3d_get_eye_matrix() at MP-matrix update time,
            // exactly like mario64VRStandalone's gVrEyeVP path.
            const bool fastVrActive=ActivateFastVrEye(eye);
            static bool sFastVrLogged=false;
            if (!sFastVrLogged) {
                sFastVrLogged=true;
                __android_log_print(fastVrActive ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,kTag,
                                    "Fast3D native per-eye VP override: %s (head=%.1f u/m stereo=%.1f u/m)",
                                    fastVrActive ? "ACTIVE" : "FAILED",
                                    kHeadMetersToGameUnits,kStereoMetersToGameUnits);
            }
            Fast::GfxSetInstance(interp);
            gHudMatteClearMode=0;
            interp->StartFrame();
            interp->Run(commands,{});
            DeactivateFastVrEye();
            if (eye==0) worldEnd=CaptureReplayState(interp);

            const GLuint source=static_cast<GLuint>(interp->mGfxFrameBuffer);
            const uint32_t cacheW=WorldRenderDim(gEyes[eye].width);
            const uint32_t cacheH=WorldRenderDim(gEyes[eye].height);
            EnsureCache(eye,cacheW,cacheH);
            bool copied=false;
            if (source) {
                copied=CopyTextureFlippedY(source,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                           gCacheTextures[eye],cacheW,cacheH);
            } else {
                copied=BlitFramebufferFlippedY(0,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                               gCacheTextures[eye],cacheW,cacheH);
            }
            if (copied) {
                gCacheValid[eye]=true;
                gCacheRenderPose[eye]=gEyes[eye].xrPose;
                gCacheRenderFov[eye]=gEyes[eye].xrFov;
                gCacheRenderViewValid[eye]=true;
            }
            interp->EndFrame();
        }
        fastCommands[hudMarker]=savedHudMarker;
        RestoreReplayState(leftInterp,worldEnd);

        // Pass B: reconstruct a real transparent HUD from black/white mattes. A nominally
        // transparent Fast3D clear is not sufficient on Quest: later RDP/Fast3D state can make the
        // offscreen target opaque again, which is why the wrist quad showed a solid black panel.
        // Replaying the HUD from the exact same logical state over black and white lets us recover
        // alpha independently of that framebuffer behaviour. CompositeHudMattes also performs the
        // missing Fast3D offscreen Y flip, fixing the upside-down wrist HUD in the same pass.
        std::memcpy(cameraMtx,original,64);
        EnsureHudTexture(kHudWidth,kHudHeight);
        const FastReplayState hudStart=worldEnd;
        FastReplayState hudEnd=hudStart;
        auto interp=leftInterp;
        gWindow->SetRendererUCode(ucode_f3dex2);
        gWindow->SetEyeDimensions(kHudWidth,kHudHeight);
        Fast::GfxSetInstance(interp);

        bool matteCopiesOk=true;
        for (int matteIndex=0;matteIndex<2;++matteIndex) {
            RestoreReplayState(interp,hudStart);
            interp->SetF3dex2Variant(hudStart.variant);
            gHudMatteClearMode=(matteIndex==0) ? 1 : 2;
            interp->StartFrame();
            interp->Run(reinterpret_cast<Gfx*>(hudCommands),{});
            if (matteIndex==0) hudEnd=CaptureReplayState(interp);

            const GLuint hudSource=static_cast<GLuint>(interp->mGfxFrameBuffer);
            bool copied=false;
            if (hudSource) {
                copied=CopyTexture(hudSource,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                   gHudMatteTextures[matteIndex],kHudWidth,kHudHeight);
            } else {
                copied=BlitFramebuffer(0,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                       gHudMatteTextures[matteIndex],kHudWidth,kHudHeight);
            }
            matteCopiesOk=matteCopiesOk && copied;
            interp->EndFrame();
        }
        gHudMatteClearMode=0;
        gHudValid=matteCopiesOk && CompositeHudMattes(kHudWidth,kHudHeight);
        RestoreReplayState(leftInterp,hudEnd);
        gWindow->SetEyeDimensions(WorldRenderDim(gEyes[0].width),WorldRenderDim(gEyes[0].height));
        std::memcpy(cameraMtx,original,64);
        Fast::GfxSetInstance(leftInterp);
        return 1;
    }
    gRaceHudActive=false;

    // Flat UI is authored for F-Zero X's native 320x240 (4:3) frame. Rendering it at the
    // near-square Quest eye size (2800x2933) changes the viewport/aspect before the compositor
    // ever sees it, which is why the menu edges look cropped. Use a fixed 4:3 Fast3D target for
    // menus; gameplay immediately switches back to the real per-eye dimensions.
    if (flatUi) {
        gWindow->SetEyeDimensions(2048, 1536);
    } else {
        gWindow->SetEyeDimensions(WorldRenderDim(gEyes[0].width), WorldRenderDim(gEyes[0].height));
    }

    const FastReplayState taskStart=CaptureReplayState(leftInterp);
    FastReplayState taskEnd=taskStart;
    const int replayCount=flatUi ? 1 : 2;
    for (int eye=0;eye<replayCount;++eye) {
        auto interp=leftInterp;
        RestoreReplayState(interp,taskStart);
        gWindow->SetRendererUCode(ucode_f3dex2);
        interp->SetF3dex2Variant(static_cast<Fast::F3dex2Variant>(taskVariant));
        DeactivateFastVrEye();
        if (haveCamera) {
            // The decomp's packed camera matrix remains the center-head/culling camera. For any 3D
            // non-HUD replay, Fast3D gets the native per-eye VP directly at vertex transform time.
            std::memcpy(cameraMtx,original,64);
            if (!flatUi) ActivateFastVrEye(eye);
        }

        // Fast3D command handlers use a process-global active Interpreter pointer. Select the
        // eye-local persistent interpreter before every replay so RSP/RDP/TMEM/texture-cache state
        // never leaks from the left eye into the right eye (or vice versa).
        Fast::GfxSetInstance(interp);
        interp->StartFrame();
        interp->Run(commands,{});
        DeactivateFastVrEye();
        if (eye==0) taskEnd=CaptureReplayState(interp);
        const GLuint source=static_cast<GLuint>(interp->mGfxFrameBuffer);

        // Flat UI is submitted as one 2048x1536 OpenXR quad visible to both eyes. Keep only one
        // 4:3 cache at that exact resolution; the previous path stretched it to a 2800x2933 eye
        // cache and then shrank it back to 2048x1536 at submission time.
        if (flatUi && eye==0) {
            EnsureCache(0,2048,1536);
            bool copied = false;
            if (source) {
                copied = CopyTextureFlippedY(source,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                             gCacheTextures[0],2048,1536);
            } else {
                copied = BlitFramebufferFlippedY(0,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                                 gCacheTextures[0],2048,1536);
            }
            gCacheValid[0]=copied;
            gCacheValid[1]=false;
            gCacheRenderViewValid[0]=false;
            gCacheRenderViewValid[1]=false;
        } else if (!flatUi) {
            const uint32_t cacheW=WorldRenderDim(gEyes[eye].width);
            const uint32_t cacheH=WorldRenderDim(gEyes[eye].height);
            EnsureCache(eye,cacheW,cacheH);
            bool copied = false;
            if (source) {
                // Keep the persistent stereo cache at Fast3D's internal resolution. The single
                // upscale happens later when gdx_vr_host_render_eye writes the acquired XR image.
                copied = CopyTextureFlippedY(source,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                             gCacheTextures[eye],cacheW,cacheH);
            } else {
                copied = BlitFramebufferFlippedY(0,interp->mCurDimensions.width,interp->mCurDimensions.height,
                                                 gCacheTextures[eye],cacheW,cacheH);
            }
            if (copied) {
                gCacheValid[eye]=true;
                gCacheRenderPose[eye]=gEyes[eye].xrPose;
                gCacheRenderFov[eye]=gEyes[eye].xrFov;
                gCacheRenderViewValid[eye]=true;
            }
        }
        interp->EndFrame();
    }
    RestoreReplayState(leftInterp,taskEnd);
    if (haveCamera) std::memcpy(cameraMtx,original,64);
    Fast::GfxSetInstance(leftInterp);
    return 1;
}

extern "C" void gdx_vr_host_mirror_texture_delete(const void*) {
    // Stereo now replays one shared Fast3D interpreter, so the primary invalidation already covers
    // both eyes. Kept as an ABI no-op for the generated bridge hooks.
}

extern "C" void gdx_vr_host_mirror_texture_clear(void) {
    // Same as above: there is no independent right-eye texture cache anymore.
}

extern "C" int gdx_vr_host_is_flat_ui(void) {
    return gFlatUiActive ? 1 : 0;
}

extern "C" int gdx_vr_host_is_race_hud(void) {
    return (gRaceHudActive && gHudValid) ? 1 : 0;
}

extern "C" int gdx_vr_host_is_diorama(void) {
    return gDioramaEnabled ? 1 : 0;
}

extern "C" int gdx_vr_host_render_hud(const HostEye* eye) {
    if (!gBooted || !gRaceHudActive || !gHudValid || eye==nullptr || !eye->colorTexture || !gHudTexture) return 0;
    return CopyTexture(gHudTexture,gHudTextureWidth,gHudTextureHeight,
                       eye->colorTexture,eye->width,eye->height) ? 1 : 0;
}

extern "C" int gdx_vr_host_render_eye(const HostEye* eye) {
    if (!gBooted || eye==nullptr || eye->eye<0 || eye->eye>1 || !gCacheValid[eye->eye] || !eye->colorTexture) return 0;
    return CopyTexture(gCacheTextures[eye->eye],gCacheWidth[eye->eye],gCacheHeight[eye->eye],
                       eye->colorTexture,eye->width,eye->height) ? 1 : 0;
}

extern "C" int gdx_vr_host_get_cached_eye_view(int eye,XrPosef* pose,XrFovf* fov) {
    if (eye<0 || eye>1 || pose==nullptr || fov==nullptr || !gCacheValid[eye] || !gCacheRenderViewValid[eye]) return 0;
    *pose=gCacheRenderPose[eye];
    *fov=gCacheRenderFov[eye];
    return 1;
}

extern "C" void gdx_vr_host_shutdown(void) {
    gdx_audio_thread_stop();
    Fast::Interpreter::SetPortAfterClearHook(nullptr);
    if (gCacheTextures[0]) glDeleteTextures(1,&gCacheTextures[0]);
    if (gCacheTextures[1]) glDeleteTextures(1,&gCacheTextures[1]);
    if (gHudTexture) glDeleteTextures(1,&gHudTexture);
    if (gHudMatteTextures[0]) glDeleteTextures(2,gHudMatteTextures);
    if (gHudCompositeProgram) glDeleteProgram(gHudCompositeProgram);
    if (gHudCompositeVao) glDeleteVertexArrays(1,&gHudCompositeVao);
    if (gReadFbo) glDeleteFramebuffers(1,&gReadFbo);
    if (gDrawFbo) glDeleteFramebuffers(1,&gDrawFbo);
    gCacheTextures[0]=gCacheTextures[1]=0;
    gCacheValid[0]=gCacheValid[1]=false;
    gCacheRenderViewValid[0]=gCacheRenderViewValid[1]=false;
    gHudTexture=0;
    gHudMatteTextures[0]=gHudMatteTextures[1]=0;
    gHudCompositeProgram=0;
    gHudCompositeVao=0;
    gHudBlackSampler=gHudWhiteSampler=-1;
    gHudTextureWidth=gHudTextureHeight=0;
    gHudValid=false;
    gRaceHudActive=false;
    gHudMatteClearMode=0;
    gReadFbo=gDrawFbo=0;
    gWindow.reset();
    // The launcher can remain alive after the immersive NativeActivity exits. Release the complete
    // libultraship Context here so AAudio, ResourceManager, EventSystem and window singletons do not
    // survive as a "ghost" VR runtime and a later launch starts from a genuinely clean process state.
    gContext.reset();
    gBooted=false; gEyesValid=false; gHeadBaseValid=false; gFlatUiActive=true;
    gRaceTrackingActive=false; gVrCenterPoseValid=false; gCourseCullState.valid=false;
    gDioramaSortState.valid=false; gDioramaSortState.dirty=true;
    gVrCameraBasisCache.generation=0; gVrMatrixGeneration=1;
    gXrProjectionCache[0].valid=gXrProjectionCache[1].valid=false;
}
