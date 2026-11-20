/*
 * cl_bridge.cpp - see cl_bridge.h.
 *
 * Wire protocol (UDP, loopback only):
 *   game -> bot : one JSON object per datagram: {"v":1,"t":"state",...} / {"v":1,"t":"chat",...}
 *   bot -> game : text lines  HELLO | BYE | CMD ... | CON <command>
 * The Python side of this protocol is jkbot/protocol.py and jkbot/bridge.py.
 *
 * Compiles cleanly against the real OpenJK codemp headers (commit 260c59c). Still untested at RUNTIME
 * inside the real engine; see CLAUDE.md for what remains to be checked in a live game.
 */
#ifdef JKBRIDGE_SELFTEST
#include "shim/client.h"
#else
#include "client.h"      /* verified: compiles against OpenJK codemp headers (see CLAUDE.md) */
#endif

#include "cl_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET bridge_sock_t;
  #define BRIDGE_INVALID INVALID_SOCKET
  #define bridge_close closesocket
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int bridge_sock_t;
  #define BRIDGE_INVALID (-1)
  #define bridge_close close
#endif

#define BRIDGE_PROTOCOL 1
#define CLAMPI(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define BRIDGE_MAX_JSON 60000
#define BRIDGE_CMD_TIMEOUT_MS 250   /* bot input older than this is ignored (human/neutral input resumes) */

/* ---- state ------------------------------------------------------------ */
static cvar_t *cl_bridge;       /* 0/1 master switch */
static cvar_t *cl_bridgePort;   /* UDP port to listen on (127.0.0.1) */

static bridge_sock_t s_sock = BRIDGE_INVALID;
static struct sockaddr_in s_peer;
static int s_havePeer = 0;
static unsigned s_seq = 0;
static int s_nextOpenTry = 0;

typedef struct {
    int   valid;
    int   fwd, right, up;
    float pitch, yaw;
    int   buttons;
    int   weapon, forcesel;     /* -1 = leave alone */
    int   recvTime;             /* Sys_Milliseconds() when received */
} botCmd_t;

static botCmd_t s_bot;

typedef struct {
    int fwd, right, up, buttons, weapon, forcesel;
    float pitch, yaw;
    int valid;
} lastCmd_t;
static lastCmd_t s_last;

static int BridgeEnabled(void) { return cl_bridge && cl_bridge->integer; }

/* ---- socket helpers ---------------------------------------------------- */
static void BridgeOpen(void)
{
    struct sockaddr_in a;
#ifdef _WIN32
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock == BRIDGE_INVALID) {
        Com_Printf("cl_bridge: socket() failed\n");
        return;
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(s_sock, FIONBIO, &nb); }
#else
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     /* loopback only: other machines can't drive the client */
    a.sin_port = htons((unsigned short)(cl_bridgePort ? cl_bridgePort->integer : 29970));
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        Com_Printf("cl_bridge: bind to 127.0.0.1:%d failed\n", (int)ntohs(a.sin_port));
        bridge_close(s_sock);
        s_sock = BRIDGE_INVALID;
        return;
    }
    Com_Printf("cl_bridge: listening on 127.0.0.1:%d\n", (int)ntohs(a.sin_port));
}

static void BridgeSend(const char *data, int len)
{
    if (s_sock == BRIDGE_INVALID || !s_havePeer) return;
    sendto(s_sock, data, len, 0, (struct sockaddr *)&s_peer, sizeof(s_peer));
}

/* ---- JSON helpers (no dependencies) ------------------------------------ */
typedef struct { char *buf; int len; int cap; int overflow; } jw_t;

static void jw_raw(jw_t *w, const char *s)
{
    int n = (int)strlen(s);
    if (w->len + n >= w->cap) { w->overflow = 1; return; }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

#include <stdarg.h>
static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jw_raw(w, tmp);
}

static void jw_vec(jw_t *w, const char *key, const float *v)
{
    jw_fmt(w, "\"%s\":[%.2f,%.2f,%.2f]", key, v[0], v[1], v[2]);
}

