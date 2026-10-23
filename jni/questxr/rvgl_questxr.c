/*
 * RVGL Quest VR host (stage 1: big virtual screen).
 *
 * Compiled into libSDL2.so. The closed RVGL engine gets every GL function via
 * SDL_GL_GetProcAddress and presents via SDL_GL_SwapWindow, so:
 *  - framebuffer 0 is redirected to an offscreen FBO (QXR_GAME_W x QXR_GAME_H),
 *  - each swap blits that FBO into an OpenXR swapchain shown as a quad layer,
 *  - Touch controllers drive an SDL virtual gamepad (remappable in RVGL's menu).
 * OpenXR setup follows QuakeQuest's TBXR layer (loader init, session state
 * handling, action-set input), reduced to what a flat screen needs.
 */
#include "rvgl_questxr.h"

#include <android/log.h>
#include <jni.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "SDL.h"
#include "SDL_system.h"

#define TAG "RVGLQuest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#define MAX_IMAGES 8

enum { HAND_L = 0, HAND_R = 1 };

/* virtual pad layout (matches the mapping string below) */
enum { VB_A, VB_B, VB_X, VB_Y, VB_BACK, VB_GUIDE, VB_START, VB_LSTICK, VB_RSTICK,
       VB_LSHOULDER, VB_RSHOULDER, VB_COUNT };
enum { VA_LX, VA_LY, VA_RX, VA_RY, VA_LT, VA_RT, VA_COUNT };

static const char *kPadMapping =
    "00000000000000000000000000007601,Quest Touch,"
    "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
    "leftshoulder:b9,rightshoulder:b10,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:+a4,righttrigger:+a5,";

static struct {
    int inited, failed;
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace localSpace, viewSpace;
    XrSessionState state;
    int running;

    XrSwapchain swapchain;
    uint32_t imgCount;
    XrSwapchainImageOpenGLESKHR imgs[MAX_IMAGES];
    GLuint imgFbo[MAX_IMAGES];
    int64_t format;
    int srgbWriteControl;

    GLuint fbo, colorRb, depthRb;

    XrPosef quadPose;
    int recenterPending;
    float dist, width, height_offset;

    XrActionSet set;
    XrAction aTrig, aGrip, aStickX, aStickY, aClick, aA, aB, aX, aY, aMenu;
    XrPath hand[2];
    int gripDown[2];
    Uint32 bothClickStart;
    int bothClickFired;

    int padAttached;
    SDL_Joystick *pad;

    Uint32 statT0;
    int statFrames;
} q;

/* ------------------------------------------------------------------ config */

static void load_config(void)
{
    q.dist = 3.0f;          /* metres */
    q.width = 3.4f;         /* metres; keeps ~59 deg arc at 3 m */
    q.height_offset = 0.0f; /* metres relative to eye height */
    FILE *f = fopen("/sdcard/RVGL/quest_vr.cfg", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        float v;
        if (sscanf(line, "screen_distance=%f", &v) == 1 && v > 0.5f) q.dist = v;
        else if (sscanf(line, "screen_width=%f", &v) == 1 && v > 0.3f) q.width = v;
        else if (sscanf(line, "screen_height_offset=%f", &v) == 1) q.height_offset = v;
    }
    fclose(f);
}

/* -------------------------------------------------------------- GL wrappers */

static PFNGLBINDFRAMEBUFFERPROC real_BindFramebuffer;
static void (GL_APIENTRYP real_GetIntegerv)(GLenum, GLint *);
static void (GL_APIENTRYP real_InvalidateFramebuffer)(GLenum, GLsizei, const GLenum *);
static void (GL_APIENTRYP real_DiscardFramebufferEXT)(GLenum, GLsizei, const GLenum *);
static void (GL_APIENTRYP real_ReadBuffer)(GLenum);
static void (GL_APIENTRYP real_DrawBuffers)(GLsizei, const GLenum *);

static GLuint bound_fb(GLenum target)
{
    GLint v = 0;
    glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING
                                                : GL_DRAW_FRAMEBUFFER_BINDING, &v);
    return (GLuint)v;
}

static void GL_APIENTRY w_BindFramebuffer(GLenum target, GLuint fb)
{
    if (fb == 0 && q.fbo) fb = q.fbo;
    real_BindFramebuffer(target, fb);
}

