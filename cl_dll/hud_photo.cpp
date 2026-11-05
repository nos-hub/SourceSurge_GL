// hud_photo.cpp - photo camera: capture, upload and photo decals (client side, cl_dll/)
//
// Capture : on "PhotoCap" the viewmodel is hidden for a frame or two, then at the top of the next
//           CHud::Redraw the centre square of the 3D view is read back with glReadPixels, shrunk to
//           PHOTO_W x PHOTO_H, dithered to RGB332 and uploaded in chunks ("ss_photo ...").
// Receive : "PhotoDat" chunks (relayed by the server) are collected per player.
// Show    : "PhotoSpr" places a decal. Its texture is a copy of the photo at that moment, so a newer
//           photo from the same player never changes sprays already on walls. At most MAX_DECALS
//           stay up, the oldest is replaced first.
//
// OpenGL functions are fetched with SDL_GL_GetProcAddress (SDL2 is already linked by the client),
// so the client needs no GL link dependency. Without OpenGL (software renderer) capture reports
// failure and the player simply keeps the normal spray.

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "const.h"
#include "triangleapi.h"
#include "cl_entity.h"

#include <SDL2/SDL_video.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

DECLARE_MESSAGE(m_Photo, PhotoCap)
DECLARE_MESSAGE(m_Photo, PhotoDat)
DECLARE_MESSAGE(m_Photo, PhotoSpr)

// ---------------- minimal OpenGL loader ----------------
#ifdef _WIN32
#define PH_GLAPI __stdcall
#else
#define PH_GLAPI
#endif

namespace
{
typedef unsigned int GLenum_;
typedef int GLint_;
typedef int GLsizei_;
typedef unsigned int GLuint_;
typedef float GLfloat_;

constexpr GLenum_ GL_RGB_ = 0x1907;
constexpr GLenum_ GL_RGBA_ = 0x1908;
constexpr GLenum_ GL_UNSIGNED_BYTE_ = 0x1401;
constexpr GLenum_ GL_PACK_ALIGNMENT_ = 0x0D05;
constexpr GLenum_ GL_UNPACK_ALIGNMENT_ = 0x0CF5;
constexpr GLenum_ GL_TEXTURE_2D_ = 0x0DE1;
constexpr GLenum_ GL_TEXTURE_MIN_FILTER_ = 0x2801;
constexpr GLenum_ GL_TEXTURE_MAG_FILTER_ = 0x2800;
constexpr GLenum_ GL_TEXTURE_WRAP_S_ = 0x2802;
constexpr GLenum_ GL_TEXTURE_WRAP_T_ = 0x2803;
constexpr GLint_ GL_LINEAR_ = 0x2601;
constexpr GLint_ GL_CLAMP_TO_EDGE_ = 0x812F;
constexpr GLenum_ GL_TEXTURE_BINDING_2D_ = 0x8069;
constexpr GLenum_ GL_POLYGON_OFFSET_FILL_ = 0x8037;
constexpr GLenum_ GL_VIEWPORT_ = 0x0BA2;

struct GLFuncs
{
	bool tried = false;
	bool ok = false;
	void(PH_GLAPI* ReadPixels)(GLint_, GLint_, GLsizei_, GLsizei_, GLenum_, GLenum_, void*) = nullptr;
	void(PH_GLAPI* GenTextures)(GLsizei_, GLuint_*) = nullptr;
	void(PH_GLAPI* DeleteTextures)(GLsizei_, const GLuint_*) = nullptr;
	void(PH_GLAPI* BindTexture)(GLenum_, GLuint_) = nullptr;
	void(PH_GLAPI* TexImage2D)(GLenum_, GLint_, GLint_, GLsizei_, GLsizei_, GLint_, GLenum_, GLenum_, const void*) = nullptr;
	void(PH_GLAPI* TexParameteri)(GLenum_, GLenum_, GLint_) = nullptr;
	void(PH_GLAPI* PixelStorei)(GLenum_, GLint_) = nullptr;
	void(PH_GLAPI* GetIntegerv)(GLenum_, GLint_*) = nullptr;
	void(PH_GLAPI* Enable)(GLenum_) = nullptr;
	void(PH_GLAPI* Disable)(GLenum_) = nullptr;
	void(PH_GLAPI* PolygonOffset)(GLfloat_, GLfloat_) = nullptr;
};
GLFuncs gl;

template <typename T>
bool Load(T& fn, const char* name)
{
	fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
	return fn != nullptr;
}

bool LoadGL()
{
	if (gl.tried)
		return gl.ok;
	gl.tried = true;
	gl.ok = Load(gl.ReadPixels, "glReadPixels") && Load(gl.GenTextures, "glGenTextures") && Load(gl.DeleteTextures, "glDeleteTextures") && Load(gl.BindTexture, "glBindTexture") && Load(gl.TexImage2D, "glTexImage2D") && Load(gl.TexParameteri, "glTexParameteri") && Load(gl.PixelStorei, "glPixelStorei") && Load(gl.GetIntegerv, "glGetIntegerv") && Load(gl.Enable, "glEnable") && Load(gl.Disable, "glDisable") && Load(gl.PolygonOffset, "glPolygonOffset");
	if (!gl.ok)
		gEngfuncs.Con_Printf("photo camera: OpenGL functions not available, photos are disabled\n");
	return gl.ok;
}

// RGB332 -> RGBA lookup
void Expand332(const unsigned char* src, unsigned char* dst /* PHOTO_W*PHOTO_H*4 */)
{
	for (int i = 0; i < PHOTO_BYTES; i++)
	{
		const int v = src[i];
		dst[i * 4 + 0] = (unsigned char)(((v >> 5) & 7) * 255 / 7);
		dst[i * 4 + 1] = (unsigned char)(((v >> 2) & 7) * 255 / 7);
		dst[i * 4 + 2] = (unsigned char)((v & 3) * 255 / 3);
		dst[i * 4 + 3] = 255;
	}
}

void Cross(const float* a, const float* b, float* out)
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

bool Normalize(float* v)
{
	const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len < 1e-4f)
		return false;
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
	return true;
}

