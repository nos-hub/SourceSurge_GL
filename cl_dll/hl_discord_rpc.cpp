#ifndef FCVAR_ARCHIVE
#define FCVAR_ARCHIVE (1 << 0) // saved to config.cfg (same bit as FCVAR_ARCHIVE in the engine)
#endif

#include "hl_discord_rpc.h"
#include "discord/discord_rpc.h"
#include "hud.h" // or cl_util.h, whichever gives you gEngfuncs in your cl_dll
#include "net_api.h" // net_status_t / gEngfuncs.pNetAPI (add "netadr.h" too if your tree needs it)
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static bool g_bDiscordInitialized = false;
static bool g_bPresenceShown = false;
static time_t g_flDiscordStartTime = 0;
static time_t g_flLastUpdate = 0;
static char g_szLastMap[64] = "\x01"; // sentinel so the first update always fires
static int g_iLastMaxClients = -1;

// per-map speed tracking
static float g_flTopSpeed = 0.0f;
static float g_vLastOrigin[3];
static float g_flLastSampleTime = 0.0f;
static bool g_bHaveLastSample = false;

#define DISCORD_APP_ID "1555096640048734228"
#define DISCORD_UPDATE_INTERVAL 15 // Discord rate-limits presence updates to ~1 per 15s
#define DISCORD_LARGE_IMAGE "logo" // asset key uploaded in the Discord dev portal (Rich Presence > Art Assets)

// rotates every minute, shown when hovering the large image
static const char* g_szQuips[] =
{
	"rethinking my life choices",
	"one more run, I swear",
	"blaming the ramp",
	"surfing at questionable speeds",
	"the skill issue is the map's",
	"touching grass? maybe later",
};
#define NUM_QUIPS (sizeof(g_szQuips) / sizeof(g_szQuips[0]))

static void GetCurrentMapName(char* out, size_t size)
{
	out[0] = '\0';

	const char* level = gEngfuncs.pfnGetLevelName();
	if (!level || !level[0])
		return;

	// "maps/crossfire.bsp" -> "crossfire"
	const char* name = level;
	const char* slash = strrchr(level, '/');
	if (!slash)
		slash = strrchr(level, '\\');
	if (slash)
		name = slash + 1;

	strncpy(out, name, size - 1);
	out[size - 1] = '\0';

	char* dot = strrchr(out, '.');
	if (dot)
		*dot = '\0';
}

// Horizontal speed from origin deltas. Doesn't rely on curstate.velocity,
// which can be zero on the local player depending on the engine.
static void SampleSpeed()
{
	cl_entity_t* ent = gEngfuncs.GetLocalPlayer();
	if (!ent)
		return;

	float now = gEngfuncs.GetClientTime();
	if (g_bHaveLastSample)
	{
		float dt = now - g_flLastSampleTime;
		if (dt > 0.0f && dt < 0.25f)
		{
			float dx = ent->origin[0] - g_vLastOrigin[0];
			float dy = ent->origin[1] - g_vLastOrigin[1];
			float speed = sqrtf(dx * dx + dy * dy) / dt;
			// ignore teleports / respawns
			if (speed < 10000.0f && speed > g_flTopSpeed)
				g_flTopSpeed = speed;
		}
	}
	g_vLastOrigin[0] = ent->origin[0];
	g_vLastOrigin[1] = ent->origin[1];
	g_vLastOrigin[2] = ent->origin[2];
	g_flLastSampleTime = now;
	g_bHaveLastSample = true;
}

static int CountPlayers(int maxClients)
{
	int count = 0;
	for (int i = 1; i <= maxClients; i++)
	{
		hud_player_info_t info;
		memset(&info, 0, sizeof(info));
		gEngfuncs.pfnGetPlayerInfo(i, &info);
		if (info.name && info.name[0])
			count++;
	}
	return count;
}

// "ip:port" of the server we're on (false for listen servers / not connected)
static bool GetServerAddress(char* out, size_t size, int* pingMs)
{
	net_status_t status;
	memset(&status, 0, sizeof(status));
	gEngfuncs.pNetAPI->Status(&status);

	*pingMs = (int)(status.latency * 1000.0);

	if (!status.connected)
		return false;

	const unsigned char* ip = status.remote_address.ip;
	if (ip[0] == 127 || (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0))
		return false;

	// port is stored in network byte order
	unsigned short port = status.remote_address.port;
	port = (unsigned short)((port << 8) | (port >> 8));

	snprintf(out, size, "%d.%d.%d.%d:%u", ip[0], ip[1], ip[2], ip[3], (unsigned)port);
	return true;
}

// ---- Discord event handlers ----

static void OnReady(const DiscordUser* user)
{
	gEngfuncs.Con_Printf("[Discord RPC] connected as %s\n", user->username);
}

static void OnDisconnected(int code, const char* msg)
{
	gEngfuncs.Con_Printf("[Discord RPC] disconnected (%d: %s)\n", code, msg);
}

static void OnErrored(int code, const char* msg)
{
	gEngfuncs.Con_Printf("[Discord RPC] error (%d: %s)\n", code, msg);
}

