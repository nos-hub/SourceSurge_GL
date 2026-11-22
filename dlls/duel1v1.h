#ifndef DUEL1V1_H
#define DUEL1V1_H

class CBasePlayer;

// ---- lifecycle hooks (see HNS_AND_SOUND.md / INTEGRATION.md for where each one goes) ----
void Duel1v1_Init(void);                 // GameDLLInit()
void Duel1v1_Precache(void);             // ClientPrecache()  (NEW in v2: precaches the spawn sound)
void Duel1v1_LinkUserMessages(void);     // LinkUserMessages()
void Duel1v1_Think(void);                // StartFrame()

// ---- client commands: returns true if the command was a 1v1_* command ----
bool Duel1v1_ClientCommand(CBasePlayer *pPlayer, const char *pcmd);

// ---- queries / events ----
bool Duel1v1_Active(void);                                   // ss_1v1 != 0
bool Duel1v1_IsChoosing(CBasePlayer *pPlayer);               // dead and NOT allowed to respawn by itself (picking, or waiting for the auto respawn)
void Duel1v1_PlayerKilled(CBasePlayer *pVictim, entvars_t *pevAttacker);
bool Duel1v1_WeaponAllowed(const char *classname);           // whitelist check
bool Duel1v1_PlayerMayHave(CBasePlayer *pPlayer, const char *classname); // role aware weapon check
bool Duel1v1_GetPVSOrigin(edict_t *pClient, Vector &outOrigin); // SetupVisibility override

#endif // DUEL1V1_H