int LocalPlayerIndex()
{
	cl_entity_t* p = gEngfuncs.GetLocalPlayer();
	return p ? p->index : 0;
}
} // namespace

// ---------------- element ----------------

bool CHudPhoto::Init()
{
	HOOK_MESSAGE(PhotoCap);
	HOOK_MESSAGE(PhotoDat);
	HOOK_MESSAGE(PhotoSpr);

	m_pCvarSize = CVAR_CREATE("ss_photo_size", "64", FCVAR_ARCHIVE); // width of a sprayed photo in world units

	m_iFlags |= HUD_ACTIVE;
	gHUD.AddHudElem(this);
	return true;
}

bool CHudPhoto::VidInit()
{
	ClearAll();
	return true;
}

void CHudPhoto::FreeDecal(Decal& d)
{
	if (d.tex != 0 && gl.ok)
		gl.DeleteTextures(1, &d.tex);
	d = Decal();
}

void CHudPhoto::ClearAll()
{
	for (auto& d : m_Decals)
		FreeDecal(d);
	for (auto& r : m_Rx)
		r = RxPhoto();
	RestoreViewModel();
	m_iPendingId = 0;
	m_iUpId = 0;
	m_flFlashStart = -100.0f;
}

void CHudPhoto::RestoreViewModel()
{
	if (!m_bVMHidden)
		return;
	m_bVMHidden = false;
	gEngfuncs.Cvar_SetValue("r_drawviewmodel", m_flOldVM);
}

bool CHudPhoto::Draw(float flTime)
{
	const float t = gEngfuncs.GetClientTime() - m_flFlashStart;
	if (t < 0.0f || t > 0.25f)
		return true;

	const int alpha = (int)(255.0f * (1.0f - t / 0.25f));
	gEngfuncs.pfnFillRGBA(0, 0, ScreenWidth, ScreenHeight, 255, 255, 255, alpha);
	return true;
}

// ---------------- messages ----------------

bool CHudPhoto::MsgFunc_PhotoCap(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const short id = (short)READ_SHORT();

	m_iPendingId = id;
	m_iCapDelay = 2; // frames to wait with the viewmodel hidden
	m_iUpId = 0;	 // a new picture cancels an upload still in progress

	// hide the viewmodel so the photo doesn't have the camera in it
	if (!m_bVMHidden)
	{
		cvar_t* cv = gEngfuncs.pfnGetCvarPointer("r_drawviewmodel");
		if (cv)
		{
			m_flOldVM = cv->value;
			gEngfuncs.Cvar_SetValue("r_drawviewmodel", 0);
			m_bVMHidden = true;
		}
	}
	m_flVMRestoreAt = gEngfuncs.GetClientTime() + 2.0f; // safety net
	return true;
}

