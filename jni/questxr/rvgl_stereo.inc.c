/* Included from rvgl_questxr.c: frame loop, recentre, stereo eye passes, present.
 *
 * Engine facts (RVGL 23.1030a1 arm64 libmain.so, all symbols exported):
 *   CAMERA (0x140 bytes) in cameras[8]: +0x20 MAT WMatrix (rows right/down/forward),
 *   +0x54 VEC WPos, +0x10/+0x18 position/look callbacks, +0x100 VIEWPORT
 *   {x,y,w,h,projW,projH,...}, +0x128 id, +0x12c active (0 free, 1 player view with HUD,
 *   3 disabled; any other non-zero value = world pass only).
 *   GLP_GameLoop: pass 1 for every active camera: MoveCamera, SetViewport(&cam->vp, lens),
 *   SetCameraView(cam) ... DrawWorld ... FlushAllBuffers; pass 2 (type 1 only): SetViewport,
 *   SetCameraView, HUD; then SetViewport(screen_viewport), menus, FlipBuffers.
 *   SetViewport fills render_settings {cx, cy, w/projW, h/projH, lens, 2lens/projW,
 *   -2lens/projH} and then calls glViewport/glScissor through GLAD pointers. Vertices are
 *   projected on the CPU from these values, so a wider FOV = bigger projW/projH vs lens.
 */

#define CAM_SIZE 0x140
#define CAM_POSFN 0x10
#define CAM_LOOKFN 0x18
#define CAM_MAT 0x20
#define CAM_POS 0x54
#define CAM_VP 0x100
#define CAM_ACTIVE 0x12c
#define CAM_ACTIVE_EYE 4

static struct {
    int tried, ok;
    char *cameras;
    char **mainCam;
    char **curVp;
    float *rs;
    void **evtLoop;
    void *gameLoop;
    char *screenVp;
    char *(*AddCamera)(int, float, float, float, float, int);
    void (*RemoveCamera)(char *);
} E;

static void engine_resolve(void)
{
    if (E.tried) return;
    E.tried = 1;
    void *h = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        LOGE("stereo: libmain.so not loaded: %s", dlerror());
        return;
    }
    E.cameras = (char *)dlsym(h, "cameras");
    E.mainCam = (char **)dlsym(h, "cam_main_camera");
    E.curVp = (char **)dlsym(h, "current_viewport");
    E.rs = (float *)dlsym(h, "render_settings");
    E.evtLoop = (void **)dlsym(h, "evt_event_loop");
    E.gameLoop = dlsym(h, "_Z12GLP_GameLoopv");
    E.screenVp = (char *)dlsym(h, "screen_viewport");
    *(void **)&E.AddCamera = dlsym(h, "_Z9AddCameraiffffi");
    *(void **)&E.RemoveCamera = dlsym(h, "_Z12RemoveCameraP6CAMERA");
    E.ok = E.cameras && E.mainCam && E.curVp && E.rs && E.evtLoop && E.gameLoop && E.screenVp &&
           E.AddCamera && E.RemoveCamera;
    LOGI("stereo: engine symbols %s", E.ok ? "resolved" : "MISSING");
}

/* RVGL only reads a menu/game controller that the profile assigns to a player.
 * A fresh install (no profile yet) has none, so the Touch pad would be ignored even
 * on the Create Profile screen: give player 1 the pad whenever nothing is assigned. */