static void jw_str(jw_t *w, const char *s)
{
    char tmp[8];
    jw_raw(w, "\"");
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = 0; jw_raw(w, tmp); }
        else if (c < 0x20)         { snprintf(tmp, sizeof(tmp), "\\u%04x", c); jw_raw(w, tmp); }
        else                       { tmp[0] = (char)c; tmp[1] = 0; jw_raw(w, tmp); }
    }
    jw_raw(w, "\"");
}

/* ---- angle conversion ---------------------------------------------------
 * VERIFY: usercmd angles are 16-bit fixed point and the server adds
 * ps.delta_angles, so absolute view = SHORT2ANGLE(cmd.angles + delta).
 */
static int AbsAngleToCmd(float absDeg, int delta)
{
    return (ANGLE2SHORT(absDeg) - delta) & 65535;
}

static float CmdToAbsAngle(int cmdAngle, int delta)
{
    return SHORT2ANGLE((cmdAngle + delta) & 65535);
}

/* ---- inbound: bot -> game ------------------------------------------------ */
static int CommandAllowed(const char *cmd)
{
    /* Only harmless client commands. Everything else is dropped. */
    static const char *ok[] = { "say", "say_team", "team", "name", "model", "connect",
                                "disconnect", "reconnect", "engage_duel", NULL };
    char word[32];
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < (int)sizeof(word) - 1) { word[i] = cmd[i]; ++i; }
    word[i] = 0;
    for (int k = 0; ok[k]; ++k)
        if (strcmp(word, ok[k]) == 0) return 1;
    return 0;
}

static void BridgeHandleLine(char *line, const struct sockaddr_in *from)
{
    /* trim newline */
    for (char *p = line; *p; ++p) if (*p == '\n' || *p == '\r') { *p = 0; break; }

    if (strncmp(line, "HELLO", 5) == 0) {
        s_peer = *from;
        s_havePeer = 1;
    } else if (strncmp(line, "BYE", 3) == 0) {
        s_havePeer = 0;
        s_bot.valid = 0;
    } else if (strncmp(line, "CMD ", 4) == 0) {
        unsigned seq; int fwd, right, up, buttons, weapon, forcesel; float pitch, yaw;
        if (sscanf(line + 4, "%u %d %d %d %f %f %d %d %d",
                   &seq, &fwd, &right, &up, &pitch, &yaw, &buttons, &weapon, &forcesel) == 9) {
            /* range-limit everything: the bot is not trusted to send sane values */
            fwd = CLAMPI(fwd, -127, 127);
            right = CLAMPI(right, -127, 127);
            up = CLAMPI(up, -127, 127);
            pitch = pitch > 89.f ? 89.f : (pitch < -89.f ? -89.f : pitch);
            s_bot.valid = 1;
            s_bot.fwd = fwd; s_bot.right = right; s_bot.up = up;
            s_bot.pitch = pitch; s_bot.yaw = yaw;
            s_bot.buttons = buttons; s_bot.weapon = weapon; s_bot.forcesel = forcesel;
            s_bot.recvTime = Sys_Milliseconds();
            s_peer = *from; s_havePeer = 1;
        }
    } else if (strncmp(line, "CON ", 4) == 0) {
        const char *c = line + 4;
        if (strchr(c, ';') || strchr(c, '\n') || !CommandAllowed(c)) {
            Com_Printf("cl_bridge: refused console command\n");
        } else {
            Cbuf_AddText(va("%s\n", c));
        }
    }
}

static void BridgePump(void)
{
    char buf[512];
    struct sockaddr_in from;
#ifdef _WIN32
    int fl;
#else
    socklen_t fl;
#endif
    if (s_sock == BRIDGE_INVALID) return;
    for (;;) {
        fl = sizeof(from);
        int n = (int)recvfrom(s_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = 0;
        BridgeHandleLine(buf, &from);
    }
}

/* Team of a client, from the same player configstring the game's own client code reads
 * (cg_players.c: Info_ValueForKey(configstring, "t")). 0 free, 1 red, 2 blue, 3 spectator; -1 unknown. */
static int PlayerTeam(int clientNum)
{
    int off;
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) return -1;
    off = cl.gameState.stringOffsets[CS_PLAYERS + clientNum];
    if (!off) return -1;
    return atoi(Info_ValueForKey(cl.gameState.stringData + off, "t"));
}