bool CHudPhoto::MsgFunc_PhotoDat(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int idx = READ_BYTE();
	const short id = (short)READ_SHORT();
	const int chunk = READ_BYTE();

	unsigned char bytes[PHOTO_CHUNK_BYTES];
	for (int i = 0; i < PHOTO_CHUNK_BYTES; i++)
		bytes[i] = (unsigned char)READ_BYTE();

	if (idx < 1 || idx >= MAX_PLAYERS_SLOTS || chunk < 0 || chunk >= PHOTO_CHUNKS)
		return true;
	if (idx == LocalPlayerIndex())
		return true; // our own photo is already here

	RxPhoto& r = m_Rx[idx];
	if (r.id != id)
	{
		r = RxPhoto();
		r.id = id;
	}
	if (r.got[chunk])
		return true;

	memcpy(r.data + chunk * PHOTO_CHUNK_BYTES, bytes, PHOTO_CHUNK_BYTES);
	r.got[chunk] = true;
	if (++r.nRecv >= PHOTO_CHUNKS)
		r.complete = true;
	return true;
}

bool CHudPhoto::MsgFunc_PhotoSpr(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int idx = READ_BYTE();
	const short id = (short)READ_SHORT();

	float pos[3], n[3];
	for (int i = 0; i < 3; i++)
		pos[i] = READ_LONG() / 16.0f;
	for (int i = 0; i < 3; i++)
		n[i] = READ_CHAR() / 127.0f;
	const float yaw = READ_BYTE() * (360.0f / 256.0f);

	if (!Normalize(n))
	{
		n[0] = 0;
		n[1] = 0;
		n[2] = 1;
	}

	// orientation: "up" of the picture
	float up[3];
	if (fabsf(n[2]) < 0.9f)
	{
		// wall: world up projected on the wall
		const float d = n[2];
		up[0] = -n[0] * d;
		up[1] = -n[1] * d;
		up[2] = 1.0f - n[2] * d;
	}
	else
	{
		// floor / ceiling: top of the picture points away from the player
		const float rad = yaw * (3.14159265f / 180.0f);
		const float f[3] = {cosf(rad), sinf(rad), 0.0f};
		const float d = f[0] * n[0] + f[1] * n[1] + f[2] * n[2];
		up[0] = f[0] - n[0] * d;
		up[1] = f[1] - n[1] * d;
		up[2] = f[2] - n[2] * d;
	}
	if (!Normalize(up))
		return true;

	float right[3];
	Cross(up, n, right); // right x up = n (towards the viewer)

	// pick a slot: free one, else the oldest
	Decal* slot = nullptr;
	for (auto& d : m_Decals)
	{
		if (!d.used)
		{
			slot = &d;
			break;
		}
	}
	if (!slot)
	{
		slot = &m_Decals[0];
		for (auto& d : m_Decals)
			if (d.startTime < slot->startTime)
				slot = &d;
		FreeDecal(*slot);
	}

	slot->used = true;
	slot->ready = false;
	slot->owner = idx;
	slot->id = id;
	slot->startTime = gEngfuncs.GetClientTime();
	for (int i = 0; i < 3; i++)
	{
		slot->pos[i] = pos[i];
		slot->normal[i] = n[i];
		slot->up[i] = up[i];
		slot->right[i] = right[i];
	}

	LoadGL();
	TryMakeTexture(*slot); // if the pixels aren't complete yet, DrawDecals() retries
	return true;
}

// ---------------- capture + upload ----------------

