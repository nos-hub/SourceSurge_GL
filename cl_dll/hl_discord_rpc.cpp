#include "hl_discord_rpc.h"
#include "discord/discord_rpc.h"
#include "hud.h" // or cl_util.h, whichever gives you gEngfuncs in your cl_dll
#include <time.h>
#include <string.h>
#include <stdio.h>

static bool g_bDiscordInitialized = false;
static time_t g_flDiscordStartTime = 0;
static char g_szLastMap[64] = "\x01"; // sentinel so the first update always fires

#define DISCORD_APP_ID "1555096640048734228"

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

static int g_iLastMaxClients = -1;

static void DiscordRPC_UpdatePresence()
{
	char map[64];
	GetCurrentMapName(map, sizeof(map));

	int maxClients = gEngfuncs.GetMaxClients();

	// only update when the map or mode actually changed
	if (strcmp(map, g_szLastMap) == 0 && maxClients == g_iLastMaxClients)
		return;

	strncpy(g_szLastMap, map, sizeof(g_szLastMap) - 1);
	g_szLastMap[sizeof(g_szLastMap) - 1] = '\0';
	g_iLastMaxClients = maxClients;

	char details[128];
	if (map[0])
		snprintf(details, sizeof(details), "Playing on %s (%s)",
			map, (maxClients > 1) ? "multiplayer" : "solo");
	else
		snprintf(details, sizeof(details), "Idling in menus");

	DiscordRichPresence presence;
	memset(&presence, 0, sizeof(presence));
	presence.details = details;
	presence.state = "rethinking my life choices";
	presence.startTimestamp = g_flDiscordStartTime;

	Discord_UpdatePresence(&presence);
}


void DiscordRPC_Init()
{
	if (g_bDiscordInitialized)
		return;

	DiscordEventHandlers handlers;
	memset(&handlers, 0, sizeof(handlers));

	Discord_Initialize(DISCORD_APP_ID, &handlers, 1, NULL);

	g_flDiscordStartTime = time(NULL);
	g_bDiscordInitialized = true;

	DiscordRPC_UpdatePresence();
}

void DiscordRPC_RunCallbacks()
{
	if (!g_bDiscordInitialized)
		return;

	DiscordRPC_UpdatePresence(); // cheap, does nothing unless the map changed
	Discord_RunCallbacks();
}

void DiscordRPC_Shutdown()
{
	if (!g_bDiscordInitialized)
		return;
	Discord_Shutdown();
	g_bDiscordInitialized = false;
}