/* ---- public API ------------------------------------------------------------ */
void CLBridge_Init(void)
{
    cl_bridge     = Cvar_Get("cl_bridge", "0", CVAR_TEMP);        /* not saved: must be given on the command line each launch */
    cl_bridgePort = Cvar_Get("cl_bridgePort", "29970", CVAR_TEMP);
    if (BridgeEnabled()) BridgeOpen();
}

void CLBridge_Shutdown(void)
{
    if (s_sock != BRIDGE_INVALID) { bridge_close(s_sock); s_sock = BRIDGE_INVALID; }
    s_havePeer = 0;
    s_bot.valid = 0;
}

void CLBridge_ModifyCmd(usercmd_t *cmd)
{
    int now, overridden = 0;

    if (!BridgeEnabled()) return;
    now = Sys_Milliseconds();
    if (s_sock == BRIDGE_INVALID && now >= s_nextOpenTry) {
        s_nextOpenTry = now + 2000;     /* retry at most every 2s (e.g. port busy) */
        BridgeOpen();
    }
    BridgePump();

    if (s_bot.valid && (now - s_bot.recvTime) <= BRIDGE_CMD_TIMEOUT_MS) {
        const int *delta = cl.snap.ps.delta_angles;
        cmd->forwardmove = (signed char)s_bot.fwd;
        cmd->rightmove   = (signed char)s_bot.right;
        cmd->upmove      = (signed char)s_bot.up;
        cmd->buttons     = s_bot.buttons;
        if (s_bot.weapon   >= 0) cmd->weapon   = (byte)s_bot.weapon;
        if (s_bot.forcesel >= 0) cmd->forcesel = (byte)s_bot.forcesel;
        cmd->angles[PITCH] = AbsAngleToCmd(s_bot.pitch, delta[PITCH]);
        cmd->angles[YAW]   = AbsAngleToCmd(s_bot.yaw,   delta[YAW]);
        cmd->angles[ROLL]  = 0;
        /* keep the client's own idea of its view in step so mouse code doesn't fight us */
        cl.viewangles[PITCH] = SHORT2ANGLE(cmd->angles[PITCH]);
        cl.viewangles[YAW]   = SHORT2ANGLE(cmd->angles[YAW]);
        overridden = 1;
    } else {
        s_bot.valid = 0;
    }

    /* remember what we are sending so the next state packet can report it
     * (when nothing overrides it, this is the human's own input - the imitation data) */
    {
        const int *delta = cl.snap.ps.delta_angles;
        s_last.valid    = 1;
        s_last.fwd      = cmd->forwardmove;
        s_last.right    = cmd->rightmove;
        s_last.up       = cmd->upmove;
        s_last.buttons  = cmd->buttons;
        s_last.weapon   = cmd->weapon;
        s_last.forcesel = cmd->forcesel;
        s_last.pitch    = CmdToAbsAngle(cmd->angles[PITCH], delta[PITCH]);
        s_last.yaw      = CmdToAbsAngle(cmd->angles[YAW],   delta[YAW]);
    }
    (void)overridden;
}