bool CHudPhoto::CaptureScreen(unsigned char* out332)
{
	if (!LoadGL())
		return false;

	GLint_ vp[4] = {0, 0, 0, 0};
	gl.GetIntegerv(GL_VIEWPORT_, vp);

	const int side = vp[2] < vp[3] ? vp[2] : vp[3];
	if (side < PHOTO_W)
		return false;
	const int x0 = vp[0] + (vp[2] - side) / 2;
	const int y0 = vp[1] + (vp[3] - side) / 2;

	std::vector<unsigned char> raw((size_t)side * side * 3);

	GLint_ oldPack = 4;
	gl.GetIntegerv(GL_PACK_ALIGNMENT_, &oldPack);
	gl.PixelStorei(GL_PACK_ALIGNMENT_, 1);
	gl.ReadPixels(x0, y0, side, side, GL_RGB_, GL_UNSIGNED_BYTE_, raw.data());
	gl.PixelStorei(GL_PACK_ALIGNMENT_, oldPack);

	// 4x4 ordered dither matrix
	static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

	for (int dy = 0; dy < PHOTO_H; dy++)
	{
		// GL rows go bottom -> top; dy = 0 is the TOP row of the photo
		const int sy0 = dy * side / PHOTO_H;
		int sy1 = (dy + 1) * side / PHOTO_H;
		if (sy1 <= sy0)
			sy1 = sy0 + 1;

		for (int dx = 0; dx < PHOTO_W; dx++)
		{
			const int sx0 = dx * side / PHOTO_W;
			int sx1 = (dx + 1) * side / PHOTO_W;
			if (sx1 <= sx0)
				sx1 = sx0 + 1;

			int sum[3] = {0, 0, 0};
			int count = 0;
			for (int sy = sy0; sy < sy1; sy++)
			{
				const int glRow = (side - 1) - sy;
				const unsigned char* row = &raw[(size_t)glRow * side * 3];
				for (int sx = sx0; sx < sx1; sx++)
				{
					sum[0] += row[sx * 3 + 0];
					sum[1] += row[sx * 3 + 1];
					sum[2] += row[sx * 3 + 2];
					count++;
				}
			}

			const float dither = bayer[dy & 3][dx & 3] / 16.0f - 0.5f;
			int q[3];
			const int levels[3] = {8, 8, 4};
			for (int c = 0; c < 3; c++)
			{
				const float avg = (float)sum[c] / (float)count;
				const float step = 255.0f / (float)(levels[c] - 1);
				float v = (avg + dither * step) / step;
				int qi = (int)(v + 0.5f);
				if (qi < 0)
					qi = 0;
				if (qi > levels[c] - 1)
					qi = levels[c] - 1;
				q[c] = qi;
			}
			out332[dy * PHOTO_W + dx] = (unsigned char)((q[0] << 5) | (q[1] << 2) | q[2]);
		}
	}
	return true;
}

void CHudPhoto::StartUpload(short id)
{
	m_iUpId = id;
	m_iUpChunk = 0;
	m_flNextUpload = 0;
}

void CHudPhoto::PreRedraw()
{
	const float now = gEngfuncs.GetClientTime();

	if (m_bVMHidden && m_iPendingId == 0 && now > m_flVMRestoreAt)
		RestoreViewModel();
	if (m_bVMHidden && now > m_flVMRestoreAt + 1.0f)
		RestoreViewModel();

	// 1) capture
	if (m_iPendingId != 0)
	{
		if (m_iCapDelay > 0)
		{
			m_iCapDelay--;
		}
		else
		{
			const short id = m_iPendingId;
			m_iPendingId = 0;

			static unsigned char shot[PHOTO_BYTES];
			const bool ok = CaptureScreen(shot);
			RestoreViewModel();

			if (ok)
			{
				const int me = LocalPlayerIndex();
				if (me > 0 && me < MAX_PLAYERS_SLOTS)
				{
					RxPhoto& r = m_Rx[me];
					r = RxPhoto();
					r.id = id;
					memcpy(r.data, shot, PHOTO_BYTES);
					r.complete = true;
					r.nRecv = PHOTO_CHUNKS;
				}
				m_flFlashStart = now;
				StartUpload(id);
			}
			else
			{
				char cmd[64];
				snprintf(cmd, sizeof(cmd), "ss_photo %d fail", (int)id);
				gEngfuncs.pfnServerCmd(cmd);
			}
		}
	}

	// 2) upload, one chunk every ~30 ms so the reliable channel is never flooded
	if (m_iUpId != 0 && now >= m_flNextUpload)
	{
		const int me = LocalPlayerIndex();
		if (me > 0 && me < MAX_PLAYERS_SLOTS && m_Rx[me].id == m_iUpId)
		{
			char text[PHOTO_CHUNK_CHARS + 1];
			PhotoEncodeChunk(m_Rx[me].data + m_iUpChunk * PHOTO_CHUNK_BYTES, text);

			char cmd[PHOTO_CHUNK_CHARS + 48];
			snprintf(cmd, sizeof(cmd), "ss_photo %d %d %s", (int)m_iUpId, m_iUpChunk, text);
			gEngfuncs.pfnServerCmd(cmd);

			m_iUpChunk++;
			m_flNextUpload = now + 0.03f;
			if (m_iUpChunk >= PHOTO_CHUNKS)
				m_iUpId = 0;
		}
		else
		{
			m_iUpId = 0;
		}
	}
}

