#ifndef DUEL1V1_SHARED_H
#define DUEL1V1_SHARED_H

// ---------------------------------------------------------------------------
// 1v1 mode - data shared by the server DLL (dlls/) and the client DLL (cl_dll/).
// Put this file where BOTH projects can #include it (e.g. common/), or copy it
// into both folders. It must stay identical on both sides.
//
// v2: adds the hide & seek mode (ss_1v1_mode 1) and the spawn sound volume.
// If you added your own mechanic rows to g_Duel1v1Opts[] in v1, copy them into this table.
// ---------------------------------------------------------------------------

enum
{
	DUEL_OFF = 0,   // mode disabled (ss_1v1 0)
	DUEL_WAITING,   // waiting for both duelists to ready up
	DUEL_COUNTDOWN, // players spawned and frozen
	DUEL_LIVE,      // match running
	DUEL_ENDED      // results screen
};

enum
{
	DOPT_BOOL = 0,
	DOPT_INT = 1,
	DOPT_FLOAT = 2
};

#define DUEL_MAX_OPTS 32

typedef struct
{
	const char *cvar;     // server cvar name
	const char *label;    // text shown in the menu
	const char *section;  // menu tab this row lives in
	int         type;     // DOPT_*
	float       minv, maxv, step;
	const char *defval;   // default value (string, like a cvar default)
} Duel1v1Opt;

// Every row here is: registered as a server cvar (if it does not already exist),
// editable from the menu through "1v1_set", and synced to clients in "DuelCfg".
// ADD YOUR EXISTING MECHANIC CVARS HERE (slam, grapple, kill-heal, fall damage...)
// as extra rows, e.g.:
//   { "ss_slam_enabled", "Slam", "Movement", DOPT_BOOL, 0, 1, 1, "1" },
// Keep the order identical on both sides.
static const Duel1v1Opt g_Duel1v1Opts[] =
{
	// cvar                     label                            section    type        min   max    step  default
	{ "ss_1v1",                 "Enable 1v1 mode",               "Match",   DOPT_BOOL,  0,    1,     1,    "0"    },
	{ "ss_1v1_mode",            "Mode (0 duel, 1 hide & seek)",  "Match",   DOPT_INT,   0,    1,     1,    "0"    },
	{ "ss_1v1_killlimit",       "Kill limit (duel)",             "Match",   DOPT_INT,   1,    100,   1,    "10"   },
	{ "ss_1v1_timelimit",       "Time limit (min, 0=off)",       "Match",   DOPT_INT,   0,    60,    1,    "0"    },
	{ "ss_1v1_countdown",       "Start countdown (s)",           "Match",   DOPT_INT,   0,    10,    1,    "3"    },

	{ "ss_1v1_respawn_min",     "Respawn ring: min radius",      "Respawn", DOPT_FLOAT, 64,   1024,  32,   "256"  },
	{ "ss_1v1_respawn_max",     "Respawn ring: max radius",      "Respawn", DOPT_FLOAT, 128,  4096,  64,   "1024" },
	{ "ss_1v1_respawn_zrange",  "Respawn height range",          "Respawn", DOPT_FLOAT, 0,    2048,  64,   "512"  },
	{ "ss_1v1_pick_timeout",    "Pick timeout (s)",              "Respawn", DOPT_INT,   3,    60,    1,    "10"   },
	{ "ss_1v1_godtime",         "Spawn godmode (s)",             "Respawn", DOPT_FLOAT, 0,    5,     0.25f,"1"    },
	{ "ss_1v1_god_endonfire",   "Godmode ends on fire",          "Respawn", DOPT_BOOL,  0,    1,     1,    "1"    },
	{ "ss_1v1_los",             "Allow spawn in enemy LOS",      "Respawn", DOPT_BOOL,  0,    1,     1,    "1"    },
	{ "ss_1v1_spawnsound_vol",  "Spawn sound volume",            "Respawn", DOPT_FLOAT, 0,    1,     0.1f, "0.8"  },

	{ "ss_1v1_fullammo",        "Full ammo on spawn",            "Rules",   DOPT_BOOL,  0,    1,     1,    "1"    },

	{ "ss_1v1_hns_time",        "Hider clock (s)",               "HNS",     DOPT_INT,   10,   900,   10,   "120"  },
	{ "ss_1v1_hns_respawn",     "Respawn delay (s)",             "HNS",     DOPT_FLOAT, 0,    10,    0.5f, "3"    },
};
#define DUEL_NUM_OPTS ((int)(sizeof(g_Duel1v1Opts) / sizeof(g_Duel1v1Opts[0])))

// Weapon whitelist rows (menu "Weapons" tab). Fix the classnames to match your mod.
typedef struct
{
	const char *classname;
	const char *label;
} Duel1v1Weapon;

static const Duel1v1Weapon g_Duel1v1Weapons[] =
{
	{ "weapon_crowbar", "Melee"   },
	{ "weapon_sniper",  "Sniper"  },
	{ "weapon_grapple", "Grapple" },
	{ "weapon_camera",  "Camera"  },
};
#define DUEL_NUM_WEAPONS ((int)(sizeof(g_Duel1v1Weapons) / sizeof(g_Duel1v1Weapons[0])))
#define DUEL_DEFAULT_WEAPONS "weapon_crowbar,weapon_sniper,weapon_grapple"

// Default sound played at every player spawn (relative to the sound/ folder).
// Change it with the server cvar ss_1v1_spawnsound (applies after a map change, it must be precached).
#define DUEL_DEFAULT_SPAWNSOUND "debris/beamstart1.wav"

// Presets: each name maps to a server-side cfg file "1v1_<name>.cfg" in the mod folder.
static const char *g_Duel1v1Presets[] = { "sniper", "melee", "hns" };
#define DUEL_NUM_PRESETS ((int)(sizeof(g_Duel1v1Presets) / sizeof(g_Duel1v1Presets[0])))

#endif // DUEL1V1_SHARED_H
