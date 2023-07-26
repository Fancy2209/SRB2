#include "../doomdef.h"
#include "../command.h"
#include "../i_video.h"
#include "../v_video.h"
#include "../screen.h"

#include "i_video.h"

#include "utils/Log.h"

rendermode_t rendermode = render_soft;
rendermode_t chosenrendermode = render_none;

boolean highcolor = false;

boolean allow_fullscreen = false;



consvar_t cv_vidwait = CVAR_INIT ("vid_wait", "On", CV_SAVE, CV_OnOff, NULL);

void I_StartupGraphics(void){}
void I_ShutdownGraphics(void){}

void VID_StartupOpenGL(void){}

void I_SetPalette(RGBA_t *palette)
{
  (void)palette;
}

INT32 VID_SetMode(INT32 modenum)
{
  vid.width = 320;
  vid.height = 240;
  vid.bpp = 1;
  vid.buffer = android_surface;
  return 0;
}

boolean VID_CheckRenderer(void)
{
	return false;
}

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
}

void I_UpdateNoBlit(void){}

void I_FinishUpdate(void) {
  LOGD("FRAME!");
  (*jni_env)->CallVoidMethod(jni_env, androidVideo, videoFrameCB);
}

void I_UpdateNoVsync(void) {}

void I_WaitVBL(INT32 count)
{
  (void)count;
}

void I_ReadScreen(UINT8 *scr)
{
  (void)scr;
}

void I_BeginRead(void){}

void I_EndRead(void){}
