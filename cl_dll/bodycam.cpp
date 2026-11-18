// bodycam.cpp - Bodycam mode for SourceSurge_GL (cl_dll)
//
// Console:   bodycam            -> toggle
//            bodycam 1 / 0      -> on / off
//
// INTEGRATION (4 one-line hooks, see bottom of file for details):
//   1. hud.cpp      CHud::Init()              -> BodyCam_Init();
//   2. view.cpp     end of V_CalcNormalRefdef -> BodyCam_CalcView(pparams);
//   3. hud_redraw   start of CHud::Redraw     -> BodyCam_PostProcess();
//   4. hud_redraw   end of CHud::Redraw       -> BodyCam_DrawOverlay();
//
// Needs OpenGL (opengl32.lib on Windows / -lGL on Linux) for the fisheye pass.
// Set BODYCAM_USE_GL to 0 to build without it (view + viewmodel + text overlay only).

#define BODYCAM_USE_GL 1

#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "bodycam.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if BODYCAM_USE_GL
// NOTE: do NOT include <windows.h> here, it clashes with HSPRITE from hud.h.
// gl.h only needs these three macros.
#ifdef _WIN32
#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#endif
#ifndef CALLBACK
#define CALLBACK __stdcall
#endif
#pragma comment(lib, "opengl32.lib") // links GL without touching the project settings
#endif
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#endif

#define BC_PI 3.14159265358979f

// ---------------------------------------------------------------------------
// small math helpers (self-contained)
// ---------------------------------------------------------------------------
static inline float BC_Clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static float BC_AngDiff(float a, float b)
{
	float d = fmodf(a - b, 360.0f);
	if (d > 180.0f) d -= 360.0f;
	if (d < -180.0f) d += 360.0f;
	return d;
}

static void BC_AngleVectors(const float *ang, float *f, float *r, float *u)
{
	float sp = sinf(ang[0] * BC_PI / 180.0f), cp = cosf(ang[0] * BC_PI / 180.0f);
	float sy = sinf(ang[1] * BC_PI / 180.0f), cy = cosf(ang[1] * BC_PI / 180.0f);
	float sr = sinf(ang[2] * BC_PI / 180.0f), cr = cosf(ang[2] * BC_PI / 180.0f);

	f[0] = cp * cy;                      f[1] = cp * sy;                      f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy;      r[1] = -sr * sp * sy - cr * cy;      r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy;       u[1] = cr * sp * sy - sr * cy;       u[2] = cr * cp;
}

// ---------------------------------------------------------------------------
// state / cvars
// ---------------------------------------------------------------------------
static struct
{
	int    active;
	float  startTime;
	float  savedFov;

	// view state
	int    angInit;
	float  cam[3];        // lagged "chest" angles
	float  phase;         // walk cycle
	float  speedFrac;     // smoothed 0..1
	float  landKick;
	float  airVelZ;
	int    wasGround;

#if BODYCAM_USE_GL
	unsigned int scrTex, noiseTex;
	int    texW, texH;
#endif
} bc;

static cvar_t *cv_fov, *cv_fisheye, *cv_ca, *cv_vignette, *cv_grain, *cv_bob, *cv_sway,
              *cv_lean, *cv_drop, *cv_hud, *cv_id, *cv_vmx, *cv_vmy, *cv_vmz;

int BodyCam_Active(void)  { return bc.active; }
int BodyCam_HideHud(void) { return bc.active && cv_hud && cv_hud->value == 0.0f; }

static void BodyCam_Set(int on)
{
	if (on == bc.active)
		return;

	cvar_t *dfov = gEngfuncs.pfnGetCvarPointer("default_fov");

	if (on)
	{
		bc.savedFov = dfov ? dfov->value : 90.0f;
		gEngfuncs.Cvar_SetValue("default_fov", cv_fov->value);
		bc.angInit = 0;
		bc.phase = bc.speedFrac = bc.landKick = 0.0f;
		bc.wasGround = 1;
		bc.startTime = gEngfuncs.GetClientTime();
		bc.active = 1;
		gEngfuncs.Con_Printf("BodyCam: ON\n");
	}
	else
	{
		bc.active = 0;
		gEngfuncs.Cvar_SetValue("default_fov", bc.savedFov);
		gEngfuncs.Con_Printf("BodyCam: OFF\n");
	}
}

static void BC_Cmd(void)
{
	int want = !bc.active;
	if (gEngfuncs.Cmd_Argc() > 1)
		want = atoi(gEngfuncs.Cmd_Argv(1)) != 0;
	BodyCam_Set(want);
}

