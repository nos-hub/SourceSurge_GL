// ss_hud_editor_v2.h - drag-and-drop HUD layout editor for HLSDK Xash3D
// With stable widget IDs, version tracking, input freeze, and profile support
#pragma once
#include "hud.h"
#include "cl_util.h"
#include <stdio.h>
#include <string.h>

#define SS_K_ESCAPE 27
#define SS_K_MOUSE1 241
#define SS_K_MOUSE2 242
#define SS_MAX_WIDGETS 32
#define SS_HUD_VERSION 1

// Stable widget ID enum - DO NOT REORDER, only append
enum
{
	HUD_SPEEDOMETER = 0,
	HUD_TIMER,
	HUD_WAVE_COUNTER,
	HUD_GRAPPLE_COOLDOWN,
	HUD_HEALTH,
	HUD_AMMO,
	HUD_ARMOR,
	HUD_POINTS_TOTAL,
	HUD_POINTS_POPUP,
	HUD_WAVE_ENEMIES,
	HUD_GRAPPLE_CHARGE,
	HUD_TECH_POPUP,
	HUD_MELEE_CHARGES,
	HUD_MAX
};

struct ssWidget_t
{
	int   id;           // stable enum ID
	const char *label;  // display name (can be changed without breaking saves)
	float defX, defY;
	int   w, h;
	float x, y;
	bool  visible;
};

class CHudEditor
{
public:
	CHudEditor() : m_count(0), m_active(false), m_drag(-1), m_cx(0), m_cy(0),
		m_offX(0), m_offY(0), m_currentProfile(0) {}

	// ---- registration ----
	int Add( int id, const char *label, float defX, float defY, int w, int h )
	{
		if( m_count >= SS_MAX_WIDGETS ) return -1;
		ssWidget_t &s = m_w[m_count];
		s.id = id;
		s.label = label;
		s.defX = s.x = defX;
		s.defY = s.y = defY;
		s.w = w;
		s.h = h;
		s.visible = true;
		return m_count++;
	}

	// ---- queries ----
	bool Visible( int id ) const
	{
		for( int i = 0; i < m_count; i++ )
			if( m_w[i].id == id ) return m_w[i].visible;
		return true;
	}

	void GetPos( int id, int &px, int &py ) const
	{
		px = py = 0;
		for( int i = 0; i < m_count; i++ )
		{
			if( m_w[i].id == id )
			{
				px = (int)( m_w[i].x * ScreenWidth );
				py = (int)( m_w[i].y * ScreenHeight );
				return;
			}
		}
	}

	bool IsActive() const { return m_active; }
	bool InputFrozen() const { return m_active; }   // freeze +forward, +attack, etc while editing

	void Toggle()
	{
		m_active = !m_active;
		m_drag = -1;
		if( m_active ) { m_cx = ScreenWidth / 2; m_cy = ScreenHeight / 2; }
		else Save();
	}

	// ---- profile switching ----
	void SetProfile( const char *name )
	{
		strncpy( m_profileName, name, sizeof( m_profileName ) - 1 );
		m_profileName[sizeof( m_profileName ) - 1] = 0;
		Load();
	}

	const char *GetProfile() const { return m_profileName; }

	// ---- input ----
	void MouseDelta( int dx, int dy )
	{
		if( !m_active ) return;
		m_cx = Clamp( m_cx + dx, 0, ScreenWidth - 1 );
		m_cy = Clamp( m_cy + dy, 0, ScreenHeight - 1 );
		if( m_drag >= 0 )
		{
			ssWidget_t &s = m_w[m_drag];
			int px = Clamp( Snap( m_cx - m_offX ), 0, ScreenWidth  - s.w );
			int py = Clamp( Snap( m_cy - m_offY ), 0, ScreenHeight - s.h );
			s.x = (float)px / ScreenWidth;
			s.y = (float)py / ScreenHeight;
		}
	}

	int KeyEvent( int down, int key )
	{
		if( !m_active ) return 1;

		if( key == SS_K_ESCAPE ) { if( down ) Toggle(); return 0; }

		if( key == SS_K_MOUSE1 )
		{
			if( !down ) { m_drag = -1; return 0; }

			// Check buttons
			if( Hit( BtnX(0), BtnY(), 60, 16 ) ) { Save();  return 0; }
			if( Hit( BtnX(1), BtnY(), 60, 16 ) ) { Reset(); return 0; }

			// Check widgets (topmost first)
			for( int i = m_count - 1; i >= 0; i-- )
			{
				int px, py; GetPosIndex( i, px, py );
				if( Hit( px, py, m_w[i].w, m_w[i].h ) )
				{
					m_drag = i;
					m_offX = m_cx - px;
					m_offY = m_cy - py;
					break;
				}
			}
			return 0;
		}

		if( key == SS_K_MOUSE2 )   // right click = toggle visibility
		{
			if( down )
			{
				for( int i = m_count - 1; i >= 0; i-- )
				{
					int px, py; GetPosIndex( i, px, py );
					if( Hit( px, py, m_w[i].w, m_w[i].h ) )
					{
						m_w[i].visible = !m_w[i].visible;
						break;
					}
				}
			}
			return 0;
		}

		return 1;
	}