// ---------------- decals ----------------

bool CHudPhoto::TryMakeTexture(Decal& d)
{
	if (d.ready)
		return true;
	if (!gl.ok || d.owner < 1 || d.owner >= MAX_PLAYERS_SLOTS)
		return false;

	const RxPhoto& r = m_Rx[d.owner];
	if (!r.complete || r.id != d.id)
		return false;

	static unsigned char rgba[PHOTO_BYTES * 4];
	Expand332(r.data, rgba);

	GLint_ prevTex = 0, prevAlign = 4;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D_, &prevTex);
	gl.GetIntegerv(GL_UNPACK_ALIGNMENT_, &prevAlign);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT_, 1);

	gl.GenTextures(1, &d.tex);
	gl.BindTexture(GL_TEXTURE_2D_, d.tex);
	gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_);
	gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_LINEAR_);
	gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
	gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
	gl.TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA_, PHOTO_W, PHOTO_H, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);

	// leave the engine's texture state exactly as we found it
	gl.BindTexture(GL_TEXTURE_2D_, (GLuint_)prevTex);
	gl.PixelStorei(GL_UNPACK_ALIGNMENT_, prevAlign);

	d.ready = true;
	return true;
}

void CHudPhoto::DrawDecals()
{
	if (!gl.ok)
		return;

	const float now = gEngfuncs.GetClientTime();
	bool any = false;
	for (auto& d : m_Decals)
	{
		if (!d.used)
			continue;
		if (!d.ready && !TryMakeTexture(d))
		{
			if (now - d.startTime > 10.0f) // pixels never arrived
				FreeDecal(d);
			continue;
		}
		any = true;
	}
	if (!any)
		return;

	float half = (m_pCvarSize ? m_pCvarSize->value : 64.0f) * 0.5f;
	if (half < 4.0f)
		half = 4.0f;
	if (half > 256.0f)
		half = 256.0f;

	GLint_ prevTex = 0;
	gl.GetIntegerv(GL_TEXTURE_BINDING_2D_, &prevTex);

	gEngfuncs.pTriAPI->RenderMode(kRenderNormal);
	gEngfuncs.pTriAPI->CullFace(TRI_NONE);
	gl.Enable(GL_TEXTURE_2D_);
	gl.Enable(GL_POLYGON_OFFSET_FILL_);
	gl.PolygonOffset(-2.0f, -2.0f);

	for (auto& d : m_Decals)
	{
		if (!d.used || !d.ready)
			continue;

		gl.BindTexture(GL_TEXTURE_2D_, d.tex);

		// centre slightly in front of the wall
		float c[3];
		for (int i = 0; i < 3; i++)
			c[i] = d.pos[i] + d.normal[i] * 0.5f;

		float v[4][3];
		const float sx[4] = {-1, 1, 1, -1};
		const float sy[4] = {1, 1, -1, -1}; // top-left, top-right, bottom-right, bottom-left
		const float u[4] = {0, 1, 1, 0};
		const float t[4] = {0, 0, 1, 1}; // image row 0 (top) is t = 0
		for (int k = 0; k < 4; k++)
			for (int i = 0; i < 3; i++)
				v[k][i] = c[i] + d.right[i] * half * sx[k] + d.up[i] * half * sy[k];

		gEngfuncs.pTriAPI->Color4f(1.0f, 1.0f, 1.0f, 1.0f);
		gEngfuncs.pTriAPI->Begin(TRI_QUADS);
		for (int k = 0; k < 4; k++)
		{
			gEngfuncs.pTriAPI->TexCoord2f(u[k], t[k]);
			gEngfuncs.pTriAPI->Vertex3fv(v[k]);
		}
		gEngfuncs.pTriAPI->End();
	}

	gl.BindTexture(GL_TEXTURE_2D_, (GLuint_)prevTex);
	gl.Disable(GL_POLYGON_OFFSET_FILL_);
	gEngfuncs.pTriAPI->CullFace(TRI_FRONT);
}