void BodyCam_Init(void)
{
	cv_fov      = gEngfuncs.pfnRegisterVariable("bodycam_fov",      "105",       FCVAR_ARCHIVE);
	cv_fisheye  = gEngfuncs.pfnRegisterVariable("bodycam_fisheye",  "0.28",      FCVAR_ARCHIVE);
	cv_ca       = gEngfuncs.pfnRegisterVariable("bodycam_ca",       "0.012",     FCVAR_ARCHIVE);
	cv_vignette = gEngfuncs.pfnRegisterVariable("bodycam_vignette", "0.55",      FCVAR_ARCHIVE);
	cv_grain    = gEngfuncs.pfnRegisterVariable("bodycam_grain",    "0.05",      FCVAR_ARCHIVE);
	cv_bob      = gEngfuncs.pfnRegisterVariable("bodycam_bob",      "1.0",       FCVAR_ARCHIVE);
	cv_sway     = gEngfuncs.pfnRegisterVariable("bodycam_sway",     "1.0",       FCVAR_ARCHIVE);
	cv_lean     = gEngfuncs.pfnRegisterVariable("bodycam_lean",     "0.06",      FCVAR_ARCHIVE);
	cv_drop     = gEngfuncs.pfnRegisterVariable("bodycam_drop",     "9",         FCVAR_ARCHIVE);
	cv_hud      = gEngfuncs.pfnRegisterVariable("bodycam_hud",      "0",         FCVAR_ARCHIVE);
	cv_id       = gEngfuncs.pfnRegisterVariable("bodycam_id",       "UNIT 0417", FCVAR_ARCHIVE);
	cv_vmx      = gEngfuncs.pfnRegisterVariable("bodycam_vm_x",     "2.0",       FCVAR_ARCHIVE); // right
	cv_vmy      = gEngfuncs.pfnRegisterVariable("bodycam_vm_y",     "1.0",       FCVAR_ARCHIVE); // forward
	cv_vmz      = gEngfuncs.pfnRegisterVariable("bodycam_vm_z",     "-1.5",      FCVAR_ARCHIVE); // up
	gEngfuncs.pfnAddCommand("bodycam", BC_Cmd);
}

