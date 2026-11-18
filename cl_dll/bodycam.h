// bodycam.h - Bodycam mode for SourceSurge_GL (GoldSrc / Xash3D client DLL)
#pragma once

struct ref_params_s;

void BodyCam_Init(void);                              // CHud::Init
void BodyCam_CalcView(struct ref_params_s *pparams);  // end of V_CalcNormalRefdef
void BodyCam_PostProcess(void);                       // start of CHud::Redraw (fisheye, CA, vignette, grain)
void BodyCam_DrawOverlay(void);                       // end of CHud::Redraw (REC, timestamp...)
int  BodyCam_Active(void);
int  BodyCam_HideHud(void);