// someone clicked "Join" on our presence
static void OnJoinGame(const char* secret)
{
	// only accept "a.b.c.d:port" so a crafted secret can't inject console commands
	size_t len = strlen(secret);
	if (len == 0 || len > 21)
		return;
	for (const char* c = secret; *c; c++)
	{
		if (!((*c >= '0' && *c <= '9') || *c == '.' || *c == ':'))
			return;
	}

	char cmd[64];
	snprintf(cmd, sizeof(cmd), "connect %s\n", secret);
	gEngfuncs.pfnClientCmd(cmd);
}

static void DiscordRPC_UpdatePresence()
{
	// cvar off -> wipe presence once and stop
	if (gEngfuncs.pfnGetCvarFloat("discord_rpc") == 0.0f)
	{
		if (g_bPresenceShown)
		{
			Discord_ClearPresence();
			g_bPresenceShown = false;
			g_szLastMap[0] = '\x01';
		}
		return;
	}

	char map[64];
	GetCurrentMapName(map, sizeof(map));

	int maxClients = gEngfuncs.GetMaxClients();
	time_t now = time(NULL);

	bool changed = (strcmp(map, g_szLastMap) != 0 || maxClients != g_iLastMaxClients);
	if (changed)
	{
		strncpy(g_szLastMap, map, sizeof(g_szLastMap) - 1);
		g_szLastMap[sizeof(g_szLastMap) - 1] = '\0';
		g_iLastMaxClients = maxClients;

		// new map: restart the "elapsed" timer and speed record
		g_flDiscordStartTime = now;
		g_flTopSpeed = 0.0f;
		g_bHaveLastSample = false;
	}

	if (map[0])
		SampleSpeed();

	// refresh on map change, otherwise at most every DISCORD_UPDATE_INTERVAL seconds
	if (!changed && g_bPresenceShown && (now - g_flLastUpdate) < DISCORD_UPDATE_INTERVAL)
		return;
	g_flLastUpdate = now;

	char details[128];
	char state[128];
	char hover[128];
	char smallText[64];
	char serverAddr[32];

	DiscordRichPresence presence;
	memset(&presence, 0, sizeof(presence));
	presence.startTimestamp = g_flDiscordStartTime;
	presence.largeImageKey = DISCORD_LARGE_IMAGE;

	// hover text: rotating quip
	snprintf(hover, sizeof(hover), "%s", g_szQuips[(now / 60) % NUM_QUIPS]);
	presence.largeImageText = hover;

	if (map[0])
	{
		bool multi = (maxClients > 1);

		// optional per-map art: upload assets named after your maps (lowercase, <=32 chars)
		if (gEngfuncs.pfnGetCvarFloat("discord_rpc_mapimages") != 0.0f)
			presence.largeImageKey = map;

		snprintf(details, sizeof(details), "Playing on %s", map);
		snprintf(state, sizeof(state), "Top speed: %d u/s", (int)g_flTopSpeed);
		presence.details = details;
		presence.state = state;

		presence.smallImageKey = multi ? "multiplayer" : "solo"; // optional assets
		snprintf(smallText, sizeof(smallText), multi ? "Multiplayer" : "Solo");

		if (multi)
		{
			int ping = 0;
			bool haveAddr = GetServerAddress(serverAddr, sizeof(serverAddr), &ping);

			if (haveAddr)
				snprintf(smallText, sizeof(smallText), "Multiplayer - %d ms", ping);

			presence.partySize = CountPlayers(maxClients);
			presence.partyMax = maxClients;

			// "Join" button for friends (needs party + secret)
			if (haveAddr && gEngfuncs.pfnGetCvarFloat("discord_rpc_join") != 0.0f)
			{
				presence.partyId = serverAddr;
				presence.joinSecret = serverAddr;
				presence.instance = 1;
			}
		}
		presence.smallImageText = smallText;
	}
	else
	{
		presence.details = "Idling in menus";
		presence.state = "Picking a map";
	}

	Discord_UpdatePresence(&presence);
	g_bPresenceShown = true;
}


void DiscordRPC_Init()
{
	if (g_bDiscordInitialized)
		return;

	gEngfuncs.pfnRegisterVariable((char*)"discord_rpc", (char*)"1", FCVAR_ARCHIVE);            // master switch
	gEngfuncs.pfnRegisterVariable((char*)"discord_rpc_join", (char*)"1", FCVAR_ARCHIVE);       // expose server IP via Join button
	gEngfuncs.pfnRegisterVariable((char*)"discord_rpc_mapimages", (char*)"0", FCVAR_ARCHIVE);  // use map name as large image key

	DiscordEventHandlers handlers;
	memset(&handlers, 0, sizeof(handlers));
	handlers.ready = OnReady;
	handlers.disconnected = OnDisconnected;
	handlers.errored = OnErrored;
	handlers.joinGame = OnJoinGame;

	Discord_Initialize(DISCORD_APP_ID, &handlers, 1, NULL);

	g_flDiscordStartTime = time(NULL);
	g_bDiscordInitialized = true;

	DiscordRPC_UpdatePresence();
}

void DiscordRPC_RunCallbacks()
{
	if (!g_bDiscordInitialized)
		return;

	DiscordRPC_UpdatePresence(); // cheap: samples speed, only sends when throttle/map allows
	Discord_RunCallbacks();
}

void DiscordRPC_Shutdown()
{
	if (!g_bDiscordInitialized)
		return;
	Discord_ClearPresence();
	Discord_Shutdown();
	g_bDiscordInitialized = false;
	g_bPresenceShown = false;
}