// ---------------------------------------------------------------------------
// VIEW + VIEWMODEL
// Call at the very end of V_CalcNormalRefdef (after the viewmodel was set up).
//
// Idea: the camera is on the chest, so
//   - it sits lower than the eyes,
//   - it follows aim angles with a spring (the torso lags behind the head/weapon),
//   - it bobs / rolls with the walk cycle + has a constant handheld micro-shake,
//   - the weapon is held in the hands: it uses the TRUE aim angles and its own
//     bob phase, so when you turn the gun visibly leads the picture.
// ---------------------------------------------------------------------------
void BodyCam_CalcView(struct ref_params_s* p)
{
	if (!bc.active || p->intermission || p->paused)
		return;

	cl_entity_t* vm = gEngfuncs.GetViewModel();
	float dt = BC_Clamp(p->frametime, 0.0f, 0.1f);
	float t = p->time;

	float trueAng[3] = {p->viewangles[0], p->viewangles[1], p->viewangles[2]};
	float base[3] = {p->vieworg[0], p->vieworg[1], p->vieworg[2]};

	if (!bc.angInit)
	{
		bc.cam[0] = trueAng[0];
		bc.cam[1] = trueAng[1];
		bc.cam[2] = 0.0f;
		bc.angInit = 1;
	}

	// ---- movement state ---------------------------------------------------
	float speed = sqrtf(p->simvel[0] * p->simvel[0] + p->simvel[1] * p->simvel[1]);
	int ground = (p->onground != -1);

	float target = ground ? BC_Clamp(speed / 260.0f, 0.0f, 1.0f) : 0.0f;
	bc.speedFrac += (target - bc.speedFrac) * (1.0f - expf(-dt * 8.0f));

	if (ground)
		bc.phase += dt * (speed / 104.0f) * 2.0f * BC_PI; // one full stride = 104 units
	if (bc.phase > 2000.0f * BC_PI)
		bc.phase = 0.0f;

	// landing kick
	if (!ground)
		bc.airVelZ = p->simvel[2];
	if (ground && !bc.wasGround)
		bc.landKick = BC_Clamp(-bc.airVelZ / 450.0f, 0.0f, 1.0f);
	bc.wasGround = ground;
	bc.landKick *= expf(-dt * 6.5f);

	// ---- chest camera: spring-lagged angles -------------------------------
	float sway = BC_Clamp(cv_sway->value, 0.0f, 3.0f);
	if (sway < 0.01f)
	{
		bc.cam[0] = trueAng[0];
		bc.cam[1] = trueAng[1];
	}
	else
	{
		float ay = 1.0f - expf(-dt * 10.0f / sway);
		float ap = 1.0f - expf(-dt * 16.0f / sway);
		bc.cam[1] += BC_AngDiff(trueAng[1], bc.cam[1]) * ay;
		bc.cam[0] += BC_AngDiff(trueAng[0], bc.cam[0]) * ap;

		// never let the weapon leave the frame completely on fast flicks
		float dy = BC_AngDiff(trueAng[1], bc.cam[1]);
		float dp = BC_AngDiff(trueAng[0], bc.cam[0]);
		const float maxY = 28.0f, maxP = 18.0f;
		if (dy > maxY)
			bc.cam[1] = trueAng[1] - maxY;
		if (dy < -maxY)
			bc.cam[1] = trueAng[1] + maxY;
		if (dp > maxP)
			bc.cam[0] = trueAng[0] - maxP;
		if (dp < -maxP)
			bc.cam[0] = trueAng[0] + maxP;
	}

	// residual lag -> banking (flip the sign of bodycam_lean if it feels inverted)
	float lagYaw = BC_AngDiff(trueAng[1], bc.cam[1]);
	float lean = BC_Clamp(lagYaw * cv_lean->value, -6.0f, 6.0f);

	// ---- bob / handheld shake --------------------------------------------
	float bobS = cv_bob->value * bc.speedFrac;
	float s1 = sinf(bc.phase), s2 = sinf(bc.phase * 2.0f);

	float camVert = s2 * 0.45f * bobS; // units
	float camSide = s1 * 0.30f * bobS;
	float camPitch = sinf(bc.phase * 2.0f + 0.8f) * 0.35f * bobS; // degrees
	float camRoll = s1 * 0.45f * bobS;

	float gunVert = sinf(bc.phase * 2.0f + 1.6f) * 0.35f * bobS; // hands bob on their own phase
	float gunSide = sinf(bc.phase + 0.9f) * 0.50f * bobS;
	float gunPitch = sinf(bc.phase * 2.0f + 2.2f) * 0.60f * bobS;

	float hx = sinf(t * 1.7f) * 0.6f + sinf(t * 3.1f + 1.3f) * 0.4f + sinf(t * 7.3f + 0.4f) * 0.15f;
	float hy = sinf(t * 1.3f + 2.0f) * 0.6f + sinf(t * 2.7f) * 0.4f + sinf(t * 6.1f + 1.1f) * 0.15f;
	float hr = sinf(t * 0.9f + 0.7f) * 0.5f + sinf(t * 2.3f) * 0.3f;
	float jit = 0.08f + 0.10f * bc.speedFrac;

	// ---- final camera -----------------------------------------------------
	float camAng[3];
	camAng[0] = BC_Clamp(bc.cam[0] + camPitch + hx * jit + bc.landKick * 3.0f, -89.0f, 89.0f);
	camAng[1] = bc.cam[1] + hy * jit;
	camAng[2] = trueAng[2] + camRoll + lean + hr * jit * 1.5f;

	float drop = cv_drop->value + bc.landKick * 2.5f;
	float cf[3], cr[3], cu[3];
	BC_AngleVectors(camAng, cf, cr, cu);

	float camOrg[3] = {base[0], base[1], base[2] - drop};
	for (int i = 0; i < 3; i++)
		camOrg[i] += cr[i] * camSide + cu[i] * camVert;

	p->vieworg[0] = camOrg[0];
	p->vieworg[1] = camOrg[1];
	p->vieworg[2] = camOrg[2];
	p->viewangles[0] = camAng[0];
	p->viewangles[1] = camAng[1];
	p->viewangles[2] = camAng[2];

	// ---- viewmodel --------------------------------------------------------
	if (vm)
	{
		// frame of the un-bobbed chest, so the hands move relative to the picture
		float gAng[3] = {bc.cam[0], bc.cam[1], 0.0f};
		float gf[3], gr[3], gu[3];
		BC_AngleVectors(gAng, gf, gr, gu);

		float gBase[3] = {base[0], base[1], base[2] - drop};
		for (int i = 0; i < 3; i++)
		{
			vm->origin[i] = gBase[i] + gf[i] * cv_vmy->value + gr[i] * (cv_vmx->value + gunSide) + gu[i] * (cv_vmz->value + gunVert);
			vm->curstate.origin[i] = vm->origin[i];
			vm->prevstate.origin[i] = vm->origin[i];
		}

		// angles stay on the TRUE aim (the gun leads the lagged camera).
		// NB: GoldSrc stores the viewmodel pitch inverted, hence the minus.
		// Indices: 0 = pitch, 2 = roll.
		vm->angles[0] -= gunPitch;
		vm->angles[2] += lean * 0.5f + hr * jit;
		vm->curstate.angles[0] = vm->angles[0];
		vm->curstate.angles[1] = vm->angles[1];
		vm->curstate.angles[2] = vm->angles[2];
	}
}