static void ensure_player1_pad(void)
{
    static char *settings;
    static int (*initJoystick)(int);
    static int tried;
    if (!tried) {
        tried = 1;
        void *h = dlopen("libmain.so", RTLD_NOW | RTLD_NOLOAD);
        if (h) {
            settings = (char *)dlsym(h, "settings");
            *(void **)&initJoystick = dlsym(h, "_Z12InitJoysticki");
        }
        if (!settings || !initJoystick) LOGE("controls: engine symbols missing");
    }
    if (!settings || !initJoystick || !q.pad) return;
    /* player 1 controller block (17 ints, measured): [0] Joystick, [1] SteeringDeadzone,
     * [2] SteeringRange, [3] ButtonOpacity, [4..14] keys Left Right Fwd Back Fire Reset
     * Reposition Honka ChangeCamera RearView Pause, [15..16] ForceFeedback/NonLinear flags */
    int *c = (int *)(settings + 0x2c0);
    static int dumped;
    if (!dumped) {
        dumped = 1;
        LOGI("controls: p1 joy %d dz %d range %d opacity %d keys %08x %08x %08x %08x",
             c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }
    int changed = 0;
    if (c[0] == -1) {
        c[0] = 0;
        changed = 1;
        LOGI("controls: player 1 had no controller, assigned Quest Touch");
    }
    int sane = c[1] >= 0 && c[1] <= 100 && c[2] > 0 && c[2] <= 100 && c[3] >= 0 && c[3] <= 100;
    /* Fwd still a keyboard key (< 0x01000000) or unbound (-1): no pad bindings yet */
    if (c[0] == 0 && sane && ((unsigned)c[6] < 0x01000000u || c[6] == -1)) {
        /* still keyboard bindings: RVGL's gamepad layout (left stick steers, RT/LT drive) */
        static const int pad_keys[11] = {0x02000000, 0x03000000, 0x03ff0005, 0x03ff0004, 0x01000000,
                                         0x01000001, 0x01000002, 0x01ff0008, 0x01ff0003, 0x01ff000a,
                                         0x01ff0006};
        memcpy(c + 4, pad_keys, sizeof pad_keys);
        changed = 1;
        LOGI("controls: player 1 bindings set to the Quest Touch gamepad layout");
    }
    if (c[0] == 0 && sane && c[3] != 0) {
        c[3] = 0; /* no Android touch-screen buttons in VR */
        LOGI("controls: touch buttons hidden");
    }
    if (changed) initJoystick(0);
}

static int menu_mode(void)
{
    engine_resolve();
    return !(E.ok && *E.evtLoop == E.gameLoop);
}

static int in_race(void)
{
    if (!q.stereoCfg || !q.eyesReady || !q.viewsValid) return 0;
    engine_resolve();
    if (!E.ok) return 0;
    return *E.evtLoop == E.gameLoop && *E.mainCam != NULL;
}

/* ------------------------------------------------------------ eye targets */

static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC p_RbStorageMsEXT;
static PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC p_FbTex2DMsEXT;

static void create_eye_targets(XrViewConfigurationView *vcv)
{
    /* symmetric frustum covering both eyes' (asymmetric) FOV */
    float tl = 0, tr = 0, tu = 0, td = 0;
    for (int e = 0; e < 2; e++) {
        XrFovf f = q.views[e].fov;
        if (tanf(-f.angleLeft) > tl) tl = tanf(-f.angleLeft);
        if (tanf(f.angleRight) > tr) tr = tanf(f.angleRight);
        if (tanf(f.angleUp) > tu) tu = tanf(f.angleUp);
        if (tanf(-f.angleDown) > td) td = tanf(-f.angleDown);
    }
    q.tanX = tl > tr ? tl : tr;
    q.tanY = tu > td ? tu : td;
    XrFovf f0 = q.views[0].fov;
    float densX = (float)vcv[0].recommendedImageRectWidth / (tanf(-f0.angleLeft) + tanf(f0.angleRight));
    float densY = (float)vcv[0].recommendedImageRectHeight / (tanf(f0.angleUp) + tanf(-f0.angleDown));
    int w = (int)(densX * 2.0f * q.tanX * q.eyeScale), h = (int)(densY * 2.0f * q.tanY * q.eyeScale);
    if (w > (int)vcv[0].maxImageRectWidth) w = (int)vcv[0].maxImageRectWidth;
    if (h > (int)vcv[0].maxImageRectHeight) h = (int)vcv[0].maxImageRectHeight;
    if (w > 2560) w = 2560;
    if (h > 2560) h = 2560;
    q.eyeW = w & ~7;
    q.eyeH = h & ~7;

    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (q.msaa > 1 && ext && strstr(ext, "GL_EXT_multisampled_render_to_texture")) {
        p_RbStorageMsEXT = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
        p_FbTex2DMsEXT = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
    }
    int ms = (p_RbStorageMsEXT && p_FbTex2DMsEXT) ? q.msaa : 1;

    GLint prevFb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = q.format;
        sc.sampleCount = 1;
        sc.width = (uint32_t)q.eyeW;
        sc.height = (uint32_t)q.eyeH;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(q.session, &sc, &q.eyeSwap[e]))) {
            LOGE("stereo: eye swapchain failed");
            return;
        }
        xrEnumerateSwapchainImages(q.eyeSwap[e], 0, &q.eyeImgCount[e], NULL);
        if (q.eyeImgCount[e] > MAX_IMAGES) q.eyeImgCount[e] = MAX_IMAGES;
        for (uint32_t i = 0; i < q.eyeImgCount[e]; i++) q.eyeImgs[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(q.eyeSwap[e], q.eyeImgCount[e], &q.eyeImgCount[e],
                                   (XrSwapchainImageBaseHeader *)q.eyeImgs[e]);

        glGenRenderbuffers(1, &q.eyeDepth[e]);
        glBindRenderbuffer(GL_RENDERBUFFER, q.eyeDepth[e]);
        if (ms > 1) p_RbStorageMsEXT(GL_RENDERBUFFER, ms, GL_DEPTH24_STENCIL8, q.eyeW, q.eyeH);
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, q.eyeW, q.eyeH);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers((GLsizei)q.eyeImgCount[e], q.eyeImgFbo[e]);
        for (uint32_t i = 0; i < q.eyeImgCount[e]; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, q.eyeImgFbo[e][i]);
            if (ms > 1)
                p_FbTex2DMsEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, q.eyeImgs[e][i].image, 0, ms);
            else
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, q.eyeImgs[e][i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, q.eyeDepth[e]);
            GLenum stt = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (stt != GL_FRAMEBUFFER_COMPLETE) {
                LOGE("stereo: eye FBO incomplete 0x%x", stt);
                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFb);
                return;
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFb);
    q.eyesReady = 1;
    LOGI("stereo: eyes %dx%d, msaa %d, fov tan x %.3f y %.3f (%.1f x %.1f deg), world scale %.1f u/m",
         q.eyeW, q.eyeH, ms, q.tanX, q.tanY, 2.0f * atanf(q.tanX) * 57.2958f, 2.0f * atanf(q.tanY) * 57.2958f,
         q.worldScale);
}