static void GL_APIENTRY w_GetIntegerv(GLenum pname, GLint *v)
{
    real_GetIntegerv(pname, v);
    if (q.fbo && (pname == GL_DRAW_FRAMEBUFFER_BINDING || pname == GL_READ_FRAMEBUFFER_BINDING) &&
        *v == (GLint)q.fbo)
        *v = 0;
}

static int map_default_attachments(GLenum target, GLsizei n, const GLenum *in, GLenum *out)
{
    if (!q.fbo || n > 8 || bound_fb(target) != q.fbo) return 0;
    for (GLsizei i = 0; i < n; i++) {
        switch (in[i]) {
        case GL_COLOR: out[i] = GL_COLOR_ATTACHMENT0; break;
        case GL_DEPTH: out[i] = GL_DEPTH_ATTACHMENT; break;
        case GL_STENCIL: out[i] = GL_STENCIL_ATTACHMENT; break;
        default: out[i] = in[i]; break;
        }
    }
    return 1;
}

static void GL_APIENTRY w_InvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *att)
{
    GLenum tmp[8];
    if (map_default_attachments(target, n, att, tmp)) att = tmp;
    real_InvalidateFramebuffer(target, n, att);
}

static void GL_APIENTRY w_DiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum *att)
{
    GLenum tmp[8];
    if (map_default_attachments(target, n, att, tmp)) att = tmp;
    real_DiscardFramebufferEXT(target, n, att);
}

static void GL_APIENTRY w_ReadBuffer(GLenum src)
{
    if (src == GL_BACK && q.fbo && bound_fb(GL_READ_FRAMEBUFFER) == q.fbo) src = GL_COLOR_ATTACHMENT0;
    real_ReadBuffer(src);
}

static void GL_APIENTRY w_DrawBuffers(GLsizei n, const GLenum *bufs)
{
    GLenum tmp[8];
    if (q.fbo && n > 0 && n <= 8 && bound_fb(GL_DRAW_FRAMEBUFFER) == q.fbo) {
        for (GLsizei i = 0; i < n; i++) tmp[i] = bufs[i] == GL_BACK ? GL_COLOR_ATTACHMENT0 : bufs[i];
        bufs = tmp;
    }
    real_DrawBuffers(n, bufs);
}

int QXR_Enabled(void) { return 1; }

void *QXR_WrapProc(const char *name, void *real)
{
    if (!real || !name) return real;
#define WRAP(n, var, fn)                     \
    if (strcmp(name, n) == 0) {              \
        *(void **)&var = real;               \
        LOGI("wrapped %s", name);            \
        return (void *)fn;                   \
    }
    WRAP("glBindFramebuffer", real_BindFramebuffer, w_BindFramebuffer);
    WRAP("glBindFramebufferOES", real_BindFramebuffer, w_BindFramebuffer);
    WRAP("glGetIntegerv", real_GetIntegerv, w_GetIntegerv);
    WRAP("glInvalidateFramebuffer", real_InvalidateFramebuffer, w_InvalidateFramebuffer);
    WRAP("glDiscardFramebufferEXT", real_DiscardFramebufferEXT, w_DiscardFramebufferEXT);
    WRAP("glReadBuffer", real_ReadBuffer, w_ReadBuffer);
    WRAP("glDrawBuffers", real_DrawBuffers, w_DrawBuffers);
#undef WRAP
    return real;
}

/* ---------------------------------------------------------------- context */

void QXR_BeforeCreateContext(int *major, int *minor, int *profile_mask)
{
    (void)profile_mask;
    /* OpenXR on Quest needs an ES 3.x context; ES 3 runs the engine's ES 2 code. */
    if (*major < 3) {
        LOGI("context: engine asked for ES %d.%d, creating ES 3.0", *major, *minor);
        *major = 3;
        *minor = 0;
    }
}

#define XRCHK(x)                                                         \
    do {                                                                 \
        XrResult r_ = (x);                                               \
        if (XR_FAILED(r_)) {                                             \
            LOGE("%s failed: %d (line %d)", #x, (int)r_, __LINE__);      \
            return 0;                                                    \
        }                                                                \
    } while (0)

static XrPath path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(q.instance, s, &p);
    return p;
}