// ---------------------------------------------------------------------------
// POST-PROCESS: fisheye + chromatic aberration + vignette + grain (OpenGL)
// Call at the START of CHud::Redraw: the 3D scene is done, the HUD is not yet drawn.
// ---------------------------------------------------------------------------
#if BODYCAM_USE_GL

static int BC_NextPow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static void BC_EnsureTextures(void)
{
	if (!bc.noiseTex)
	{
		static unsigned char noise[128 * 128];
		for (int i = 0; i < 128 * 128; i++)
			noise[i] = (unsigned char)(rand() & 255);

		glGenTextures(1, &bc.noiseTex);
		glBindTexture(GL_TEXTURE_2D, bc.noiseTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 128, 128, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, noise);
	}

	int w = BC_NextPow2(ScreenWidth), h = BC_NextPow2(ScreenHeight);
	if (!bc.scrTex || w != bc.texW || h != bc.texH)
	{
		if (!bc.scrTex)
			glGenTextures(1, &bc.scrTex);
		glBindTexture(GL_TEXTURE_2D, bc.scrTex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		bc.texW = w; bc.texH = h;
	}
}

// barrel distortion: output point (u,v) in 0..1 -> source texcoord
static void BC_Distort(float u, float v, float k, float *tu, float *tv)
{
	float x = (u - 0.5f) * 2.0f, y = (v - 0.5f) * 2.0f;
	float r2 = (x * x + y * y) * 0.5f;                 // 1.0 in the corners
	float f = (1.0f + k * r2) / (1.0f + k);             // corners stay on the corners
	float sx = x * f, sy = y * f;
	*tu = (0.5f + sx * 0.5f) * (float)ScreenWidth  / (float)bc.texW;
	*tv = (0.5f + sy * 0.5f) * (float)ScreenHeight / (float)bc.texH;
}

static void BC_DrawDistortedMesh(float k)
{
	const int N = 32, M = 20;
	for (int j = 0; j < M; j++)
	{
		glBegin(GL_TRIANGLE_STRIP);
		for (int i = 0; i <= N; i++)
		{
			float u = (float)i / N, tu, tv;
			float v0 = (float)j / M, v1 = (float)(j + 1) / M;

			BC_Distort(u, v1, k, &tu, &tv); glTexCoord2f(tu, tv); glVertex2f(u, v1);
			BC_Distort(u, v0, k, &tu, &tv); glTexCoord2f(tu, tv); glVertex2f(u, v0);
		}
		glEnd();
	}
}

void BodyCam_PostProcess(void)
{
	if (!bc.active)
		return;

	GLint prevTex = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT |
				 GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT | GL_LIGHTING_BIT);

	BC_EnsureTextures();

	// grab the finished 3D frame
	glBindTexture(GL_TEXTURE_2D, bc.scrTex);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, ScreenWidth, ScreenHeight);

	glViewport(0, 0, ScreenWidth, ScreenHeight);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_LIGHTING);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glShadeModel(GL_SMOOTH); // the engine can leave GL_FLAT on -> vignette spikes
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	// --- distorted scene, one pass per channel = chromatic aberration -------
	float k = BC_Clamp(cv_fisheye->value, 0.0f, 1.5f);
	float ca = BC_Clamp(cv_ca->value, 0.0f, 0.1f);

	glBindTexture(GL_TEXTURE_2D, bc.scrTex);
	glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
	BC_DrawDistortedMesh(k * (1.0f + ca * 10.0f));
	glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
	BC_DrawDistortedMesh(k);
	glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
	BC_DrawDistortedMesh(k * (1.0f - ca * 10.0f));
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	// --- vignette ------------------------------------------------------------
	float vig = BC_Clamp(cv_vignette->value, 0.0f, 1.0f);
	if (vig > 0.0f)
	{
		glDisable(GL_TEXTURE_2D);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		const int S = 48;
		const float r0 = 0.30f, r1 = 0.85f;
		glBegin(GL_TRIANGLE_STRIP);
		for (int i = 0; i <= S; i++)
		{
			float a = (float)i / S * 2.0f * BC_PI, c = cosf(a), s = sinf(a);
			glColor4f(0, 0, 0, 0);
			glVertex2f(0.5f + c * r0, 0.5f + s * r0);
			glColor4f(0, 0, 0, vig);
			glVertex2f(0.5f + c * r1, 0.5f + s * r1);
		}
		glEnd();
	}

	// --- film / sensor grain --------------------------------------------------
	float grain = BC_Clamp(cv_grain->value, 0.0f, 0.5f);
	if (grain > 0.0f)
	{
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, bc.noiseTex);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glColor4f(1, 1, 1, grain);

		float ox = (float)rand() / RAND_MAX, oy = (float)rand() / RAND_MAX;
		float su = (float)ScreenWidth / (128.0f * 1.5f), sv = (float)ScreenHeight / (128.0f * 1.5f);
		glBegin(GL_QUADS);
		glTexCoord2f(ox, oy);
		glVertex2f(0, 0);
		glTexCoord2f(ox + su, oy);
		glVertex2f(1, 0);
		glTexCoord2f(ox + su, oy + sv);
		glVertex2f(1, 1);
		glTexCoord2f(ox, oy + sv);
		glVertex2f(0, 1);
		glEnd();
	}

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopAttrib();

	// the engine caches its bound texture: put the old one back
	glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
}

