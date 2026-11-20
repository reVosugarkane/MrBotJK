/*
 * cl_bridge.h - "bot bridge" for the OpenJK multiplayer client.
 *
 * Exposes ONLY what a normal client receives (its own playerState and the
 * entities in its snapshot) to a local program over UDP, and lets that program
 * supply the usercmd the client sends. The server is untouched and sees an
 * ordinary player.
 *
 * Inert unless the cvar cl_bridge is 1.
 */
#ifndef CL_BRIDGE_H
#define CL_BRIDGE_H

void CLBridge_Init(void);                          /* call once from CL_Init            */
void CLBridge_Shutdown(void);                      /* call from CL_Shutdown             */
void CLBridge_OnSnapshot(void);                    /* call after a snapshot is parsed   */
void CLBridge_OnServerCommand(const char *cmd);    /* call for each server command text */
void CLBridge_ModifyCmd(usercmd_t *cmd);           /* call at the end of CL_CreateCmd   */

#endif