static XrAction make_action(const char *name, XrActionType type)
{
    XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", name);
    ci.countSubactionPaths = 2;
    ci.subactionPaths = q.hand;
    XrAction a = XR_NULL_HANDLE;
    XrResult r = xrCreateAction(q.set, &ci, &a);
    if (XR_FAILED(r)) LOGE("xrCreateAction %s: %d", name, (int)r);
    return a;
}

static int init_actions(void)
{
    XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "rvgl");
    strcpy(si.localizedActionSetName, "RVGL");
    XRCHK(xrCreateActionSet(q.instance, &si, &q.set));
    q.hand[HAND_L] = path("/user/hand/left");
    q.hand[HAND_R] = path("/user/hand/right");

    q.aTrig = make_action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    q.aGrip = make_action("grip", XR_ACTION_TYPE_FLOAT_INPUT);
    q.aStickX = make_action("stick_x", XR_ACTION_TYPE_FLOAT_INPUT);
    q.aStickY = make_action("stick_y", XR_ACTION_TYPE_FLOAT_INPUT);
    q.aClick = make_action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    q.aA = make_action("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    q.aB = make_action("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    q.aX = make_action("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT);
    q.aY = make_action("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    q.aMenu = make_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

    XrActionSuggestedBinding b[] = {
        {q.aTrig, path("/user/hand/left/input/trigger/value")},
        {q.aTrig, path("/user/hand/right/input/trigger/value")},
        {q.aGrip, path("/user/hand/left/input/squeeze/value")},
        {q.aGrip, path("/user/hand/right/input/squeeze/value")},
        {q.aStickX, path("/user/hand/left/input/thumbstick/x")},
        {q.aStickX, path("/user/hand/right/input/thumbstick/x")},
        {q.aStickY, path("/user/hand/left/input/thumbstick/y")},
        {q.aStickY, path("/user/hand/right/input/thumbstick/y")},
        {q.aClick, path("/user/hand/left/input/thumbstick/click")},
        {q.aClick, path("/user/hand/right/input/thumbstick/click")},
        {q.aA, path("/user/hand/right/input/a/click")},
        {q.aB, path("/user/hand/right/input/b/click")},
        {q.aX, path("/user/hand/left/input/x/click")},
        {q.aY, path("/user/hand/left/input/y/click")},
        {q.aMenu, path("/user/hand/left/input/menu/click")},
    };
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = sizeof b / sizeof b[0];
    XRCHK(xrSuggestInteractionProfileBindings(q.instance, &sb));

    XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &q.set;
    XRCHK(xrAttachSessionActionSets(q.session, &ai));
    return 1;
}

static int has_ext(const XrExtensionProperties *props, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(props[i].extensionName, name) == 0) return 1;
    return 0;
}

static int create_offscreen_fbo(void)
{
    glGenRenderbuffers(1, &q.colorRb);
    glBindRenderbuffer(GL_RENDERBUFFER, q.colorRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, QXR_GAME_W, QXR_GAME_H);
    glGenRenderbuffers(1, &q.depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, q.depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, QXR_GAME_W, QXR_GAME_H);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, q.colorRb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, q.depthRb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("offscreen FBO incomplete: 0x%x", st);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }
    glViewport(0, 0, QXR_GAME_W, QXR_GAME_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    q.fbo = fbo; /* stays bound: the engine believes framebuffer 0 is current */
    LOGI("offscreen FBO %u (%dx%d)", fbo, QXR_GAME_W, QXR_GAME_H);
    return 1;
}

static int xr_init(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act) {
        LOGE("no JNI env/activity");
        return 0;
    }
    JavaVM *vm = NULL;
    (*env)->GetJavaVM(env, &vm);
    jobject actRef = (*env)->NewGlobalRef(env, act);
    (*env)->DeleteLocalRef(env, act);

    PFN_xrInitializeLoaderKHR initLoader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = vm;
        li.applicationContext = actRef;
        initLoader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
    }

    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, props);
    const char *exts[8];
    uint32_t ne = 0;
    exts[ne++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    exts[ne++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    int perf = has_ext(props, n, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (perf) exts[ne++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    free(props);

    XrInstanceCreateInfoAndroidKHR aci = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    aci.applicationVM = vm;
    aci.applicationActivity = actRef;
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    ici.next = &aci;
    strcpy(ici.applicationInfo.applicationName, "RVGL Quest");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "RVGL");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = ne;
    ici.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ici, &q.instance);
    if (XR_FAILED(r)) {
        ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        XRCHK(xrCreateInstance(&ici, &q.instance));
    }

    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHK(xrGetSystem(q.instance, &sgi, &q.system));

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = NULL;
    xrGetInstanceProcAddr(q.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(q.instance, q.system, &req);

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLint cfgId = 0, nc = 0;
    EGLConfig cfg = 0;
    eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &cfgId);
    EGLint attr[] = {EGL_CONFIG_ID, cfgId, EGL_NONE};
    eglChooseConfig(dpy, attr, &cfg, 1, &nc);

    XrGraphicsBindingOpenGLESAndroidKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = dpy;
    gb.config = cfg;
    gb.context = ctx;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = q.system;
    XRCHK(xrCreateSession(q.instance, &sci, &q.session));

    if (perf) {
        PFN_xrPerfSettingsSetPerformanceLevelEXT setLevel = NULL;
        xrGetInstanceProcAddr(q.instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&setLevel);
        if (setLevel) {
            setLevel(q.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            setLevel(q.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        }
    }

    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XRCHK(xrCreateReferenceSpace(q.session, &rs, &q.localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHK(xrCreateReferenceSpace(q.session, &rs, &q.viewSpace));

    /* swapchain format: sRGB only if we can switch off encoding on write */
    const char *glext = (const char *)glGetString(GL_EXTENSIONS);
    q.srgbWriteControl = glext && strstr(glext, "GL_EXT_sRGB_write_control") != NULL;
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(q.session, 0, &nf, NULL);
    int64_t fmts[64];
    if (nf > 64) nf = 64;
    xrEnumerateSwapchainFormats(q.session, nf, &nf, fmts);
    q.format = 0;
    for (uint32_t i = 0; i < nf; i++) {
        if (q.srgbWriteControl && fmts[i] == GL_SRGB8_ALPHA8) { q.format = GL_SRGB8_ALPHA8; break; }
    }
    if (!q.format) {
        for (uint32_t i = 0; i < nf; i++)
            if (fmts[i] == GL_RGBA8) { q.format = GL_RGBA8; break; }
    }
    if (!q.format && nf) q.format = fmts[0];
    LOGI("swapchain format 0x%llx, sRGB write control %d", (long long)q.format, q.srgbWriteControl);

    XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sc.format = q.format;
    sc.sampleCount = 1;
    sc.width = QXR_GAME_W;
    sc.height = QXR_GAME_H;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XRCHK(xrCreateSwapchain(q.session, &sc, &q.swapchain));
    XRCHK(xrEnumerateSwapchainImages(q.swapchain, 0, &q.imgCount, NULL));
    if (q.imgCount > MAX_IMAGES) q.imgCount = MAX_IMAGES;
    for (uint32_t i = 0; i < q.imgCount; i++) q.imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XRCHK(xrEnumerateSwapchainImages(q.swapchain, q.imgCount, &q.imgCount,
                                     (XrSwapchainImageBaseHeader *)q.imgs));
    glGenFramebuffers((GLsizei)q.imgCount, q.imgFbo);
    for (uint32_t i = 0; i < q.imgCount; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, q.imgFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, q.imgs[i].image, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) LOGE("swapchain FBO %u incomplete 0x%x", i, st);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!init_actions()) return 0;

    load_config();
    q.quadPose.orientation.w = 1.0f;
    q.quadPose.position.z = -q.dist;
    q.recenterPending = 1;

    LOGI("OpenXR ready: %u swapchain images, screen %.2f m wide at %.2f m", q.imgCount, q.width, q.dist);
    return 1;
}

void QXR_OnContextCreated(void)
{
    if (q.inited || q.failed) return;
    LOGI("GL: %s / %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    if (!xr_init()) {
        LOGE("OpenXR init failed, falling back to the plain window");
        q.failed = 1;
        return;
    }
    if (!create_offscreen_fbo()) {
        q.failed = 1;
        return;
    }
    q.inited = 1;
}

/* ------------------------------------------------------------------ events */

static void poll_events(void)
{
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(q.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *s = (const XrEventDataSessionStateChanged *)&ev;
            q.state = s->state;
            LOGI("session state %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(q.session, &bi))) {
                    q.running = 1;
                    q.recenterPending = 1;
                }
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(q.session);
                q.running = 0;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                q.running = 0;
                SDL_Event e;
                SDL_zero(e);
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            q.recenterPending = 1;
            break;
        default:
            break;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

/* ------------------------------------------------------------------- input */

static float getf(XrAction a, int hand)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = q.hand[hand];
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(q.session, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}

static int getb(XrAction a, int hand)
{
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = q.hand[hand];
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(q.session, &gi, &st)) || !st.isActive) return 0;
    return st.currentState ? 1 : 0;
}

void QXR_OnJoystickInit(void)
{
    if (q.padAttached) return;
    SDL_GameControllerAddMapping(kPadMapping);
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, VA_COUNT, VB_COUNT, 0);
    if (idx < 0) {
        LOGE("virtual pad attach failed: %s", SDL_GetError());
        return;
    }
    q.padAttached = 1;
    LOGI("virtual pad attached (device %d)", idx);
}

static void open_pad(void)
{
    if (q.pad || !q.padAttached) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        if (g.data[14] == 'v') {
            q.pad = SDL_JoystickOpen(i);
            LOGI("virtual pad opened (index %d, controller %d)", i, SDL_IsGameController(i));
            return;
        }
    }
}

static Sint16 axis(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (Sint16)(v * 32767.0f);
}

static void update_input(void)
{
    open_pad();
    XrActiveActionSet as = {q.set, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    int focused = XR_SUCCEEDED(xrSyncActions(q.session, &si)) && q.state == XR_SESSION_STATE_FOCUSED;

    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    int bA = 0, bB = 0, bX = 0, bY = 0, bMenu = 0, cL = 0, cR = 0;
    float gL = 0, gR = 0;
    if (focused) {
        lx = getf(q.aStickX, HAND_L); ly = getf(q.aStickY, HAND_L);
        rx = getf(q.aStickX, HAND_R); ry = getf(q.aStickY, HAND_R);
        lt = getf(q.aTrig, HAND_L);   rt = getf(q.aTrig, HAND_R);
        gL = getf(q.aGrip, HAND_L);   gR = getf(q.aGrip, HAND_R);
        bA = getb(q.aA, HAND_R); bB = getb(q.aB, HAND_R);
        bX = getb(q.aX, HAND_L); bY = getb(q.aY, HAND_L);
        bMenu = getb(q.aMenu, HAND_L);
        cL = getb(q.aClick, HAND_L); cR = getb(q.aClick, HAND_R);
    }

    /* grips are digital shoulders, with hysteresis */
    float g[2] = {gL, gR};
    for (int h = 0; h < 2; h++) {
        if (!q.gripDown[h] && g[h] > 0.45f) q.gripDown[h] = 1;
        else if (q.gripDown[h] && g[h] < 0.25f) q.gripDown[h] = 0;
    }

    /* hold both stick clicks 1 s: re-place the screen in front of the head */
    if (cL && cR) {
        if (!q.bothClickStart) q.bothClickStart = SDL_GetTicks();
        if (!q.bothClickFired && SDL_GetTicks() - q.bothClickStart > 1000) {
            q.recenterPending = 1;
            q.bothClickFired = 1;
            LOGI("recenter requested");
        }
    } else {
        q.bothClickStart = 0;
        q.bothClickFired = 0;
    }
    if (q.bothClickStart) cL = cR = 0; /* don't leak the combo into the game */

    if (!q.pad) return;
    SDL_JoystickSetVirtualAxis(q.pad, VA_LX, axis(lx));
    SDL_JoystickSetVirtualAxis(q.pad, VA_LY, axis(-ly));
    SDL_JoystickSetVirtualAxis(q.pad, VA_RX, axis(rx));
    SDL_JoystickSetVirtualAxis(q.pad, VA_RY, axis(-ry));
    SDL_JoystickSetVirtualAxis(q.pad, VA_LT, axis(lt));
    SDL_JoystickSetVirtualAxis(q.pad, VA_RT, axis(rt));
    SDL_JoystickSetVirtualButton(q.pad, VB_A, bA);
    SDL_JoystickSetVirtualButton(q.pad, VB_B, bB);
    SDL_JoystickSetVirtualButton(q.pad, VB_X, bX);
    SDL_JoystickSetVirtualButton(q.pad, VB_Y, bY);
    SDL_JoystickSetVirtualButton(q.pad, VB_START, bMenu);
    SDL_JoystickSetVirtualButton(q.pad, VB_LSTICK, cL);
    SDL_JoystickSetVirtualButton(q.pad, VB_RSTICK, cR);
    SDL_JoystickSetVirtualButton(q.pad, VB_LSHOULDER, q.gripDown[HAND_L]);
    SDL_JoystickSetVirtualButton(q.pad, VB_RSHOULDER, q.gripDown[HAND_R]);
}

/* ----------------------------------------------------------------- present */

static void place_screen(XrTime t)
{
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(q.viewSpace, q.localSpace, t, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    XrQuaternionf o = loc.pose.orientation;
    /* head forward = rotate (0,0,-1) by o */
    float fx = -2.0f * (o.x * o.z + o.w * o.y);
    float fz = -(1.0f - 2.0f * (o.x * o.x + o.y * o.y));
    float yaw = atan2f(-fx, -fz);
    q.quadPose.orientation.x = 0.0f;
    q.quadPose.orientation.y = sinf(yaw * 0.5f);
    q.quadPose.orientation.z = 0.0f;
    q.quadPose.orientation.w = cosf(yaw * 0.5f);
    q.quadPose.position.x = loc.pose.position.x - sinf(yaw) * q.dist;
    q.quadPose.position.y = loc.pose.position.y + q.height_offset;
    q.quadPose.position.z = loc.pose.position.z - cosf(yaw) * q.dist;
    q.recenterPending = 0;
    LOGI("screen placed: yaw %.1f deg, pos %.2f %.2f %.2f", yaw * 57.2958f,
         q.quadPose.position.x, q.quadPose.position.y, q.quadPose.position.z);
}

static int copy_frame(void)
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(q.swapchain, &ai, &idx))) return 0;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(q.swapchain, &wi);

    GLint draw = 0, read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean srgb = q.srgbWriteControl ? glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT) : GL_FALSE;
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (q.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, q.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, q.imgFbo[idx]);
    glBlitFramebuffer(0, 0, QXR_GAME_W, QXR_GAME_H, 0, 0, QXR_GAME_W, QXR_GAME_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (srgb) glEnable(GL_FRAMEBUFFER_SRGB_EXT);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(q.swapchain, &ri);
    return 1;
}

int QXR_Present(void)
{
    if (!q.inited) return 0;
    poll_events();
    if (!q.running) {
        SDL_Delay(10);
        return 1;
    }

    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(q.session, &fwi, &fs))) return 1;
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(q.session, &fbi))) return 1;

    update_input();
    if (q.recenterPending) place_screen(fs.predictedDisplayTime);

    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[1];
    uint32_t nl = 0;
    if (fs.shouldRender && copy_frame()) {
        quad.space = q.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = q.swapchain;
        quad.subImage.imageRect.offset.x = 0;
        quad.subImage.imageRect.offset.y = 0;
        quad.subImage.imageRect.extent.width = QXR_GAME_W;
        quad.subImage.imageRect.extent.height = QXR_GAME_H;
        quad.pose = q.quadPose;
        quad.size.width = q.width;
        quad.size.height = q.width * (float)QXR_GAME_H / (float)QXR_GAME_W;
        layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
    }

    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nl;
    fei.layers = layers;
    XrResult r = xrEndFrame(q.session, &fei);
    if (XR_FAILED(r)) LOGE("xrEndFrame: %d", (int)r);

    q.statFrames++;
    Uint32 now = SDL_GetTicks();
    if (!q.statT0) q.statT0 = now;
    if (now - q.statT0 >= 5000) {
        LOGI("perf: %.1f fps presented (state %d)", q.statFrames * 1000.0f / (float)(now - q.statT0), (int)q.state);
        q.statT0 = now;
        q.statFrames = 0;
    }
    return 1;
}

int QXR_IgnoreAndroidJoystick(const char *name, int vendor_id)
{
    if (vendor_id == 0x2833) return 1;
    if (name && (strstr(name, "Oculus") || strstr(name, "Quest") || strstr(name, "Meta "))) return 1;
    return 0;
}