#else
void BodyCam_PostProcess(void) {}
#endif // BODYCAM_USE_GL

// ---------------------------------------------------------------------------
// OVERLAY: REC light, timestamp, unit id, elapsed time
// Call at the END of CHud::Redraw.
// ---------------------------------------------------------------------------
static void BC_TextRight(int xRight, int y, const char *s)
{
	int w = 0, h = 0;
	gEngfuncs.pfnDrawConsoleStringLen(s, &w, &h);
	gEngfuncs.pfnDrawConsoleString(xRight - w, y, (char *)s);
}

void BodyCam_DrawOverlay(void)
{
	if (!bc.active)
		return;

	int mx = ScreenWidth / 32, my = ScreenHeight / 24;
	int lh = 16;
	char buf[96];

	gEngfuncs.pfnDrawSetTextColor(0.92f, 0.92f, 0.92f);

	// top-left: date + time, unit id
	time_t now = time(NULL);
	struct tm *lt = localtime(&now);
	if (lt)
	{
		strftime(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S", lt);
		gEngfuncs.pfnDrawConsoleString(mx, my, buf);
	}
	gEngfuncs.pfnDrawConsoleString(mx, my + lh, (char *)cv_id->string);

	// top-right: blinking REC + elapsed
	int el = (int)(gEngfuncs.GetClientTime() - bc.startTime);
	if (el < 0) el = 0;

	if (((int)(gEngfuncs.GetClientTime() * 1.25f) & 1) == 0)
		gEngfuncs.pfnFillRGBA(ScreenWidth - mx - 58, my + 3, 9, 9, 230, 20, 20, 255);

	BC_TextRight(ScreenWidth - mx, my, "REC");
	snprintf(buf, sizeof(buf), "%02d:%02d:%02d", el / 3600, (el / 60) % 60, el % 60);
	BC_TextRight(ScreenWidth - mx, my + lh, buf);
}

// ---------------------------------------------------------------------------
// INTEGRATION DETAILS
//
// hud.cpp, CHud::Init():                 BodyCam_Init();
//
// view.cpp, V_CalcNormalRefdef(), at the very end (after the viewmodel code):
//                                        BodyCam_CalcView(pparams);
//
// hud_redraw.cpp, CHud::Redraw(), right after the time bookkeeping:
//                                        BodyCam_PostProcess();
//                                        if (BodyCam_HideHud()) { BodyCam_DrawOverlay(); return 1; }
//   ...and just before the final `return 1;`:
//                                        BodyCam_DrawOverlay();
//
// Add #include "bodycam.h" to those three files and bodycam.cpp to the cl_dll build
// (wscript / CMakeLists). Link GL: opengl32 (Windows) or GL (Linux).
//
// Tuning cvars: bodycam_fov, bodycam_fisheye, bodycam_ca, bodycam_vignette, bodycam_grain,
//               bodycam_bob, bodycam_sway, bodycam_lean, bodycam_drop, bodycam_hud,
//               bodycam_id, bodycam_vm_x / _y / _z (viewmodel right / forward / up).
// ---------------------------------------------------------------------------