/* ------------------------------------------------------------- eye math */

static void qrot(XrQuaternionf o, const float v[3], float out[3])
{
    /* out = o * v * conj(o) */
    float tx = 2.0f * (o.y * v[2] - o.z * v[1]);
    float ty = 2.0f * (o.z * v[0] - o.x * v[2]);
    float tz = 2.0f * (o.x * v[1] - o.y * v[0]);
    out[0] = v[0] + o.w * tx + (o.y * tz - o.z * ty);
    out[1] = v[1] + o.w * ty + (o.z * tx - o.x * tz);
    out[2] = v[2] + o.w * tz + (o.x * ty - o.y * tx);
}

/* game local axes: x right, y down, z forward; OpenXR: x right, y up, z back */
static void xr_to_game(const float v[3], float out[3])
{
    out[0] = v[0];
    out[1] = -v[1];
    out[2] = -v[2];
}

static void eye_transform(const float *camM, const float *camP, int eye, float *outM, float *outP)
{
    float B[9];
    if (q.horizonLock) {
        float fx = camM[6], fz = camM[8];
        float len = sqrtf(fx * fx + fz * fz);
        if (len > 1e-3f) {
            q.lastFwd[0] = fx / len;
            q.lastFwd[1] = fz / len;
        } else if (q.lastFwd[0] == 0.0f && q.lastFwd[1] == 0.0f) {
            q.lastFwd[1] = 1.0f;
        }
        fx = q.lastFwd[0];
        fz = q.lastFwd[1];
        /* forward (fx,0,fz), down (0,1,0), right = down x forward = (fz, 0, -fx) */
        B[0] = fz;  B[1] = 0; B[2] = -fx;
        B[3] = 0;   B[4] = 1; B[5] = 0;
        B[6] = fx;  B[7] = 0; B[8] = fz;
    } else {
        memcpy(B, camM, sizeof B);
    }

    XrPosef p = q.views[eye].pose;
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float r[3], u[3], b[3], lr[3], ld[3], lf[3];
    qrot(p.orientation, ex, r);
    qrot(p.orientation, ey, u);
    qrot(p.orientation, ez, b);
    xr_to_game(r, lr);
    float nu[3] = {-u[0], -u[1], -u[2]}, nb[3] = {-b[0], -b[1], -b[2]};
    xr_to_game(nu, ld);
    xr_to_game(nb, lf);
    const float *loc[3] = {lr, ld, lf};
    for (int row = 0; row < 3; row++)
        for (int c = 0; c < 3; c++)
            outM[row * 3 + c] = loc[row][0] * B[c] + loc[row][1] * B[3 + c] + loc[row][2] * B[6 + c];

    float pv[3] = {p.position.x * q.worldScale, p.position.y * q.worldScale, p.position.z * q.worldScale}, lp[3];
    xr_to_game(pv, lp);
    for (int c = 0; c < 3; c++) outP[c] = camP[c] + lp[0] * B[c] + lp[1] * B[3 + c] + lp[2] * B[6 + c];
}

