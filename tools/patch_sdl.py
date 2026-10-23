#!/usr/bin/env python3
"""Apply the Quest VR hooks to the SDL2 2.0.20 copy in jni/SDL2.

Idempotent: every edit checks for its marker first. Run from the repo root:
    python3 tools/patch_sdl.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDL = ROOT / "jni" / "SDL2"
MARK = "/* QUESTXR */"
INC = '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n"


def edit(rel, old, new, count=1):
    p = SDL / rel
    s = p.read_text()
    if new in s:
        print(f"  already: {rel}: {new.strip()[:60]}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor found {n}x (want {count}) in {rel}: {old[:80]!r}")
    p.write_text(s.replace(old, new))
    print(f"  patched: {rel}")


def include(rel, *_):
    p = SDL / rel
    s = p.read_text()
    line = '#include "rvgl_questxr.h" ' + MARK + "\n"
    if line in s:
        print(f"  already include: {rel}")
        return
    m = re.search(r'#include "(\.\./)+SDL_internal\.h"\n', s)
    assert m, f"SDL_internal.h include not found in {rel}"
    p.write_text(s[:m.end()] + line + s[m.end():])
    print(f"  include: {rel}")


# ---- SDL_video.c: GL proc wrapping, swap, context creation
include("src/video/SDL_video.c", '#include "SDL_video.h"\n', '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n")
edit("src/video/SDL_video.c",
     "            func = _this->GL_GetProcAddress(_this, proc);\n",
     "            func = _this->GL_GetProcAddress(_this, proc);\n"
     "            func = QXR_WrapProc(proc, func); " + MARK + "\n")
edit("src/video/SDL_video.c",
     "    ctx = _this->GL_CreateContext(_this, window);\n",
     "    { " + MARK + "\n"
     "        int qmaj = _this->gl_config.major_version, qmin = _this->gl_config.minor_version;\n"
     "        QXR_BeforeCreateContext(&_this->gl_config.major_version, &_this->gl_config.minor_version,\n"
     "                                &_this->gl_config.profile_mask);\n"
     "        ctx = _this->GL_CreateContext(_this, window);\n"
     "        _this->gl_config.major_version = qmaj;\n"
     "        _this->gl_config.minor_version = qmin;\n"
     "    }\n")
edit("src/video/SDL_video.c",
     "        SDL_TLSSet(_this->current_glctx_tls, ctx, NULL);\n    }\n    return ctx;\n",
     "        SDL_TLSSet(_this->current_glctx_tls, ctx, NULL);\n"
     "        QXR_OnContextCreated(); " + MARK + "\n    }\n    return ctx;\n")
edit("src/video/SDL_video.c",
     "    _this->GL_SwapWindow(_this, window);\n}\n",
     "    if (QXR_Present()) { " + MARK + "\n        return;\n    }\n"
     "    _this->GL_SwapWindow(_this, window);\n}\n")

# ---- Android video: the game always sees a QXR_GAME_W x QXR_GAME_H screen
include("src/video/android/SDL_androidvideo.c", '#include "SDL_androidvideo.h"\n',
        '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n")
edit("src/video/android/SDL_androidvideo.c",
     "Android_SetScreenResolution(int surfaceWidth, int surfaceHeight, int deviceWidth, int deviceHeight, float rate)\n{\n",
     "Android_SetScreenResolution(int surfaceWidth, int surfaceHeight, int deviceWidth, int deviceHeight, float rate)\n{\n"
     "    if (QXR_Enabled()) { " + MARK + "\n"
     "        surfaceWidth = deviceWidth = QXR_GAME_W;\n"
     "        surfaceHeight = deviceHeight = QXR_GAME_H;\n"
     "        if (rate < 72.0f) rate = 72.0f;\n"
     "    }\n")

# ---- focus loss: immersive VR apps lose window focus right after start
include("src/core/android/SDL_android.c", '#include "SDL_android.h"\n',
        '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n")
p = SDL / "src/core/android/SDL_android.c"
s = p.read_text()
i = s.find("JNICALL SDL_JAVA_INTERFACE(nativeFocusChanged)(\n                                    JNIEnv *env")
if i < 0:
    i = s.find("SDL_JAVA_INTERFACE(nativeFocusChanged)(", s.find("SDL_JAVA_INTERFACE(nativeFocusChanged)(") + 10)
assert i > 0, "nativeFocusChanged definition not found"
brace = s.find("{", i)
body_start = brace + 1
if MARK not in s[body_start:body_start + 200]:
    s = s[:body_start] + "\n    if (QXR_Enabled() && !hasFocus) { " + MARK + "\n        return;\n    }" + s[body_start:]
    p.write_text(s)
    print("  patched: nativeFocusChanged")
else:
    print("  already: nativeFocusChanged")

# ---- hide the real Touch controllers (dead Android gamepads in immersive mode)
include("src/joystick/android/SDL_sysjoystick.c", '#include "SDL_sysjoystick_c.h"\n',
        '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n")
edit("src/joystick/android/SDL_sysjoystick.c",
     "    if (JoystickByDeviceId(device_id) != NULL || name == NULL) {\n        return -1;\n    }\n",
     "    if (JoystickByDeviceId(device_id) != NULL || name == NULL) {\n        return -1;\n    }\n"
     "    if (QXR_IgnoreAndroidJoystick(name, vendor_id)) { " + MARK + "\n"
     "        SDL_Log(\"QuestXR: ignoring Android joystick '%s' (vendor 0x%04x)\", name, vendor_id);\n"
     "        return -1;\n    }\n")

# ---- virtual pad: name + attach at joystick init
edit("src/joystick/virtual/SDL_virtualjoystick.c",
     '    hwdata->name = "Virtual Joystick";\n',
     '    hwdata->name = "Quest Touch"; ' + MARK + "\n")
include("src/joystick/SDL_joystick.c", '#include "SDL_sysjoystick.h"\n',
        '#include "../../../questxr/rvgl_questxr.h" ' + MARK + "\n")
edit("src/joystick/SDL_joystick.c",
     "            status = 0;\n        }\n    }\n    return status;\n}\n",
     "            status = 0;\n        }\n    }\n    QXR_OnJoystickInit(); " + MARK + "\n    return status;\n}\n")
# ---- Android.mk: build the VR host into libSDL2.so, link EGL/GLES3 + OpenXR loader
edit("Android.mk",
     "LOCAL_CFLAGS += -DGL_GLEXT_PROTOTYPES\n",
     "LOCAL_CFLAGS += -DGL_GLEXT_PROTOTYPES\n"
     "# QUESTXR\n"
     "LOCAL_SRC_FILES += ../questxr/rvgl_questxr.c\n"
     "LOCAL_C_INCLUDES += $(LOCAL_PATH)/../questxr $(LOCAL_PATH)/../openxr/include\n"
     "LOCAL_SHARED_LIBRARIES += openxr_loader\n")
edit("Android.mk",
     "LOCAL_LDLIBS := -ldl -lGLESv1_CM -lGLESv2 -lOpenSLES -llog -landroid\n",
     "LOCAL_LDLIBS := -ldl -lGLESv1_CM -lGLESv2 -lGLESv3 -lEGL -lOpenSLES -llog -landroid # QUESTXR\n")
print("done")