void CLBridge_OnSnapshot(void)
{
    static char json[BRIDGE_MAX_JSON];
    jw_t w;
    const playerState_t *ps = &cl.snap.ps;
    int i, first = 1;

    if (!BridgeEnabled() || s_sock == BRIDGE_INVALID) return;
    BridgePump();
    if (!s_havePeer) return;

    w.buf = json; w.len = 0; w.cap = sizeof(json); w.overflow = 0; json[0] = 0;

    jw_fmt(&w, "{\"v\":%d,\"t\":\"state\",\"seq\":%u,\"server_time\":%d,", BRIDGE_PROTOCOL, ++s_seq, cl.snap.serverTime);

    /* ---- self: playerState_t ---- */
    jw_fmt(&w, "\"self\":{\"num\":%d,\"team\":%d,", ps->clientNum, PlayerTeam(ps->clientNum));
    jw_vec(&w, "origin", ps->origin);               jw_raw(&w, ",");
    jw_vec(&w, "velocity", ps->velocity);           jw_raw(&w, ",");
    jw_vec(&w, "viewangles", ps->viewangles);       jw_raw(&w, ",");
    jw_fmt(&w, "\"health\":%d,\"armor\":%d,\"force\":%d,\"weapon\":%d,",
           ps->stats[STAT_HEALTH], ps->stats[STAT_ARMOR], ps->fd.forcePower, ps->weapon);
    jw_fmt(&w, "\"saber_move\":%d,\"saber_blocked\":%d,\"saber_holstered\":%d,",
           ps->saberMove, ps->saberBlocked, ps->saberHolstered);
    jw_fmt(&w, "\"torso_anim\":%d,\"legs_anim\":%d,\"on_ground\":%s,\"duel\":%s,\"lock_time\":%d,\"score\":%d}",
           ps->torsoAnim, ps->legsAnim,
           (ps->groundEntityNum != ENTITYNUM_NONE) ? "true" : "false",
           ps->duelInProgress ? "true" : "false",
           ps->saberLockTime, ps->persistant[PERS_SCORE]);

    /* ---- other players: entities of type ET_PLAYER in this snapshot ---- */
    jw_raw(&w, ",\"players\":[");
    for (i = 0; i < cl.snap.numEntities; ++i) {
        const entityState_t *es =
            &cl.parseEntities[(cl.snap.parseEntitiesNum + i) & (MAX_PARSE_ENTITIES - 1)];
        if (es->eType != ET_PLAYER) continue;
        if (es->number < 0 || es->number >= MAX_CLIENTS) continue;
        if (!first) jw_raw(&w, ",");
        first = 0;
        jw_fmt(&w, "{\"num\":%d,\"team\":%d,", es->number, PlayerTeam(es->number));
        jw_vec(&w, "origin", es->pos.trBase);       jw_raw(&w, ",");   /* players are TR_INTERPOLATE: trBase is the position (bg_misc.c) */
        jw_vec(&w, "angles", es->apos.trBase);      jw_raw(&w, ",");
        jw_fmt(&w, "\"saber_move\":%d,\"torso_anim\":%d,\"legs_anim\":%d,\"weapon\":%d,"
                   "\"eflags\":%d,\"force_active\":%d,\"saber_in_flight\":%s}",
               es->saberMove, es->torsoAnim, es->legsAnim, es->weapon,
               es->eFlags, es->forcePowersActive, es->saberInFlight ? "true" : "false");
    }
    jw_raw(&w, "]");

    if (s_last.valid) {
        jw_fmt(&w, ",\"last_cmd\":{\"fwd\":%d,\"right\":%d,\"up\":%d,\"buttons\":%d,"
                   "\"pitch\":%.3f,\"yaw\":%.3f,\"weapon\":%d,\"forcesel\":%d}",
               s_last.fwd, s_last.right, s_last.up, s_last.buttons,
               s_last.pitch, s_last.yaw, s_last.weapon, s_last.forcesel);
    }
    jw_raw(&w, "}");

    if (!w.overflow) BridgeSend(json, w.len);
}

void CLBridge_OnServerCommand(const char *cmd)
{
    /* Verified against codemp/game/g_cmds.c (G_SayTo): the server sends
     *     chat "Name^7: ^2message" <senderClientNum>
     * (tchat for team chat). The message may itself contain quotes, so take everything between the first
     * quote and the LAST quote. */
    static char json[2048];
    jw_t w;
    const char *p, *q;
    char text[1024];
    int n;

    if (!BridgeEnabled() || s_sock == BRIDGE_INVALID || !s_havePeer || !cmd) return;
    if (strncmp(cmd, "chat \"", 6) == 0)       p = cmd + 6;
    else if (strncmp(cmd, "tchat \"", 7) == 0) p = cmd + 7;
    else return;

    q = strrchr(p, '"');
    n = q ? (int)(q - p) : (int)strlen(p);
    if (n >= (int)sizeof(text)) n = (int)sizeof(text) - 1;
    memcpy(text, p, n);
    text[n] = 0;

    w.buf = json; w.len = 0; w.cap = sizeof(json); w.overflow = 0; json[0] = 0;
    jw_fmt(&w, "{\"v\":%d,\"t\":\"chat\",\"text\":", BRIDGE_PROTOCOL);
    jw_str(&w, text);
    jw_raw(&w, "}");
    if (!w.overflow) BridgeSend(json, w.len);
}