/* wide symmetric projection for one eye camera, after SetViewport filled render_settings */
static void eye_projection(char *cam)
{
    float *vp = (float *)(cam + CAM_VP);
    float projW = vp[4];
    float lens = projW / (2.0f * q.tanX);
    float projH = 2.0f * lens * q.tanY;
    vp[5] = projH;
    E.rs[2] = vp[2] / projW;
    E.rs[3] = vp[3] / projH;
    E.rs[4] = lens;
    E.rs[5] = 2.0f * lens / projW;
    E.rs[6] = -2.0f * lens / projH;
}

static void bind_target(GLuint fb)
{
    q.fbo = fb; /* framebuffer 0 now means this target */
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    if (q.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

static int acquire_eye(int e)
{
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(q.eyeSwap[e], &ai, &q.eyeIdx[e]))) return 0;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(q.eyeSwap[e], &wi);
    q.eyeAcquired[e] = 1;
    bind_target(q.eyeImgFbo[e][q.eyeIdx[e]]);
    return 1;
}

static void release_eyes(void)
{
    for (int e = 0; e < 2; e++) {
        if (!q.eyeAcquired[e]) continue;
        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(q.eyeSwap[e], &ri);
        q.eyeAcquired[e] = 0;
    }
}

static void restore_main(void)
{
    char *m = *E.mainCam;
    memcpy(m + CAM_MAT, q.savedMat, sizeof q.savedMat);
    memcpy(m + CAM_POS, q.savedPos, sizeof q.savedPos);
    ((float *)(m + CAM_VP))[5] = q.savedProjH;
}

static int ensure_clone(char *main)
{
    char *c = q.clone;
    if (c && c != main && *(int *)(c + CAM_ACTIVE) == CAM_ACTIVE_EYE) return 1;
    c = E.AddCamera(*(int *)(main + 0x128), 0.0f, 0.0f, 1.0f, 1.0f, CAM_ACTIVE_EYE);
    if (!c) {
        LOGE("stereo: AddCamera failed (no free camera slot)");
        return 0;
    }
    if (c < main) {
        /* the world pass runs in slot order: the right eye must come after the main camera */
        LOGE("stereo: eye camera slot %d is before main slot %d", (int)((c - E.cameras) / CAM_SIZE),
             (int)((main - E.cameras) / CAM_SIZE));
        E.RemoveCamera(c);
        return 0;
    }
    q.clone = c;
    LOGI("stereo: eye camera in slot %d (main slot %d)", (int)((c - E.cameras) / CAM_SIZE),
         (int)((main - E.cameras) / CAM_SIZE));
    return 1;
}