	// ---- drawing ----
	void Draw()
	{
		if( !m_active ) return;

		// Dim background
		gEngfuncs.pfnFillRGBABlend( 0, 0, ScreenWidth, ScreenHeight, 0, 0, 0, 110 );

		// Draw widgets
		for( int i = 0; i < m_count; i++ )
		{
			int px, py; GetPosIndex( i, px, py );
			const ssWidget_t &s = m_w[i];
			int a = s.visible ? 90 : 35;
			if( i == m_drag )
				gEngfuncs.pfnFillRGBABlend( px, py, s.w, s.h, 255, 200, 0, 120 );
			else
				gEngfuncs.pfnFillRGBABlend( px, py, s.w, s.h, 80, 160, 255, a );
			Outline( px, py, s.w, s.h );

			gEngfuncs.pfnDrawSetTextColor( 1.0f, 1.0f, 1.0f );
			gEngfuncs.pfnDrawConsoleString( px + 3, py + 3, (char *)s.label );

			// Visibility indicator
			if( !s.visible )
			{
				gEngfuncs.pfnDrawSetTextColor( 1.0f, 0.3f, 0.3f );
				gEngfuncs.pfnDrawConsoleString( px + 3, py + 14, (char *)"[X]" );
			}
		}

		// Toolbar
		Button( 0, "Save" );
		Button( 1, "Reset" );

		gEngfuncs.pfnDrawSetTextColor( 0.8f, 0.8f, 0.8f );
		gEngfuncs.pfnDrawConsoleString( BtnX(2), BtnY() + 3,
			(char *)"LMB drag | RMB hide/show | ESC save+exit" );

		// Profile name
		static char profileStr[128];
		snprintf( profileStr, sizeof(profileStr), "Profile: %s", m_profileName );
		gEngfuncs.pfnDrawSetTextColor( 0.7f, 0.9f, 0.7f );
		gEngfuncs.pfnDrawConsoleString( 8, ScreenHeight - 20, profileStr );

		// Cursor
		gEngfuncs.pfnFillRGBABlend( m_cx - 1, m_cy - 6, 2, 12, 255, 255, 255, 255 );
		gEngfuncs.pfnFillRGBABlend( m_cx - 6, m_cy - 1, 12, 2, 255, 255, 255, 255 );
	}

	// ---- persistence ----
	void Save()
	{
		FILE *f = fopen( Path(), "w" );
		if( !f ) return;

		fprintf( f, "version %d\n", SS_HUD_VERSION );
		fprintf( f, "profile %s\n", m_profileName );
		fprintf( f, "\n" );

		for( int i = 0; i < m_count; i++ )
		{
			fprintf( f, "widget %d %s %f %f %d\n",
				m_w[i].id, m_w[i].label, m_w[i].x, m_w[i].y, m_w[i].visible ? 1 : 0 );
		}
		fclose( f );
	}

	void Load()
	{
		FILE *f = fopen( Path(), "r" );
		if( !f )
		{
			// File doesn't exist; defaults are already set
			if( !m_profileName[0] ) strcpy( m_profileName, "default" );
			return;
		}

		if( !m_profileName[0] ) strcpy( m_profileName, "default" );

		char line[256], cmd[64], label[64];
		int id, vis, ver;
		float x, y;

		while( fgets( line, sizeof(line), f ) )
		{
			if( sscanf( line, "version %d", &ver ) == 1 ) { /* version check here if needed */ }
			else if( sscanf( line, "profile %63s", m_profileName ) == 1 ) { /* load profile */ }
			else if( sscanf( line, "widget %d %63s %f %f %d", &id, label, &x, &y, &vis ) == 5 )
			{
				for( int i = 0; i < m_count; i++ )
				{
					if( m_w[i].id == id )
					{
						m_w[i].x = x;
						m_w[i].y = y;
						m_w[i].visible = vis != 0;
						break;
					}
				}
			}
		}
		fclose( f );
	}

	void Reset()
	{
		for( int i = 0; i < m_count; i++ )
		{
			m_w[i].x = m_w[i].defX;
			m_w[i].y = m_w[i].defY;
			m_w[i].visible = true;
		}
	}

private:
	static int Clamp( int v, int lo, int hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }
	static int Snap( int v ) { return ( v / 4 ) * 4; }
	bool Hit( int x, int y, int w, int h ) const
	{
		return m_cx >= x && m_cx < x + w && m_cy >= y && m_cy < y + h;
	}

	static int BtnX( int i ) { return 8 + i * 68; }
	static int BtnY() { return 8; }

	void GetPosIndex( int idx, int &px, int &py ) const
	{
		px = (int)( m_w[idx].x * ScreenWidth );
		py = (int)( m_w[idx].y * ScreenHeight );
	}

	void Outline( int x, int y, int w, int h )
	{
		gEngfuncs.pfnFillRGBABlend( x, y, w, 1, 255, 255, 255, 200 );
		gEngfuncs.pfnFillRGBABlend( x, y + h - 1, w, 1, 255, 255, 255, 200 );
		gEngfuncs.pfnFillRGBABlend( x, y, 1, h, 255, 255, 255, 200 );
		gEngfuncs.pfnFillRGBABlend( x + w - 1, y, 1, h, 255, 255, 255, 200 );
	}

	void Button( int i, const char *label )
	{
		bool hot = Hit( BtnX( i ), BtnY(), 60, 16 );
		gEngfuncs.pfnFillRGBABlend( BtnX( i ), BtnY(), 60, 16,
			hot ? 255 : 60, hot ? 200 : 60, hot ? 0 : 60, 200 );
		gEngfuncs.pfnDrawSetTextColor( 1.0f, 1.0f, 1.0f );
		gEngfuncs.pfnDrawConsoleString( BtnX( i ) + 8, BtnY() + 3, (char *)label );
	}

	const char *Path()
	{
		static char p[260];
		snprintf( p, sizeof( p ), "%s/hud_layout_%s.cfg", gEngfuncs.pfnGetGameDirectory(), m_profileName );
		return p;
	}

	ssWidget_t m_w[SS_MAX_WIDGETS];
	int  m_count;
	bool m_active;
	int  m_drag, m_cx, m_cy, m_offX, m_offY;
	char m_profileName[64];
	int  m_currentProfile;
};
