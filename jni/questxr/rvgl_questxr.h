/* RVGL Quest VR host: lives inside libSDL2.so so the closed RVGL engine
 * (libmain.so) needs no changes. Hooks are called from patched SDL sources. */
#ifndef RVGL_QUESTXR_H
#define RVGL_QUESTXR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Size the game sees as its window / default framebuffer. */
#define QXR_GAME_W 1920
#define QXR_GAME_H 1080

int   QXR_Enabled(void);
/* Called right before / after SDL creates the GL context. */
void  QXR_BeforeCreateContext(int *major, int *minor, int *profile_mask);
void  QXR_OnContextCreated(void);
/* Wrap GL entry points the engine fetches (framebuffer 0 -> offscreen FBO). */
void *QXR_WrapProc(const char *name, void *real);
/* Present the frame to OpenXR. Returns 1 when the real swap must be skipped. */
int   QXR_Present(void);
/* Real Quest Touch controllers show up as dead Android gamepads: hide them. */
int   QXR_IgnoreAndroidJoystick(const char *name, int vendor_id);
/* Called at the end of SDL_JoystickInit: attach the Touch virtual pad. */
void  QXR_OnJoystickInit(void);

#ifdef __cplusplus
}
#endif
#endif