static void hud_begin(void)
{
    if (q.st == ST_EYE0) restore_main();
    q.st = ST_HUD;
    bind_target(q.mainFbo);
    GLfloat cc[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cm[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (sc) glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
}

static Uint32 s_diagT;
static void stereo_diag(void)
{
    Uint32 now = SDL_GetTicks();
    if (now - s_diagT < 5000) return;
    s_diagT = now;
    engine_resolve();
    LOGI("stereo diag: cfg %d eyes %d views %d ok %d loop %p (game %p) main %p vp %p (main vp %p) st %d clone %p",
         q.stereoCfg, q.eyesReady, q.viewsValid, E.ok, E.ok ? *E.evtLoop : NULL, E.gameLoop,
         E.ok ? (void *)*E.mainCam : NULL, E.ok ? (void *)*E.curVp : NULL,
         (E.ok && *E.mainCam) ? (void *)(*E.mainCam + CAM_VP) : NULL, q.st, (void *)q.clone);
}

static void stereo_on_viewport(void)
{
    stereo_diag();
    if (q.st == ST_HUD) return;
    if (q.st == ST_NONE && !in_race()) return;
    char *main = *E.mainCam;
    char *vp = *E.curVp;
    if (q.st == ST_NONE) {
        if (vp != main + CAM_VP) return;
        if (!ensure_clone(main)) return;
        memcpy(q.savedMat, main + CAM_MAT, sizeof q.savedMat);
        memcpy(q.savedPos, main + CAM_POS, sizeof q.savedPos);
        q.savedProjH = ((float *)(main + CAM_VP))[5];
        if (!acquire_eye(0)) return;
        float m[9], p[3];
        eye_transform(q.savedMat, q.savedPos, 0, m, p);
        memcpy(main + CAM_MAT, m, sizeof m);
        memcpy(main + CAM_POS, p, sizeof p);
        eye_projection(main);
        q.st = ST_EYE0;
        return;
    }
    if (q.st == ST_EYE0 && vp == q.clone + CAM_VP) {
        restore_main();
        char *c = q.clone;
        memcpy(c, main, CAM_SIZE);
        *(int *)(c + CAM_ACTIVE) = CAM_ACTIVE_EYE;
        *(void **)(c + CAM_POSFN) = NULL;
        *(void **)(c + CAM_LOOKFN) = NULL;
        if (!acquire_eye(1)) {
            hud_begin();
            return;
        }
        float m[9], p[3];
        eye_transform(q.savedMat, q.savedPos, 1, m, p);
        memcpy(c + CAM_MAT, m, sizeof m);
        memcpy(c + CAM_POS, p, sizeof p);
        eye_projection(c);
        q.st = ST_EYE1;
        return;
    }
    if (vp == main + CAM_VP || vp == E.screenVp) hud_begin();
}

/* ---------------------------------------------------------------- frame */

static void recenter(XrTime t)
{
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(q.viewSpace, q.localSpace, t, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    XrQuaternionf o = loc.pose.orientation;
    float fx = -2.0f * (o.x * o.z + o.w * o.y);
    float fz = -(1.0f - 2.0f * (o.x * o.x + o.y * o.y));
    float yaw = atan2f(-fx, -fz);
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.y = sinf(yaw * 0.5f);
    rs.poseInReferenceSpace.orientation.w = cosf(yaw * 0.5f);
    rs.poseInReferenceSpace.position = loc.pose.position;
    XrSpace ns = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateReferenceSpace(q.session, &rs, &ns))) return;
    if (q.appSpace && q.appSpace != q.localSpace) xrDestroySpace(q.appSpace);
    q.appSpace = ns;
    q.recenterPending = 0;
    LOGI("recentred: yaw %.1f deg, head %.2f %.2f %.2f", yaw * 57.2958f, loc.pose.position.x,
         loc.pose.position.y, loc.pose.position.z);
}

static XrViewConfigurationView s_vcv[2];

static void frame_begin(void)
{
    if (q.frameBegun || !q.inited) return;
    poll_events();
    if (!q.running) return;
    q.fs.type = XR_TYPE_FRAME_STATE;
    q.fs.next = NULL;
    XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(q.session, &fwi, &q.fs))) return;
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(q.session, &fbi))) return;
    q.frameBegun = 1;

    update_input();
    ensure_player1_pad();
    if (q.recenterPending) recenter(q.fs.predictedDisplayTime);

    q.viewsValid = 0;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = q.fs.predictedDisplayTime;
    li.space = q.appSpace;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    q.views[0].type = q.views[1].type = XR_TYPE_VIEW;
    if (XR_SUCCEEDED(xrLocateViews(q.session, &li, &vs, 2, &n, q.views)) && n == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        q.viewsValid = 1;

    if (q.viewsValid && !q.eyesReady && q.stereoCfg && !q.eyeSwap[0]) {
        uint32_t nv = 0;
        s_vcv[0].type = s_vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        xrEnumerateViewConfigurationViews(q.instance, q.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, s_vcv);
        create_eye_targets(s_vcv);
    }
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
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (q.srgbWriteControl) glDisable(GL_FRAMEBUFFER_SRGB_EXT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, q.mainFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, q.imgFbo[idx]);
    glBlitFramebuffer(0, 0, QXR_GAME_W, QXR_GAME_H, 0, 0, QXR_GAME_W, QXR_GAME_H, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(q.swapchain, &ri);
    return 1;
}

int QXR_Present(void)
{
    if (!q.inited) return 0;
    frame_begin();
    if (!q.running || !q.frameBegun) {
        if (q.st == ST_EYE0) restore_main();
        release_eyes();
        q.st = ST_NONE;
        bind_target(q.mainFbo);
        SDL_Delay(10);
        return 1;
    }

    int stereo = q.st != ST_NONE && q.eyeAcquired[0] && q.eyeAcquired[1];
    if (q.st == ST_EYE0) restore_main();
    release_eyes();

    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nl = 0;
    if (q.fs.shouldRender) {
        if (stereo) {
            for (int e = 0; e < 2; e++) {
                memset(&pv[e], 0, sizeof pv[e]);
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pv[e].pose = q.views[e].pose;
                pv[e].fov.angleLeft = -atanf(q.tanX);
                pv[e].fov.angleRight = atanf(q.tanX);
                pv[e].fov.angleUp = atanf(q.tanY);
                pv[e].fov.angleDown = -atanf(q.tanY);
                pv[e].subImage.swapchain = q.eyeSwap[e];
                pv[e].subImage.imageRect.extent.width = q.eyeW;
                pv[e].subImage.imageRect.extent.height = q.eyeH;
            }
            proj.space = q.appSpace;
            proj.viewCount = 2;
            proj.views = pv;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&proj;
        }
        if (copy_frame()) {
            quad.space = q.appSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = q.swapchain;
            quad.subImage.imageRect.extent.width = QXR_GAME_W;
            quad.subImage.imageRect.extent.height = QXR_GAME_H;
            float w = stereo ? q.hudWidth : q.width;
            quad.pose.orientation.w = 1.0f;
            quad.pose.position.y = stereo ? q.hudHeight : q.height_offset;
            quad.pose.position.z = stereo ? -q.hudDist : -q.dist;
            quad.size.width = w;
            quad.size.height = w * (float)QXR_GAME_H / (float)QXR_GAME_W;
            if (stereo) quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layers[nl++] = (const XrCompositionLayerBaseHeader *)&quad;
        }
    }

    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = q.fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nl;
    fei.layers = layers;
    XrResult r = xrEndFrame(q.session, &fei);
    if (XR_FAILED(r)) LOGE("xrEndFrame: %d", (int)r);
    q.frameBegun = 0;
    q.st = ST_NONE;
    bind_target(q.mainFbo);

    if (stereo) q.statStereo++;
    else q.statFlat++;
    q.statFrames++;
    Uint32 now = SDL_GetTicks();
    if (!q.statT0) q.statT0 = now;
    if (now - q.statT0 >= 5000) {
        LOGI("perf: %.1f fps presented (stereo %d, flat %d, state %d)",
             q.statFrames * 1000.0f / (float)(now - q.statT0), q.statStereo, q.statFlat, (int)q.state);
        q.statT0 = now;
        q.statFrames = q.statStereo = q.statFlat = 0;
    }
    return 1;
}
