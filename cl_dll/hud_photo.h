// hud_photo.h - photo camera: capture, upload and photo decals (client side, cl_dll/)
#pragma once

#include "photo_shared.h"

class CHudPhoto : public CHudBase
{
public:
	bool Init() override;
	bool VidInit() override;
	bool Draw(float flTime) override; // white flash after a shot

	bool MsgFunc_PhotoCap(const char* pszName, int iSize, void* pbuf);
	bool MsgFunc_PhotoDat(const char* pszName, int iSize, void* pbuf);
	bool MsgFunc_PhotoSpr(const char* pszName, int iSize, void* pbuf);

	// Called at the very top of CHud::Redraw, BEFORE any HUD element is drawn:
	// the screen then holds the clean 3D view. Does the capture and paces the upload.
	void PreRedraw();

	// Called from HUD_DrawTransparentTriangles: draws the sprayed photos in the world.
	void DrawDecals();

private:
	static constexpr int MAX_DECALS = 24;
	static constexpr int MAX_PLAYERS_SLOTS = 33;

	struct RxPhoto
	{
		short id = 0;
		bool complete = false;
		int nRecv = 0;
		bool got[PHOTO_CHUNKS] = {};
		unsigned char data[PHOTO_BYTES] = {};
	};

	struct Decal
	{
		bool used = false;
		bool ready = false; // texture uploaded
		unsigned int tex = 0;
		int owner = 0;
		short id = 0;
		float startTime = 0;
		float pos[3] = {};
		float right[3] = {};
		float up[3] = {};
		float normal[3] = {};
	};

	bool CaptureScreen(unsigned char* out332);
	void RestoreViewModel();
	void StartUpload(short id);
	bool TryMakeTexture(Decal& d);
	void FreeDecal(Decal& d);
	void ClearAll();

	RxPhoto m_Rx[MAX_PLAYERS_SLOTS];
	Decal m_Decals[MAX_DECALS];

	// capture
	short m_iPendingId = 0;
	int m_iCapDelay = 0;
	bool m_bVMHidden = false;
	float m_flOldVM = 1.0f;
	float m_flVMRestoreAt = 0;
	float m_flFlashStart = -100.0f;

	// upload
	short m_iUpId = 0;
	int m_iUpChunk = 0;
	float m_flNextUpload = 0;

	struct cvar_s* m_pCvarSize = nullptr;
};
