/*
 * dplay: DirectPlay 4 inside the host, for LAN games between recomp instances.
 *
 * Windows' own DirectPlay (dplayx, the optional "DirectPlay" component) cannot
 * be counted on: it is an optional feature, it is not on Linux, and it needs
 * dplaysvr.exe to own UDP and TCP port 47624. On a machine with WSL's mirrored
 * networking that port is inside the block Windows hands to Linux for its
 * ephemeral ports, so dplaysvr cannot bind it and every
 * Open(DPOPEN_CREATE) fails with DPERR_GENERIC, for the original game too
 * (docs/multiplayer.md). --dplay native keeps the real one.
 *
 * What the game uses (traced): DirectPlayEnumerateA for the connection list,
 * DirectPlayCreate (an SP's GUID) and QueryInterface to IDirectPlay4 (the
 * Unicode one), DirectPlayLobbyCreateA -> IDirectPlayLobby2A::CreateAddress,
 * CoCreateInstance(CLSID_DirectPlay) -> InitializeConnection, EnumSessions,
 * Open, CreatePlayer, Send, Receive, GetMessageCount, GetPlayerName,
 * GetSessionDesc, GetCaps, GetPlayerCaps, DestroyPlayer, CancelMessage, Close.
 * Every other method is here with its real signature (the bridge takes the
 * purge from the callee) and says so the first time it is called.
 *
 * The session: the host process owns it. Each other process keeps one TCP
 * connection to the host, which relays every message (a star), so messages
 * between two processes arrive in the order they were sent, and nothing is
 * lost: every Send is delivered as if DPSEND_GUARANTEED. Finding sessions is
 * a UDP probe to the port (default 2300; never 47624) on 127.0.0.1, the
 * broadcast address of each interface and any --mp-join address; the host
 * answers with its session and TCP port.
 *
 * Threads: the game's threads call the methods (with the machine let go, as
 * for any native call); a network thread of ours reads the sockets and
 * queues messages. Guest code (the EnumSessions callback) is called only on
 * the game's thread, inside EnumSessions, and never with our lock held.
 *
 * The Linux and macOS host (SWGB_WIN32HLE, src/linux) builds this file too:
 * winport.h gives it the Win32 it uses (sockets, threads, events, UTF-16), the
 * methods are win32hle shims (a vtable of shim addresses; each shim lets the
 * machine go and calls the method below with the guest's arguments), guest
 * callbacks go through hle_call_guest, and a player's event is the game's own
 * win32hle event (hle_set_event). 32-bit only: the game's pointers are the
 * host's (flat memory), as under native32.
 */
#ifdef SWGB_WIN32HLE
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include "winport.h"
#include "win32hle.h"
#if UINTPTR_MAX != 0xFFFFFFFFu
#error "dplay.c on win32hle needs the 32-bit host (flat memory: the game's pointers are the host's)"
#endif
typedef socklen_t sock_len;
#define WSTR(s) u##s
#else
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "widescreen.h"
typedef int sock_len;
#define WSTR(s) L##s
#endif
#include "dplay.h"
#include "guest_addrs.h"
#include "recomp_types.h"   /* the dispatch table: --mp-cross's stamp */

typedef DWORD DPID;

/* ---- DirectPlay's structures and codes (dplay.h is not in today's SDK) ---- */
typedef struct {
    DWORD dwSize, dwFlags;
    GUID guidInstance, guidApplication;
    DWORD dwMaxPlayers, dwCurrentPlayers;
    void* lpszSessionName;   /* WCHAR* or char*, by the interface */
    void* lpszPassword;
    DWORD dwReserved1, dwReserved2, dwUser1, dwUser2, dwUser3, dwUser4;
} SD2;                       /* 80 bytes */
typedef struct { DWORD dwSize, dwFlags; void* lpszShortName; void* lpszLongName; } DPN;   /* 16 */
typedef struct {
    DWORD dwSize, dwFlags, dwMaxBufferSize, dwMaxQueueSize, dwMaxPlayers, dwHundredBaud,
          dwLatency, dwMaxLocalPlayers, dwHeaderLength, dwTimeout;
} DPCAPS_;                   /* 40 */

#define DP_OK                    0
#define DPERR_ALREADYINITIALIZED 0x88770005u
#define DPERR_BUFFERTOOSMALL     0x8877001Eu
#define DPERR_CANTCREATEPLAYER   0x8877003Cu
#define DPERR_INVALIDFLAGS       0x88770078u
#define DPERR_INVALIDPLAYER      0x88770096u
#define DPERR_NOCONNECTION       0x887700AAu
#define DPERR_NOMESSAGES         0x887700BEu
#define DPERR_NOSESSIONS         0x887700DCu
#define DPERR_UNAVAILABLE        0x887700FAu
#define DPERR_NOTLOBBIED         0x887703F2u
#define DPERR_GENERIC            0x80004005u
#define DPERR_UNSUPPORTED        0x80004001u
#define DPERR_INVALIDPARAMS      0x80070057u
#define E_NOINTERFACE_           0x80004002u

#define DPOPEN_JOIN   1
#define DPOPEN_CREATE 2
#define DPRECEIVE_TOPLAYER   2
#define DPRECEIVE_FROMPLAYER 4
#define DPRECEIVE_PEEK       8
#define DPENUMSESSIONS_ALL       2
#define DPENUMSESSIONS_ASYNC     0x10
#define DPENUMSESSIONS_STOPASYNC 0x20
#define DPESC_TIMEDOUT 1
#define DPSESSION_NEWPLAYERSDISABLED 0x1
#define DPSESSION_JOINDISABLED       0x20
#define DPCAPS_ISHOST              0x2
#define DPCAPS_GUARANTEEDSUPPORTED 0x40
#define DPPLAYERCAPS_LOCAL         0x800
#define DPSYS_CREATEPLAYERORGROUP  0x0003
#define DPSYS_DESTROYPLAYERORGROUP 0x0005
#define DPSYS_SESSIONLOST          0x0031
#define DPSYS_SETSESSIONDESC       0x0104
#define DPSYS_HOST                 0x0101
#define DPSESSION_MIGRATEHOST        0x4
#define DPPLAYERTYPE_PLAYER 1

static const GUID SP_TCPIP   = { 0x36E95EE0, 0x8577, 0x11CF, { 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 } };
static const GUID CLSID_DP   = { 0xD1EB6D20, 0x8923, 0x11D0, { 0x9D, 0x97, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB } };
static const GUID CLSID_DPL  = { 0x2FE8F810, 0xB2A5, 0x11D0, { 0xA7, 0x87, 0x00, 0x00, 0xF8, 0x03, 0xAB, 0xFC } };
static const GUID AID_TOTAL  = { 0x1318F560, 0x912C, 0x11D0, { 0x9D, 0xAA, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB } };
static const GUID AID_SP     = { 0x07D916C0, 0xE0AF, 0x11CF, { 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E } };
static const GUID AID_INET   = { 0xC4A54DA0, 0xE0AF, 0x11CF, { 0x9C, 0x4E, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E } };
static const GUID AID_INETW  = { 0xE63232A0, 0x9DBF, 0x11D1, { 0x9C, 0xC1, 0x00, 0xA0, 0xC9, 0x05, 0x42, 0x5E } };

/* ---- options -------------------------------------------------------------------- */
static int  g_native;            /* --dplay native */
static int  g_port = 2300;       /* --mp-port */
static char g_join[256];         /* --mp-join a,b */
static int  g_log;               /* --mp-log */
static int  g_cross;             /* --mp-cross: the stamp is the lift's, so Windows and Linux can meet */

/* seconds since the host started, on the lines tools/mp_pair.py times (joins, leaves, drops) */
static DWORD g_t0;
static double secs(void) { if (!g_t0) g_t0 = GetTickCount(); return (GetTickCount() - g_t0) / 1000.0; }

int dplay_arg(int argc, char** argv, int i) {
    if (!g_t0) g_t0 = GetTickCount();
    if (!strcmp(argv[i], "--dplay") && i + 1 < argc) {
        g_native = !_stricmp(argv[i + 1], "native");
#ifdef SWGB_WIN32HLE
        if (g_native) { printf("[dplay] --dplay native: no DirectPlay on this system but ours\n"); g_native = 0; }
#endif
        return 2;
    }
    if (!strcmp(argv[i], "--mp-port") && i + 1 < argc) { g_port = atoi(argv[i + 1]); return 2; }
    if (!strcmp(argv[i], "--mp-join") && i + 1 < argc) { lstrcpynA(g_join, argv[i + 1], sizeof g_join); return 2; }
    if (!strcmp(argv[i], "--mp-log")) { g_log = 1; return 1; }
    if (!strcmp(argv[i], "--mp-cross")) { g_cross = 1; return 1; }
    return 0;
}

#define LOG(...) do { printf("[dplay] " __VA_ARGS__); printf("\n"); } while (0)
#define VLOG(...) do { if (g_log) LOG(__VA_ARGS__); } while (0)

/* ---- the first call of each method, and counts ------------------------------------ */
static const char* const k_dp_names[53] = {
    "QueryInterface", "AddRef", "Release", "AddPlayerToGroup", "Close", "CreateGroup", "CreatePlayer",
    "DeletePlayerFromGroup", "DestroyGroup", "DestroyPlayer", "EnumGroupPlayers", "EnumGroups", "EnumPlayers",
    "EnumSessions", "GetCaps", "GetGroupData", "GetGroupName", "GetMessageCount", "GetPlayerAddress",
    "GetPlayerCaps", "GetPlayerData", "GetPlayerName", "GetSessionDesc", "Initialize", "Open", "Receive", "Send",
    "SetGroupData", "SetGroupName", "SetPlayerData", "SetPlayerName", "SetSessionDesc", "AddGroupToGroup",
    "CreateGroupInGroup", "DeleteGroupFromGroup", "EnumConnections", "EnumGroupsInGroup",
    "GetGroupConnectionSettings", "InitializeConnection", "SecureOpen", "SendChatMessage",
    "SetGroupConnectionSettings", "StartSession", "GetGroupFlags", "GetGroupParent", "GetPlayerAccount",
    "GetPlayerFlags", "GetGroupOwner", "SetGroupOwner", "SendEx", "GetMessageQueue", "CancelMessage",
    "CancelPriority" };
static const char* const k_lob_names[19] = {
    "QueryInterface", "AddRef", "Release", "Connect", "CreateAddress", "EnumAddress", "EnumAddressTypes",
    "EnumLocalApplications", "GetConnectionSettings", "ReceiveLobbyMessage", "RunApplication",
    "SendLobbyMessage", "SetConnectionSettings", "SetLobbyMessageEvent", "CreateCompoundAddress", "ConnectEx",
    "RegisterApplication", "UnregisterApplication", "WaitForConnectionSettings" };
static volatile LONG g_dp_calls[53], g_lob_calls[19];
static void seen(volatile LONG* c, const char* what, const char* name) {
    if (InterlockedIncrement(c) == 1) LOG("first %s::%s", what, name);
}
#define DPM(n) seen(&g_dp_calls[n], "IDirectPlay4", k_dp_names[n])
#define LOBM(n) seen(&g_lob_calls[n], "IDirectPlayLobby", k_lob_names[n])
#define UNSUP(n) do { DPM(n); LOG("IDirectPlay4::%s is not implemented", k_dp_names[n]); return DPERR_UNSUPPORTED; } while (0)

/* ---- strings: the game's interface is the Unicode one, and it passes ANSI strings through it
 * ("<name>'s Game" as a WCHAR*). dplayx takes them as WCHARs up to a 0x0000 and hands the same
 * bytes back, so the game reads its own text; so do we. An A interface is handled too. ---- */
#ifdef SWGB_WIN32HLE
static int wlen(const WCHAR* s) { return s ? wcslen16(s) : 0; }
#else
static int wlen(const WCHAR* s) { return s ? (int)wcslen(s) : 0; }
#endif
/* a name from the game (WCHAR* or char*) into ours */
static void name_in(WCHAR* out, int cap, const void* s, int ansi) {
    out[0] = 0;
    if (!s) return;
    if (ansi) MultiByteToWideChar(CP_ACP, 0, (const char*)s, -1, out, cap);
    else lstrcpynW(out, (const WCHAR*)s, cap);
    out[cap - 1] = 0;
}
/* bytes a name takes in the game's form, with its terminator */
static DWORD name_bytes(const WCHAR* s, int ansi) {
    return ansi ? (DWORD)WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL) : (DWORD)(wlen(s) + 1) * 2;
}
static void name_out(void* dst, const WCHAR* s, int ansi) {
    if (ansi) WideCharToMultiByte(CP_ACP, 0, s, -1, (char*)dst, (int)name_bytes(s, 1), NULL, NULL);
    else memcpy(dst, s, (wlen(s) + 1) * 2);
}

/* ---- the session's state (one per process) ---------------------------------------------- */
#define MAXP 64
#define MAXC 16
#define NAMEN 64

/* a name for the log: UTF-8, anything below a space as \xNN (the CRT's %ls stops at the first
 * character the C locale cannot show, and drops the rest of the line) */
static const char* u8(const WCHAR* w) {
    static __declspec(thread) char b[4][4 * NAMEN + 8];
    static __declspec(thread) int k;
    char* o = b[k++ & 3];
    int n = 0;
    for (const WCHAR* q = w; q && *q; q++)
        if (*q >= 0x100) {   /* ANSI bytes in a WCHAR string: the game's way (see name_in) */
            lstrcpynA(o, (const char*)w, 4 * NAMEN);
            return o;
        }
    for (; w && *w && n < 4 * NAMEN; w++) {
        if (*w < 0x20) n += _snprintf(o + n, 8, "\\x%02X", *w);
        else n += WideCharToMultiByte(CP_UTF8, 0, w, 1, o + n, 4, NULL, NULL);
    }
    o[n] = 0;
    return o;
}

typedef struct {
    DPID id;
    int owner;                 /* -1: this process; -2: behind a connection not yet back after a
                                  host migration; else the connection it lives behind */
    DWORD flags;
    int has_short, has_long;
    WCHAR shortname[NAMEN], longname[NAMEN];
    HANDLE event;              /* a local player's CreatePlayer event */
} player_t;

typedef struct {
    SOCKET s;
    struct sockaddr_in peer;
    uint8_t* rx;
    int rxn, rxcap;
    DWORD last_rx, last_tx;
    int dead;
    u_short lport;             /* host: the client's own listening port (host migration) */
} conn_t;

typedef struct { uint8_t* b; int n, cap; } wbuf;

typedef struct qmsg {
    struct qmsg* next;
    DPID from, to;
    DWORD size;
    int nreloc;
    DWORD reloc[4];           /* offsets of pointers in data, stored as offsets from data */
    uint8_t data[1];
} qmsg_t;

static CRITICAL_SECTION g_cs;
static int g_inited;
static int g_role;            /* 0 none, 1 host, 2 client */
static void* g_owner;         /* the object that opened the session */
static SD2 g_sd;              /* the session (its name pointer unused) */
static WCHAR g_sname[NAMEN];
static player_t g_pl[MAXP];
static int g_npl;
static conn_t g_conn[MAXC];   /* host: its clients; a client: [0] is the host */
static int g_nconn;
static SOCKET g_listen = INVALID_SOCKET, g_disc = INVALID_SOCKET, g_enum = INVALID_SOCKET;
static int g_tcp_port;
static HANDLE g_thread;
static volatile int g_stop;
static qmsg_t *g_qhead, *g_qtail;
static int g_qn;
static DPID g_next_id;
static HANDLE g_reply_ev;     /* a client's WELCOME / CREATED */
static volatile LONG g_reply_type, g_reply_val, g_reply_req;
static char g_target[64];     /* InitializeConnection's address, if one was given */
static LONG64 g_sent_msgs, g_sent_bytes, g_recv_msgs, g_recv_bytes;
static CRITICAL_SECTION g_send_cs;

/* Host migration. The game opens its session with DPSESSION_MIGRATEHOST: when the host goes,
 * the others carry on, and one of them becomes the host (DPSYS_HOST). Each client listens on a
 * port of its own from its join; the host tells every client where the others listen (T_PEERS).
 * When the host's connection closes, the process with the lowest player id among those left
 * takes its listener as the host's, the others connect to it and say which players are theirs
 * (T_REJOIN), and every process destroys the old host's players. What is sent meanwhile waits in
 * g_backlog and goes out once the connection it is for is back. */
typedef struct { DPID id; uint32_t ip; u_short port; } peer_t;
static peer_t g_peers[MAXP];
static int g_npeers;
static SOCKET g_plisten = INVALID_SOCKET;   /* a client's listener */
static u_short g_plisten_port;
static DWORD g_migrate_until;              /* a new host: players not back by then are dropped */
static int g_lost;                         /* a client: the host went, and no new one took over */
typedef struct backlog { struct backlog* next; DPID from, to; wbuf w; } backlog_t;
static backlog_t* g_backlog;

/* sessions seen by EnumSessions */
typedef struct {
    GUID inst;
    struct sockaddr_in addr;  /* the host's TCP address */
    SD2 sd;
    WCHAR name[NAMEN];
    DWORD seen;
} found_t;
static found_t g_found[16];
static int g_nfound;
/* An asynchronous enumeration goes on between the game's calls, as DirectPlay's does: the
 * network thread probes once a second until STOPASYNC, Open or Close. Clone Campaigns asks
 * for the list only every five seconds or so, and a session is listed for four seconds after
 * its last answer: probing only inside EnumSessions, its game blinked in and out of the list
 * and a click on it found nothing to join. */
static volatile int g_async;
static GUID g_async_app;

static void lock(void) { EnterCriticalSection(&g_cs); }
static void unlock(void) { LeaveCriticalSection(&g_cs); }

static player_t* find_player(DPID id) {
    for (int i = 0; i < g_npl; i++) if (g_pl[i].id == id) return &g_pl[i];
    return NULL;
}
static DPID first_local(void) {
    for (int i = 0; i < g_npl; i++) if (g_pl[i].owner == -1) return g_pl[i].id;
    return 0;
}
/* A local player's event is the game's (CreatePlayer's): on win32hle a handle of its own,
 * set holding the machine. */
static void signal_player(DPID to) {
    for (int i = 0; i < g_npl; i++)
        if (g_pl[i].owner == -1 && g_pl[i].event && (to == 0 || g_pl[i].id == to)) {
#ifdef SWGB_WIN32HLE
            mach_enter();
            hle_set_event((uint32_t)(uintptr_t)g_pl[i].event);
            mach_leave();
#else
            SetEvent(g_pl[i].event);
#endif
        }
}

/* queue a message for this process (lock held) */
static qmsg_t* enqueue(DPID from, DPID to, const void* data, DWORD size) {
    qmsg_t* m = (qmsg_t*)calloc(1, sizeof *m + size);
    if (!m) return NULL;
    m->from = from, m->to = to, m->size = size;
    if (data) memcpy(m->data, data, size);
    if (g_qtail) g_qtail->next = m; else g_qhead = m;
    g_qtail = m;
    g_qn++;
    g_recv_msgs++, g_recv_bytes += size;
    signal_player(to);
    return m;
}

/* ---- system messages, built with offsets that Receive turns into pointers ---- */
static void sys_player(int type, const player_t* p) {
    /* DPMSG_CREATEPLAYERORGROUP (48 bytes) or DPMSG_DESTROYPLAYERORGROUP (52), then the names */
    uint8_t b[64 + 4 * NAMEN];
    DWORD n, nameoff;
    memset(b, 0, sizeof b);
    ((DWORD*)b)[0] = type;
    ((DWORD*)b)[1] = DPPLAYERTYPE_PLAYER;
    ((DWORD*)b)[2] = p->id;
    if (type == DPSYS_CREATEPLAYERORGROUP) { ((DWORD*)b)[3] = g_sd.dwCurrentPlayers; nameoff = 24; n = 48; ((DWORD*)b)[11] = p->flags; }
    else { nameoff = 28; n = 52; ((DWORD*)b)[12] = p->flags; }
    DPN* dn = (DPN*)(b + nameoff);
    dn->dwSize = sizeof(DPN);
    int r = 0;
    DWORD rel[2];
    if (p->has_short) { dn->lpszShortName = (void*)(uintptr_t)n; rel[r++] = nameoff + 8; memcpy(b + n, p->shortname, (wlen(p->shortname) + 1) * 2); n += (wlen(p->shortname) + 1) * 2; }
    if (p->has_long) { dn->lpszLongName = (void*)(uintptr_t)n; rel[r++] = nameoff + 12; memcpy(b + n, p->longname, (wlen(p->longname) + 1) * 2); n += (wlen(p->longname) + 1) * 2; }
    qmsg_t* m = enqueue(0, first_local(), b, n);
    if (m) { m->nreloc = r; memcpy(m->reloc, rel, r * sizeof(DWORD)); }
}
static void sys_simple(DWORD type) { VLOG("system message %04lX to %08lX", type, first_local()); enqueue(0, first_local(), &type, 4); }
static void sys_sessiondesc(void) {
    uint8_t b[4 + sizeof(SD2) + NAMEN * 2];
    ((DWORD*)b)[0] = DPSYS_SETSESSIONDESC;
    SD2 sd = g_sd;
    sd.lpszSessionName = (void*)(uintptr_t)(4 + sizeof(SD2));
    sd.lpszPassword = NULL;
    memcpy(b + 4, &sd, sizeof sd);
    memcpy(b + 4 + sizeof sd, g_sname, (wlen(g_sname) + 1) * 2);
    qmsg_t* m = enqueue(0, first_local(), b, 4 + sizeof sd + (wlen(g_sname) + 1) * 2);
    if (m) { m->nreloc = 1; m->reloc[0] = 4 + 48; }
}

/* ---- the wire: [u32 length][u8 type][payload] ---------------------------------- */
enum { T_HELLO = 1, T_WELCOME, T_CREATE, T_CREATED, T_PADD, T_PDEL, T_DATA, T_SDESC, T_PING, T_DESTROY,
       T_PEERS, T_REJOIN };

static void wb_put(wbuf* w, const void* p, int n) {
    if (w->n + n > w->cap) { w->cap = (w->n + n) * 2 + 64; w->b = (uint8_t*)realloc(w->b, w->cap); }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void wb_u32(wbuf* w, DWORD v) { wb_put(w, &v, 4); }
static void wb_str(wbuf* w, const WCHAR* s, int has) {
    DWORD n = has ? (DWORD)wlen(s) : 0xFFFFFFFFu;
    wb_u32(w, n);
    if (has) wb_put(w, s, n * 2);
}
static void wb_begin(wbuf* w, int type) { w->n = 0; wb_u32(w, 0); uint8_t t = (uint8_t)type; wb_put(w, &t, 1); }
static void wb_end(wbuf* w) { *(DWORD*)w->b = (DWORD)(w->n - 4); }

typedef struct { const uint8_t* p; int n, bad; } rbuf;
static DWORD rb_u32(rbuf* r) { DWORD v = 0; if (r->n < 4) { r->bad = 1; return 0; } memcpy(&v, r->p, 4); r->p += 4, r->n -= 4; return v; }
static void rb_get(rbuf* r, void* out, int n) { if (r->n < n) { r->bad = 1; memset(out, 0, n); return; } memcpy(out, r->p, n); r->p += n, r->n -= n; }
static int rb_str(rbuf* r, WCHAR* out, int cap) {
    DWORD n = rb_u32(r);
    out[0] = 0;
    if (n == 0xFFFFFFFFu) return 0;
    if ((int)n * 2 > r->n) { r->bad = 1; return 0; }
    int k = (int)n < cap - 1 ? (int)n : cap - 1;
    memcpy(out, r->p, k * 2);
    out[k] = 0;
    r->p += n * 2, r->n -= n * 2;
    return 1;
}

static void wb_sd(wbuf* w) {
    SD2 sd = g_sd;
    sd.lpszSessionName = sd.lpszPassword = NULL;
    wb_put(w, &sd, sizeof sd);
    wb_str(w, g_sname, 1);
}
static void wb_player(wbuf* w, const player_t* p) {
    wb_u32(w, p->id);
    wb_u32(w, p->flags);
    wb_str(w, p->shortname, p->has_short);
    wb_str(w, p->longname, p->has_long);
}
static void rb_player(rbuf* r, player_t* p) {
    memset(p, 0, sizeof *p);
    p->id = rb_u32(r);
    p->flags = rb_u32(r);
    p->has_short = rb_str(r, p->shortname, NAMEN);
    p->has_long = rb_str(r, p->longname, NAMEN);
}

static int conn_send(conn_t* c, const wbuf* w) {
    int ok = 1;
    EnterCriticalSection(&g_send_cs);
    if (c->s == INVALID_SOCKET || c->dead) ok = 0;
    for (int off = 0; ok && off < w->n;) {
        int k = send(c->s, (const char*)w->b + off, w->n - off, 0);
        if (k <= 0) { ok = 0; c->dead = 1; break; }
        off += k;
    }
    if (ok) c->last_tx = GetTickCount();
    LeaveCriticalSection(&g_send_cs);
    return ok;
}
static void send_all(const wbuf* w, int except) {
    for (int i = 0; i < g_nconn; i++) if (i != except && g_conn[i].s != INVALID_SOCKET) conn_send(&g_conn[i], w);
}

/* This build, for the join handshake: lockstep needs the same code on every machine (the same
 * lift, compiler and CRT; docs/multiplayer.md), and a different build would only show later, as an
 * out-of-sync game. The host exe's link time and size, and the guest exe's link time.
 * --mp-cross (on every side) is the lift's stamp instead: the guest exe's link time and every
 * address in the dispatch table. A Windows and a Linux host of the same lift then meet; whether they
 * compute the same is what the game's sync records tell (tools/mp_pair.py --wsl). */
static DWORD lift_stamp(void) {
    uint32_t h = 2166136261u;   /* FNV-1a over the lifted functions' addresses */
    for (uint32_t i = 0; i < recomp_dispatch_count; i++) {
        uint32_t a = recomp_dispatch_table[i].address;
        for (int k = 0; k < 4; k++) { h ^= (a >> (8 * k)) & 0xFF; h *= 16777619u; }
    }
#ifdef SWGB_WIN32HLE
    uint32_t guest = MEM32(0x00400000u + MEM32(0x0040003Cu) + 8);
#else
    const IMAGE_NT_HEADERS* g = (const IMAGE_NT_HEADERS*)(uintptr_t)(0x00400000u + *(const DWORD*)(uintptr_t)0x0040003Cu);
    uint32_t guest = g->FileHeader.TimeDateStamp;
#endif
    return h ^ guest ^ 0x58504C54u;   /* "XPLT": never a single build's stamp by chance */
}
static DWORD build_stamp(void) {
    static DWORD stamp;
    if (!stamp && g_cross) stamp = lift_stamp();
    if (!stamp) {
#ifdef SWGB_WIN32HLE
        /* the host's file (its time and size: an ELF has no link stamp) and the guest's PE stamp */
        struct stat st;
        uint32_t host = stat("/proc/self/exe", &st) == 0 ? (uint32_t)st.st_mtime ^ ((uint32_t)st.st_size * 0x9E3779B1u) : 0;
        uint32_t guest = MEM32(0x00400000u + MEM32(0x0040003Cu) + 8);
        stamp = host ^ guest ^ 0x4C4E5800u;   /* never equal to a Windows host's (but with --mp-cross) */
#else
        const uint8_t* h = (const uint8_t*)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(h + ((const IMAGE_DOS_HEADER*)h)->e_lfanew);
        const IMAGE_NT_HEADERS* g = (const IMAGE_NT_HEADERS*)(uintptr_t)(0x00400000u + *(const DWORD*)(uintptr_t)0x0040003Cu);
        stamp = nt->FileHeader.TimeDateStamp ^ (nt->OptionalHeader.SizeOfImage * 0x9E3779B1u) ^ g->FileHeader.TimeDateStamp;
#endif
        if (!stamp) stamp = 1;
    }
    return stamp;
}

/* ---- the host's handling of a frame from client c (lock held) ---------------------- */
static void route_data(int origin, DPID from, DPID to, DWORD flags, const uint8_t* data, DWORD size);
static void send_peers(void);
static void conn_close(int i);
static void backlog_flush(int c);
static void backlog_free(void);

static void drop_player(int i, int tell) {
    player_t p = g_pl[i];
    g_pl[i] = g_pl[--g_npl];
    if (g_sd.dwCurrentPlayers) g_sd.dwCurrentPlayers--;
    sys_player(DPSYS_DESTROYPLAYERORGROUP, &p);
    if (tell && g_role == 1) {
        wbuf w = { 0 };
        wb_begin(&w, T_PDEL); wb_u32(&w, p.id); wb_end(&w);
        send_all(&w, p.owner);
        free(w.b);
        send_peers();
    }
    LOG("player %08lX (%s) left (%.1f s)", p.id, u8(p.shortname), secs());
}

/* ---- host migration (g_peers, g_backlog: above) ---- */
static void send_peers(void) {   /* host: where each client listens, by its players (lock held) */
    wbuf w = { 0 };
    int n = 0, at;
    wb_begin(&w, T_PEERS);
    at = w.n;
    wb_u32(&w, 0);
    for (int k = 0; k < g_npl; k++) {
        int o = g_pl[k].owner;
        if (o < 0 || g_conn[o].s == INVALID_SOCKET || !g_conn[o].lport) continue;
        wb_u32(&w, g_pl[k].id);
        wb_u32(&w, g_conn[o].peer.sin_addr.s_addr);
        wb_u32(&w, g_conn[o].lport);
        n++;
    }
    memcpy(w.b + at, &n, 4);
    wb_end(&w);
    send_all(&w, -1);
    free(w.b);
}

static void backlog_add(DPID from, DPID to, const wbuf* w) {
    backlog_t* b = (backlog_t*)calloc(1, sizeof *b);
    if (!b) return;
    b->from = from, b->to = to;
    b->w.b = (uint8_t*)malloc(w->n);
    if (!b->w.b) { free(b); return; }
    memcpy(b->w.b, w->b, w->n);
    b->w.n = b->w.cap = w->n;
    backlog_t** t = &g_backlog;
    while (*t) t = &(*t)->next;
    *t = b;
}
static void backlog_free(void) {
    while (g_backlog) { backlog_t* b = g_backlog; g_backlog = b->next; free(b->w.b); free(b); }
}
/* What waited for connection c: on a client, everything for its new host; on a new host, for a
 * client back after the migration, what went to everyone and what went to its players. */
static void backlog_flush(int c) {
    int n = 0;
    for (backlog_t* b = g_backlog; b; b = b->next) {
        player_t* to = b->to ? find_player(b->to) : NULL;
        if (g_role == 2 || !b->to || (to && to->owner == c)) { conn_send(&g_conn[c], &b->w); n++; }
    }
    if (n) LOG("%d message(s) held during the host migration sent", n);
    if (g_role == 2) backlog_free();
}

static void session_lost(void) {
    LOG("the host is gone: session lost (%.1f s)", secs());
    g_lost = 1;
    for (int k = g_npl - 1; k >= 0; k--) if (g_pl[k].owner != -1) g_pl[k] = g_pl[--g_npl];
    backlog_free();
    sys_simple(DPSYS_SESSIONLOST);
    InterlockedExchange(&g_reply_type, -1);
    SetEvent(g_reply_ev);
}

/* a connection to the new host, tried for up to ms (its own process may not have seen the old
 * host go yet; its listener is up from its join, so a refusal is unexpected but retried) */
static SOCKET connect_within(uint32_t ip, u_short port, int ms) {
    struct sockaddr_in a = { AF_INET };
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < (DWORD)ms; Sleep(100)) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) break;
        if (!connect(s, (struct sockaddr*)&a, sizeof a)) {
            BOOL one = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            return s;
        }
        closesocket(s);
    }
    return INVALID_SOCKET;
}

/* A client whose host is gone (lock held): the old host's players leave, and the process with
 * the lowest player id left becomes the host (DPSYS_HOST); the others connect to it. */
static void migrate(void) {
    for (int k = g_npl - 1; k >= 0; k--) {
        if (g_pl[k].owner == -1) continue;
        int listed = 0;
        for (int j = 0; j < g_npeers; j++) if (g_peers[j].id == g_pl[k].id) listed = 1;
        if (!listed) drop_player(k, 0);   /* the old host's: DPSYS_DESTROYPLAYERORGROUP */
    }
    int bi = -1;
    for (int k = 0; k < g_npl; k++) if (bi < 0 || g_pl[k].id < g_pl[bi].id) bi = k;
    if (bi < 0 || !(g_sd.dwFlags & DPSESSION_MIGRATEHOST) || g_plisten == INVALID_SOCKET) { session_lost(); return; }
    if (g_pl[bi].owner == -1) {
        DPID top = 0;
        g_role = 1;
        g_listen = g_plisten, g_plisten = INVALID_SOCKET, g_tcp_port = g_plisten_port;
        for (int k = 0; k < g_npl; k++) {
            if (g_pl[k].owner != -1) g_pl[k].owner = -2;
            if (g_pl[k].id > top) top = g_pl[k].id;
        }
        if (g_next_id <= top) g_next_id = top + 1;
        g_npeers = 0;
        g_migrate_until = GetTickCount() + 15000;
        sys_simple(DPSYS_HOST);
        LOG("the host is gone (%.1f s): this process hosts now (DPSYS_HOST), on TCP port %d", secs(), g_tcp_port);
        return;
    }
    peer_t* pe = NULL;
    for (int j = 0; j < g_npeers; j++) if (g_peers[j].id == g_pl[bi].id) pe = &g_peers[j];
    SOCKET s = pe ? connect_within(pe->ip, pe->port, 3000) : INVALID_SOCKET;
    if (s == INVALID_SOCKET) { session_lost(); return; }
    memset(&g_conn[0], 0, sizeof g_conn[0]);
    g_conn[0].s = s;
    g_conn[0].peer.sin_family = AF_INET;
    g_conn[0].peer.sin_addr.s_addr = pe->ip;
    g_conn[0].peer.sin_port = htons(pe->port);
    g_conn[0].last_rx = g_conn[0].last_tx = GetTickCount();
    if (g_nconn < 1) g_nconn = 1;
    for (int k = 0; k < g_npl; k++) if (g_pl[k].owner != -1) g_pl[k].owner = 0;
    wbuf w = { 0 };
    int n = 0, at;
    wb_begin(&w, T_REJOIN);
    wb_u32(&w, g_plisten_port);
    at = w.n;
    wb_u32(&w, 0);
    for (int k = 0; k < g_npl; k++) if (g_pl[k].owner == -1) { wb_u32(&w, g_pl[k].id); n++; }
    memcpy(w.b + at, &n, 4);
    wb_end(&w);
    conn_send(&g_conn[0], &w);
    free(w.b);
    LOG("the host is gone (%.1f s): rejoined the session at %s:%u, where player %08lX hosts", secs(),
        inet_ntoa(g_conn[0].peer.sin_addr), pe->port, g_pl[bi].id);
    backlog_flush(0);
}

static void host_frame(int c, int type, rbuf* r) {
    wbuf w = { 0 };
    switch (type) {
    case T_HELLO: {
        GUID inst;
        rb_get(r, &inst, sizeof inst);
        DWORD theirs = rb_u32(r), mine = build_stamp();
        if (r->n >= 4) g_conn[c].lport = (u_short)rb_u32(r);   /* where it listens (host migration) */
        /* SWGB_MP_ANY_BUILD=1: let it in anyway (mp_pair.py --run-b: two builds of one source at
         * other flags, whose sync records then show whether they simulate alike) */
        int any = getenv("SWGB_MP_ANY_BUILD") && *getenv("SWGB_MP_ANY_BUILD") == '1';
        if (theirs != mine)
            LOG("a client with another build (%08lX; this one is %08lX%s): %s", theirs, mine,
                g_cross ? ", --mp-cross" : "; the same lift on another system needs --mp-cross on both",
                any ? "let in (SWGB_MP_ANY_BUILD)" : "refused");
        int ok = (theirs == mine || any) && IsEqualGUID(&inst, &g_sd.guidInstance) &&
                 !(g_sd.dwFlags & (DPSESSION_JOINDISABLED | DPSESSION_NEWPLAYERSDISABLED)) &&
                 (!g_sd.dwMaxPlayers || g_sd.dwCurrentPlayers < g_sd.dwMaxPlayers);
        wb_begin(&w, T_WELCOME);
        wb_u32(&w, ok);
        wb_sd(&w);
        wb_u32(&w, g_npl);
        for (int i = 0; i < g_npl; i++) wb_player(&w, &g_pl[i]);
        wb_end(&w);
        conn_send(&g_conn[c], &w);
        LOG("a client joined from %s:%u (%s)", inet_ntoa(g_conn[c].peer.sin_addr), ntohs(g_conn[c].peer.sin_port), ok ? "welcome" : "refused");
        if (!ok) g_conn[c].dead = 1;
        break;
    }
    case T_CREATE: {
        DWORD req = rb_u32(r);
        player_t p;
        rb_player(r, &p);
        p.id = g_next_id++;
        p.owner = c;
        if (g_npl < MAXP) {
            g_pl[g_npl++] = p;
            g_sd.dwCurrentPlayers++;
        } else p.id = 0;
        wb_begin(&w, T_CREATED); wb_u32(&w, req); wb_u32(&w, p.id); wb_end(&w);
        conn_send(&g_conn[c], &w);
        if (p.id) {
            wb_begin(&w, T_PADD); wb_player(&w, &p); wb_u32(&w, g_sd.dwCurrentPlayers); wb_end(&w);
            send_all(&w, c);
            sys_player(DPSYS_CREATEPLAYERORGROUP, &p);
            LOG("player %08lX (%s) joined (%.1f s)", p.id, u8(p.shortname), secs());
            send_peers();
        }
        break;
    }
    case T_REJOIN: {   /* a client back after the host migration, at its new host */
        if (g_role == 2 && g_conn[0].s != INVALID_SOCKET) conn_close(0);   /* it saw the old host go first */
        if (g_role != 1) break;
        DWORD lport = rb_u32(r), n = rb_u32(r), got = 0;
        g_conn[c].lport = (u_short)lport;
        for (DWORD i = 0; i < n && !r->bad; i++) {
            player_t* p = find_player(rb_u32(r));
            if (p && p->owner == -2) { p->owner = c; got++; }
        }
        LOG("client %d (%s) is back after the host migration: %lu player(s)", c, inet_ntoa(g_conn[c].peer.sin_addr), got);
        backlog_flush(c);
        int pending = 0;
        for (int k = 0; k < g_npl; k++) if (g_pl[k].owner == -2) pending = 1;
        if (!pending) { backlog_free(); g_migrate_until = 0; }
        send_peers();
        break;
    }
    case T_DESTROY: {
        DPID id = rb_u32(r);
        for (int i = 0; i < g_npl; i++) if (g_pl[i].id == id && g_pl[i].owner == c) { drop_player(i, 1); break; }
        break;
    }
    case T_DATA: {
        DPID from = rb_u32(r), to = rb_u32(r);
        DWORD flags = rb_u32(r);
        if (!r->bad) route_data(c, from, to, flags, r->p, (DWORD)r->n);
        break;
    }
    case T_PING: break;
    default: LOG("host: frame type %d from a client?", type);
    }
    free(w.b);
}

/* a client's handling of a frame from the host (lock held) */
static void client_frame(int type, rbuf* r) {
    switch (type) {
    case T_WELCOME: {
        DWORD ok = rb_u32(r);
        rb_get(r, &g_sd, sizeof g_sd);
        rb_str(r, g_sname, NAMEN);
        DWORD n = rb_u32(r);
        g_npl = 0;
        for (DWORD i = 0; i < n && i < MAXP && !r->bad; i++) { rb_player(r, &g_pl[g_npl]); g_pl[g_npl++].owner = 0; }
        g_reply_val = (LONG)ok;
        InterlockedExchange(&g_reply_type, T_WELCOME);
        SetEvent(g_reply_ev);
        break;
    }
    case T_CREATED: {
        DWORD req = rb_u32(r), id = rb_u32(r);
        if ((LONG)req == g_reply_req) { g_reply_val = (LONG)id; InterlockedExchange(&g_reply_type, T_CREATED); SetEvent(g_reply_ev); }
        break;
    }
    case T_PADD: {
        player_t p;
        rb_player(r, &p);
        p.owner = 0;
        DWORD cur = rb_u32(r);
        if (!find_player(p.id) && g_npl < MAXP) g_pl[g_npl++] = p;
        g_sd.dwCurrentPlayers = cur;
        sys_player(DPSYS_CREATEPLAYERORGROUP, &p);
        LOG("player %08lX (%s) joined (%.1f s)", p.id, u8(p.shortname), secs());
        break;
    }
    case T_PDEL: {
        DPID id = rb_u32(r);
        for (int i = 0; i < g_npl; i++) if (g_pl[i].id == id) { drop_player(i, 0); break; }
        break;
    }
    case T_DATA: {
        DPID from = rb_u32(r), to = rb_u32(r);
        rb_u32(r);
        if (r->bad) break;
        if (to == 0) {   /* to all: each local player but the sender */
            for (int i = 0; i < g_npl; i++)
                if (g_pl[i].owner == -1 && g_pl[i].id != from) enqueue(from, g_pl[i].id, r->p, (DWORD)r->n);
        } else enqueue(from, to, r->p, (DWORD)r->n);
        break;
    }
    case T_SDESC: {
        rb_get(r, &g_sd, sizeof g_sd);
        rb_str(r, g_sname, NAMEN);
        sys_sessiondesc();
        break;
    }
    case T_PEERS: {
        DWORD n = rb_u32(r);
        g_npeers = 0;
        for (DWORD i = 0; i < n && i < MAXP && !r->bad; i++) {
            g_peers[g_npeers].id = rb_u32(r);
            g_peers[g_npeers].ip = rb_u32(r);
            g_peers[g_npeers].port = (u_short)rb_u32(r);
            g_npeers++;
        }
        VLOG("%d peer(s) to migrate to", g_npeers);
        break;
    }
    case T_PING: break;
    default: LOG("client: frame type %d from the host?", type);
    }
}

/* the host routes a message: origin is the connection it came from, or -1 (this process) */
static void route_data(int origin, DPID from, DPID to, DWORD flags, const uint8_t* data, DWORD size) {
    wbuf w = { 0 };
    (void)flags;
    if (to == 0) {
        for (int i = 0; i < g_npl; i++)
            if (g_pl[i].owner == -1 && g_pl[i].id != from) enqueue(from, g_pl[i].id, data, size);
    } else {
        player_t* p = find_player(to);
        if (!p) { VLOG("a message to %08lX, who is not here", to); return; }
        if (p->owner == -1) { enqueue(from, to, data, size); return; }
    }
    wb_begin(&w, T_DATA); wb_u32(&w, from); wb_u32(&w, to); wb_u32(&w, flags); wb_put(&w, data, size); wb_end(&w);
    if (to == 0) {
        int pending = 0;
        for (int i = 0; i < g_nconn; i++) {
            if (i == origin || g_conn[i].s == INVALID_SOCKET) continue;
            int any = 0;   /* only connections with a player behind them */
            for (int k = 0; k < g_npl; k++) if (g_pl[k].owner == i && g_pl[k].id != from) any = 1;
            if (any) conn_send(&g_conn[i], &w);
        }
        for (int k = 0; k < g_npl; k++) if (g_pl[k].owner == -2) pending = 1;
        if (pending) backlog_add(from, 0, &w);   /* for the clients not back yet */
    } else {
        int o = find_player(to)->owner;
        if (o >= 0) conn_send(&g_conn[o], &w); else backlog_add(from, to, &w);
    }
    free(w.b);
}

/* ---- the network thread ---------------------------------------------------------------- */
static void conn_close(int i) {
    conn_t* c = &g_conn[i];
    if (c->s == INVALID_SOCKET) return;
    closesocket(c->s);
    c->s = INVALID_SOCKET;
    free(c->rx);
    c->rx = NULL;
    c->rxn = c->rxcap = 0;
    if (g_role == 1) {
        LOG("client %d (%s) gone (%.1f s)", i, inet_ntoa(c->peer.sin_addr), secs());
        for (int k = g_npl - 1; k >= 0; k--) if (g_pl[k].owner == i) drop_player(k, 1);
    } else if (g_role == 2 && i == 0) {
        migrate();   /* the host: carry on without it, or the session is lost */
    }
}

static void conn_read(int i) {
    conn_t* c = &g_conn[i];
    if (c->rxcap - c->rxn < 65536) { c->rxcap = c->rxcap * 2 + 65536; c->rx = (uint8_t*)realloc(c->rx, c->rxcap); }
    int k = recv(c->s, (char*)c->rx + c->rxn, c->rxcap - c->rxn, 0);
    if (k <= 0) { conn_close(i); return; }
    c->rxn += k;
    c->last_rx = GetTickCount();
    int off = 0;
    while (c->rxn - off >= 5) {
        DWORD len;
        memcpy(&len, c->rx + off, 4);
        if (len > (16u << 20) || len < 1) { LOG("a bad frame (%lu bytes): closing", len); conn_close(i); return; }
        if (c->rxn - off < 4 + (int)len) break;
        rbuf r = { c->rx + off + 5, (int)len - 1, 0 };
        int type = c->rx[off + 4];
        if (g_role == 1 || i > 0) host_frame(i, type, &r); else client_frame(type, &r);   /* i > 0 on a client: a peer back after a migration */
        if (c->s == INVALID_SOCKET) return;
        off += 4 + (int)len;
    }
    memmove(c->rx, c->rx + off, c->rxn - off);
    c->rxn -= off;
}

/* the UDP probe ("SWGBDPE1" + the application's GUID) and the answer ("SWGBDPS1" + TCP port + session) */
static void disc_read(void) {
    uint8_t b[1500];
    struct sockaddr_in from;
    sock_len fl = sizeof from;
    int k = recvfrom(g_disc, (char*)b, sizeof b, 0, (struct sockaddr*)&from, &fl);
    if (k < 8 + 16 || memcmp(b, "SWGBDPE1", 8)) return;
    GUID app, zero = { 0 };
    memcpy(&app, b + 8, 16);
    if (!IsEqualGUID(&app, &zero) && !IsEqualGUID(&app, &g_sd.guidApplication)) return;
    wbuf w = { 0 };
    wb_put(&w, "SWGBDPS1", 8);
    wb_u32(&w, (DWORD)g_tcp_port);
    wb_sd(&w);
    sendto(g_disc, (const char*)w.b, w.n, 0, (struct sockaddr*)&from, sizeof from);
    free(w.b);
    VLOG("answered a probe from %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
}

static void enum_read(void) {
    uint8_t b[1500];
    struct sockaddr_in from;
    sock_len fl = sizeof from;
    int k = recvfrom(g_enum, (char*)b, sizeof b, 0, (struct sockaddr*)&from, &fl);
    if (k < 12 || memcmp(b, "SWGBDPS1", 8)) return;
    rbuf r = { b + 8, k - 8, 0 };
    found_t f;
    memset(&f, 0, sizeof f);
    DWORD port = rb_u32(&r);
    rb_get(&r, &f.sd, sizeof f.sd);
    rb_str(&r, f.name, NAMEN);
    if (r.bad) return;
    f.inst = f.sd.guidInstance;
    f.addr = from;
    f.addr.sin_port = htons((u_short)port);
    f.seen = GetTickCount();
    int i;
    for (i = 0; i < g_nfound; i++) if (IsEqualGUID(&g_found[i].inst, &f.inst)) break;
    if (i < g_nfound) {   /* the same session again: keep the address it was first seen at */
        f.addr = g_found[i].addr;
        g_found[i] = f;
    } else if (g_nfound < 16) {
        g_found[g_nfound++] = f;
        LOG("found \"%s\" at %s:%lu (%lu/%lu players)", u8(f.name), inet_ntoa(from.sin_addr), port, f.sd.dwCurrentPlayers, f.sd.dwMaxPlayers);
    }
}

/* ---- out of sync: the game's own records, logged so two instances can be compared ----------
 * The communications handler (G_COMM) keeps, at +0x1600, a table of 54 records of nine dwords
 * from +0x38 (the `this` of sub_00448EF0): per player and turn, the world time and the checksums its out-of-sync check
 * (sub_00440260, "SaveGameChecksum: %ld, Me: %ld, oosWorldTime") compares between players.
 * Each record is printed once it has held still for a second ("[sync] slot: nine words"):
 * tools/mp_sync.py matches the records two logs share and reports any that differ. */
#define SYNC_N 54
static uint32_t g_sync_prev[SYNC_N][9], g_sync_logged[SYNC_N][9];
static LONG64 g_sync_lines;
#ifdef SWGB_WIN32HLE
/* sync_sample holds the machine (net_thread), so the game frees nothing meanwhile */
static uint32_t peek32(uint32_t a) { return a >= 0x10000 && a < 0xC0000000u ? MEM32(a) : 0; }
#else
static uint32_t peek32(uint32_t a) {
    __try { return a >= 0x10000 ? *(volatile uint32_t*)(uintptr_t)a : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
#endif
static void sync_sample(void) {
    uint32_t comm = peek32(G_COMM), t = comm ? peek32(comm + 0x1600) : 0;
    if (!t) return;
    for (int j = 0; j < SYNC_N; j++) {
        uint32_t cur[9], any = 0;
        for (int k = 0; k < 9; k++) any |= cur[k] = peek32(t + 0x38 + j * 36 + k * 4);
        if (any && !memcmp(cur, g_sync_prev[j], sizeof cur) && memcmp(cur, g_sync_logged[j], sizeof cur)) {
            printf("[sync] %2d: %08X %08X %08X %08X %08X %08X %08X %08X %08X\n", j,
                   cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], cur[6], cur[7], cur[8]);
            memcpy(g_sync_logged[j], cur, sizeof cur);
            g_sync_lines++;
        }
        memcpy(g_sync_prev[j], cur, sizeof cur);
    }
}

/* The game's out-of-sync messages, as it loads them to show them. */
void dplay_string(unsigned id) {
    if (id == 1211 || id == 2012 || (id >= 2406 && id <= 2413) || id == 9013 || id == 9800 || id == 9801)
        printf("[mp] the game loaded string %u (%s)\n", id, id == 1211 ? "resynchronizing" : "OUT OF SYNC");
}
#ifndef SWGB_WIN32HLE
void dplay_LoadStringA(void) {
    dplay_string(MEM32(g_esp + 8));
    widescreen_LoadStringA();
}
#endif

static void probe_all(const GUID* app);

static DWORD WINAPI net_thread(LPVOID unused) {
    DWORD last_sync = GetTickCount();
    (void)unused;
    while (!g_stop) {
        fd_set rd;
        if (GetTickCount() - last_sync >= 1000) {
            last_sync = GetTickCount();
#ifdef SWGB_WIN32HLE
            mach_enter();
            sync_sample();
            mach_leave();
#else
            sync_sample();
#endif
        }
        static DWORD last_probe;
        if (g_async && g_enum != INVALID_SOCKET && GetTickCount() - last_probe >= 1000) {
            last_probe = GetTickCount();
            probe_all(&g_async_app);
        }
        FD_ZERO(&rd);
        int nfd = 0, maxfd = 0;   /* select's first argument: POSIX needs it, Windows ignores it */
#define ADD_FD(s_) do { FD_SET((s_), &rd); nfd++; if ((int)(s_) > maxfd) maxfd = (int)(s_); } while (0)
        lock();
        if (g_listen != INVALID_SOCKET) ADD_FD(g_listen);
        if (g_plisten != INVALID_SOCKET) ADD_FD(g_plisten);
        if (g_disc != INVALID_SOCKET) ADD_FD(g_disc);
        if (g_enum != INVALID_SOCKET) ADD_FD(g_enum);
        for (int i = 0; i < g_nconn; i++) {
            if (g_conn[i].dead && g_conn[i].s != INVALID_SOCKET) conn_close(i);
            if (g_conn[i].s != INVALID_SOCKET) ADD_FD(g_conn[i].s);
        }
        /* a ping each second each way; 30 s of silence is a drop (a pulled cable sends no FIN) */
        DWORD now = GetTickCount();
        for (int i = 0; i < g_nconn; i++) {
            conn_t* c = &g_conn[i];
            if (c->s == INVALID_SOCKET) continue;
            if (now - c->last_tx > 1000) { wbuf w = { 0 }; wb_begin(&w, T_PING); wb_end(&w); conn_send(c, &w); free(w.b); }
            if (now - c->last_rx > 30000) { LOG("connection %d silent for 30 s: dropped (%.1f s)", i, secs()); conn_close(i); }
        }
        if (g_role == 1 && g_migrate_until && (int)(now - g_migrate_until) > 0) {   /* not back after a migration */
            for (int k = g_npl - 1; k >= 0; k--) if (g_pl[k].owner == -2) drop_player(k, 1);
            backlog_free();
            g_migrate_until = 0;
        }
        unlock();
        struct timeval tv = { 0, 50000 };
        if (nfd == 0) { Sleep(50); continue; }
        int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (n <= 0) continue;
        lock();
        for (int l = 0; l < 2; l++) {   /* the host's listener; a client's (a peer, after a host migration) */
            SOCKET ls = l ? g_plisten : g_listen;
            if (ls == INVALID_SOCKET || !FD_ISSET(ls, &rd)) continue;
            struct sockaddr_in a;
            sock_len al = sizeof a;
            SOCKET s = accept(ls, (struct sockaddr*)&a, &al);
            if (s != INVALID_SOCKET) {
                int i;
                for (i = g_role == 2 ? 1 : 0; i < g_nconn; i++) if (g_conn[i].s == INVALID_SOCKET) break;   /* a client's [0] is its host */
                if (i == MAXC) closesocket(s);
                else {
                    BOOL one = TRUE;
                    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
                    memset(&g_conn[i], 0, sizeof g_conn[i]);
                    g_conn[i].s = s;
                    g_conn[i].peer = a;
                    g_conn[i].last_rx = g_conn[i].last_tx = GetTickCount();
                    if (i == g_nconn) g_nconn++;
                }
            }
        }
        if (g_disc != INVALID_SOCKET && FD_ISSET(g_disc, &rd)) disc_read();
        if (g_enum != INVALID_SOCKET && FD_ISSET(g_enum, &rd)) enum_read();
        for (int i = 0; i < g_nconn; i++)
            if (g_conn[i].s != INVALID_SOCKET && FD_ISSET(g_conn[i].s, &rd)) conn_read(i);
        unlock();
    }
    return 0;
}

static void net_start(void) {
    if (g_thread) return;
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
}

static void net_stop(void) {
    if (!g_thread) return;
    g_stop = 1;
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
    lock();
    for (int i = 0; i < g_nconn; i++) if (g_conn[i].s != INVALID_SOCKET) { closesocket(g_conn[i].s); g_conn[i].s = INVALID_SOCKET; free(g_conn[i].rx); g_conn[i].rx = NULL; }
    g_nconn = 0;
    if (g_listen != INVALID_SOCKET) { closesocket(g_listen); g_listen = INVALID_SOCKET; }
    if (g_plisten != INVALID_SOCKET) { closesocket(g_plisten); g_plisten = INVALID_SOCKET; }
    if (g_disc != INVALID_SOCKET) { closesocket(g_disc); g_disc = INVALID_SOCKET; }
    g_npeers = 0;
    g_migrate_until = 0;
    backlog_free();
    unlock();
}

/* the probe socket lives from the first EnumSessions to Close */
static int enum_socket(void) {
    if (g_enum != INVALID_SOCKET) return 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
    struct sockaddr_in a = { AF_INET };
    bind(s, (struct sockaddr*)&a, sizeof a);
    lock();
    g_enum = s;
    unlock();
    net_start();
    return 1;
}

static void probe_to(uint32_t ip_net, const GUID* app) {
    uint8_t b[24];
    memcpy(b, "SWGBDPE1", 8);
    memcpy(b + 8, app, 16);
    for (int k = 0; k < 4; k++) {   /* a second host on one machine takes the next port */
        struct sockaddr_in a = { AF_INET };
        a.sin_addr.s_addr = ip_net;
        a.sin_port = htons((u_short)(g_port + k));
        sendto(g_enum, (const char*)b, sizeof b, 0, (struct sockaddr*)&a, sizeof a);
    }
}

/* loopback, each interface's broadcast address, InitializeConnection's address, --mp-join */
static void probe_all(const GUID* app) {
    probe_to(htonl(INADDR_LOOPBACK), app);
    probe_to(INADDR_BROADCAST, app);
#ifdef SWGB_WIN32HLE
    struct ifaddrs* ifs = NULL;
    if (!getifaddrs(&ifs)) {
        for (struct ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t a = ((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr, m = ((struct sockaddr_in*)i->ifa_netmask)->sin_addr.s_addr;
            probe_to(a | ~m, app);
        }
        freeifaddrs(ifs);
    }
#else
    INTERFACE_INFO ii[32];
    DWORD got = 0;
    if (!WSAIoctl(g_enum, SIO_GET_INTERFACE_LIST, NULL, 0, ii, sizeof ii, &got, NULL, NULL)) {
        for (int i = 0; i < (int)(got / sizeof ii[0]); i++) {
            if (!(ii[i].iiFlags & IFF_UP) || (ii[i].iiFlags & IFF_LOOPBACK) || ii[i].iiAddress.AddressIn.sin_family != AF_INET) continue;
            uint32_t a = ii[i].iiAddress.AddressIn.sin_addr.s_addr, m = ii[i].iiNetmask.AddressIn.sin_addr.s_addr;
            probe_to(a | ~m, app);
        }
    }
#endif
    char list[sizeof g_join + 70];
    _snprintf(list, sizeof list, "%s,%s", g_join, g_target);
    list[sizeof list - 1] = 0;
    for (char* t = strtok(list, ","); t; t = strtok(NULL, ",")) {
        struct addrinfo hints = { 0 }, *res = NULL;
        hints.ai_family = AF_INET;
        if (*t && !getaddrinfo(t, NULL, &hints, &res) && res) {
            probe_to(((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr, app);
            freeaddrinfo(res);
        }
    }
}

/* ---- the game's callbacks: called directly under native32, through hle_call_guest on win32hle ---- */
#ifdef SWGB_WIN32HLE
static BOOL guest_cb(void* cb, int n, ...) {
    uint32_t a[8];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    /* The bridge let the machine go (hle_block_begin, which keeps this thread's nesting count):
     * take it back first, or hle_call_guest would run the callback without it. */
    hle_block_end();
    BOOL r = hle_call_guest((uint32_t)(uintptr_t)cb, n, a) != 0;
    hle_block_begin();
    return r;
}
#define U_(x) ((uint32_t)(uintptr_t)(x))
#define GUEST_CB4(T, cb, a, b, c, d) guest_cb((cb), 4, U_(a), U_(b), U_(c), U_(d))
#define GUEST_CB5(T, cb, a, b, c, d, e) guest_cb((cb), 5, U_(a), U_(b), U_(c), U_(d), U_(e))
#define GUEST_CB6(T, cb, a, b, c, d, e, f) guest_cb((cb), 6, U_(a), U_(b), U_(c), U_(d), U_(e), U_(f))
#else
#define GUEST_CB4(T, cb, a, b, c, d) ((T)(cb))(a, b, c, d)
#define GUEST_CB5(T, cb, a, b, c, d, e) ((T)(cb))(a, b, c, d, e)
#define GUEST_CB6(T, cb, a, b, c, d, e, f) ((T)(cb))(a, b, c, d, e, f)
#endif

/* ---- the IDirectPlay4 object ---------------------------------------------------------------- */
typedef struct { void** vtbl; LONG refs; int ansi; } dpobj;
static void* g_dp_vtbl[53];
static void* g_lob_vtbl[19];

static int dp_iid(const GUID* r, int* ansi) {
    static const DWORD w[] = { 0x2B74F7C0, 0x133EFE40, 0x0AB1C530 }, a[] = { 0x9D460580, 0x133EFE41, 0x0AB1C531 };
    static const GUID unk = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    if (IsEqualGUID(r, &unk)) return 1;
    for (int i = 0; i < 3; i++) if (r->Data1 == w[i]) { *ansi = 0; return 1; }
    for (int i = 0; i < 3; i++) if (r->Data1 == a[i]) { *ansi = 1; return 1; }
    return 0;
}

static dpobj* dp_new(int ansi) {
    dpobj* o = (dpobj*)calloc(1, sizeof *o);
    o->vtbl = g_dp_vtbl;
    o->refs = 1;
    o->ansi = ansi;
    return o;
}

static void* dp1_new(dpobj* dp);
static HRESULT __stdcall dp_QueryInterface(dpobj* o, const GUID* riid, void** out) {
    DPM(0);
    int ansi = o->ansi;
    if (!out) return DPERR_INVALIDPARAMS;
    if (riid->Data1 == 0x5454E9A0) {   /* IID_IDirectPlay: a layout of its own */
        InterlockedIncrement(&o->refs);
        *out = dp1_new(o);
        return DP_OK;
    }
    if (!dp_iid(riid, &ansi)) { *out = NULL; LOG("QueryInterface for {%08lX-...}: no", riid->Data1); return E_NOINTERFACE_; }
    /* One object for every interface; the strings follow the last one asked for. */
    o->ansi = ansi;
    InterlockedIncrement(&o->refs);
    *out = o;
    return DP_OK;
}
static ULONG __stdcall dp_AddRef(dpobj* o) { DPM(1); return (ULONG)InterlockedIncrement(&o->refs); }
static HRESULT __stdcall dp_Close(dpobj* o);
static ULONG __stdcall dp_Release(dpobj* o) {
    DPM(2);
    LONG r = InterlockedDecrement(&o->refs);
    if (r == 0) { if (g_owner == o) dp_Close(o); free(o); }
    return (ULONG)r;
}

static HRESULT __stdcall dp_AddPlayerToGroup(dpobj* o, DPID g, DPID p) { (void)o; (void)g; (void)p; UNSUP(3); }

static HRESULT __stdcall dp_Close(dpobj* o) {
    DPM(4);
    g_async = 0;
    if (g_owner != o) return DP_OK;
    LOG("Close (%s) (%.1f s)", g_role == 1 ? "host" : g_role == 2 ? "client" : "-", secs());
    net_stop();
    lock();
    g_role = 0;
    g_owner = NULL;
    g_npl = 0;
    while (g_qhead) { qmsg_t* m = g_qhead; g_qhead = m->next; free(m); }
    g_qtail = NULL;
    g_qn = 0;
    if (g_enum != INVALID_SOCKET) { closesocket(g_enum); g_enum = INVALID_SOCKET; }
    if (g_plisten != INVALID_SOCKET) { closesocket(g_plisten); g_plisten = INVALID_SOCKET; }
    backlog_free();
    unlock();
    return DP_OK;
}

static HRESULT __stdcall dp_CreateGroup(dpobj* o, DPID* id, DPN* n, void* d, DWORD ds, DWORD f) { (void)o; (void)id; (void)n; (void)d; (void)ds; (void)f; UNSUP(5); }

static HRESULT __stdcall dp_CreatePlayer(dpobj* o, DPID* lpid, DPN* name, HANDLE ev, void* data, DWORD dsize, DWORD flags) {
    DPM(6);
    if (!lpid) return DPERR_INVALIDPARAMS;
    if (!g_role) return DPERR_NOCONNECTION;
    player_t p;
    memset(&p, 0, sizeof p);
    p.owner = -1;
    p.flags = flags;
    p.event = ev;
    if (name) {
        p.has_short = name->lpszShortName != NULL;
        p.has_long = name->lpszLongName != NULL;
        name_in(p.shortname, NAMEN, name->lpszShortName, o->ansi);
        name_in(p.longname, NAMEN, name->lpszLongName, o->ansi);
    }
    if (dsize) VLOG("CreatePlayer: %lu bytes of player data (not kept)", dsize);
    (void)data;
    if (g_role == 1) {
        lock();
        if (g_npl >= MAXP) { unlock(); return DPERR_CANTCREATEPLAYER; }
        p.id = g_next_id++;
        g_pl[g_npl++] = p;
        g_sd.dwCurrentPlayers++;
        wbuf w = { 0 };
        wb_begin(&w, T_PADD); wb_player(&w, &p); wb_u32(&w, g_sd.dwCurrentPlayers); wb_end(&w);
        send_all(&w, -1);
        free(w.b);
        unlock();
    } else {
        static LONG req;
        wbuf w = { 0 };
        LONG me = InterlockedIncrement(&req);
        lock();
        g_reply_req = me;
        g_reply_type = 0;
        ResetEvent(g_reply_ev);
        wb_begin(&w, T_CREATE); wb_u32(&w, (DWORD)me); wb_player(&w, &p); wb_end(&w);
        int ok = conn_send(&g_conn[0], &w);
        unlock();
        free(w.b);
        if (!ok || WaitForSingleObject(g_reply_ev, 10000) != WAIT_OBJECT_0 || g_reply_type != T_CREATED || !g_reply_val) {
            LOG("CreatePlayer: no answer from the host");
            return DPERR_CANTCREATEPLAYER;
        }
        p.id = (DPID)g_reply_val;
        lock();
        g_pl[g_npl++] = p;
        unlock();
    }
    *lpid = p.id;
    LOG("CreatePlayer \"%s\" -> %08lX (flags %lX, event %p)", u8(p.shortname), p.id, flags, ev);
    return DP_OK;
}

static HRESULT __stdcall dp_DeletePlayerFromGroup(dpobj* o, DPID g, DPID p) { (void)o; (void)g; (void)p; UNSUP(7); }
static HRESULT __stdcall dp_DestroyGroup(dpobj* o, DPID g) { (void)o; (void)g; UNSUP(8); }

static HRESULT __stdcall dp_DestroyPlayer(dpobj* o, DPID id) {
    DPM(9);
    (void)o;
    lock();
    player_t* p = find_player(id);
    if (!p || p->owner != -1) { unlock(); return DPERR_INVALIDPLAYER; }
    int i = (int)(p - g_pl);
    if (g_role == 2) {
        wbuf w = { 0 };
        wb_begin(&w, T_DESTROY); wb_u32(&w, id); wb_end(&w);
        conn_send(&g_conn[0], &w);
        free(w.b);
        g_pl[i] = g_pl[--g_npl];
    } else {
        player_t q = *p;
        g_pl[i] = g_pl[--g_npl];
        if (g_sd.dwCurrentPlayers) g_sd.dwCurrentPlayers--;
        wbuf w = { 0 };
        wb_begin(&w, T_PDEL); wb_u32(&w, q.id); wb_end(&w);
        send_all(&w, -1);
        free(w.b);
    }
    unlock();
    LOG("DestroyPlayer %08lX (%.1f s)", id, secs());
    return DP_OK;
}

typedef BOOL (__stdcall *ENUMPLAYERS_CB)(DPID, DWORD, const DPN*, DWORD, void*);
static HRESULT __stdcall dp_EnumGroupPlayers(dpobj* o, DPID g, GUID* inst, void* cb, void* ctx, DWORD f) { (void)o; (void)g; (void)inst; (void)cb; (void)ctx; (void)f; UNSUP(10); }
static HRESULT __stdcall dp_EnumGroups(dpobj* o, GUID* inst, void* cb, void* ctx, DWORD f) { (void)o; (void)inst; (void)cb; (void)ctx; (void)f; UNSUP(11); }

static HRESULT __stdcall dp_EnumPlayers(dpobj* o, GUID* inst, void* cb, void* ctx, DWORD flags) {
    DPM(12);
    (void)inst;
    if (!g_role) return DPERR_NOCONNECTION;
    player_t copy[MAXP];
    lock();
    int n = g_npl;
    memcpy(copy, g_pl, n * sizeof copy[0]);
    unlock();
    for (int i = 0; i < n; i++) {
        uint8_t sb[NAMEN * 2], lb[NAMEN * 2];
        DPN dn = { sizeof dn, 0, NULL, NULL };
        if (copy[i].has_short) { name_out(sb, copy[i].shortname, o->ansi); dn.lpszShortName = sb; }
        if (copy[i].has_long) { name_out(lb, copy[i].longname, o->ansi); dn.lpszLongName = lb; }
        DWORD pf = copy[i].owner == -1 ? 0x8 /* DPENUMPLAYERS_LOCAL */ : 0x10 /* REMOTE */;
        if (!GUEST_CB5(ENUMPLAYERS_CB, cb, copy[i].id, DPPLAYERTYPE_PLAYER, &dn, pf, ctx)) break;
    }
    (void)flags;
    return DP_OK;
}

typedef BOOL (__stdcall *ENUMSESS_CB)(const SD2*, DWORD*, DWORD, void*);
static HRESULT __stdcall dp_EnumSessions(dpobj* o, SD2* want, DWORD timeout, void* cb, void* ctx, DWORD flags) {
    DPM(13);
    if (!want || !cb) return DPERR_INVALIDPARAMS;
    if (flags & DPENUMSESSIONS_STOPASYNC) { g_async = 0; return DP_OK; }
    if (!enum_socket()) return DPERR_GENERIC;
    if (flags & DPENUMSESSIONS_ASYNC) { g_async_app = want->guidApplication; g_async = 1; }
    static int said;
    if (!said++) LOG("EnumSessions (timeout %lu, flags %lX): probing port %d", timeout, flags, g_port);
    for (int round = 0; round < 20; round++) {
        DWORD t0 = GetTickCount();
        probe_all(&want->guidApplication);
        if (!(flags & DPENUMSESSIONS_ASYNC)) Sleep(timeout ? (timeout > 3000 ? 3000 : timeout) : 500);
        found_t list[16];
        int n = 0;
        lock();
        for (int i = 0; i < g_nfound; i++) {
            found_t* f = &g_found[i];
            if (flags & DPENUMSESSIONS_ASYNC ? GetTickCount() - f->seen > 4000 : (int)(f->seen - t0) < 0) continue;
            if (!(flags & DPENUMSESSIONS_ALL) &&
                ((f->sd.dwFlags & (DPSESSION_JOINDISABLED | DPSESSION_NEWPLAYERSDISABLED)) ||
                 (f->sd.dwMaxPlayers && f->sd.dwCurrentPlayers >= f->sd.dwMaxPlayers))) continue;
            list[n++] = *f;
        }
        unlock();
        for (int i = 0; i < n; i++) {
            uint8_t nb[NAMEN * 2];
            SD2 sd = list[i].sd;
            sd.dwSize = sizeof sd;
            name_out(nb, list[i].name, o->ansi);
            sd.lpszSessionName = nb;
            sd.lpszPassword = NULL;
            DWORD to = timeout;
            if (!GUEST_CB4(ENUMSESS_CB, cb, &sd, &to, 0, ctx)) return DP_OK;
        }
        DWORD to = timeout;
        if (!GUEST_CB4(ENUMSESS_CB, cb, NULL, &to, DPESC_TIMEDOUT, ctx) || (flags & DPENUMSESSIONS_ASYNC)) return DP_OK;
    }
    return DP_OK;
}

static void fill_caps(DPCAPS_* c, int local) {
    DWORD size = c->dwSize;
    memset(c, 0, sizeof *c);
    c->dwSize = size;
    c->dwFlags = DPCAPS_GUARANTEEDSUPPORTED | (g_role == 1 ? DPCAPS_ISHOST : 0) | (local ? DPPLAYERCAPS_LOCAL : 0);
    c->dwMaxBufferSize = 65479;   /* what dpwsockx (TCP/IP) says */
    c->dwMaxPlayers = MAXP;
    c->dwHundredBaud = 1000000;   /* 100 Mbit/s, in hundreds of bits */
    c->dwLatency = 20;
    c->dwMaxLocalPlayers = MAXP;
    c->dwHeaderLength = 20;
    c->dwTimeout = 5000;
}
static HRESULT __stdcall dp_GetCaps(dpobj* o, DPCAPS_* c, DWORD flags) {
    DPM(14);
    (void)o; (void)flags;
    if (!c || c->dwSize < sizeof *c) return DPERR_INVALIDPARAMS;
    fill_caps(c, 1);
    return DP_OK;
}
static HRESULT __stdcall dp_GetGroupData(dpobj* o, DPID g, void* d, DWORD* n, DWORD f) { (void)o; (void)g; (void)d; (void)n; (void)f; UNSUP(15); }
static HRESULT __stdcall dp_GetGroupName(dpobj* o, DPID g, void* d, DWORD* n) { (void)o; (void)g; (void)d; (void)n; UNSUP(16); }

static HRESULT __stdcall dp_GetMessageCount(dpobj* o, DPID id, DWORD* count) {
    DPM(17);
    (void)o;
    if (!count) return DPERR_INVALIDPARAMS;
    DWORD n = 0;
    lock();
    for (qmsg_t* m = g_qhead; m; m = m->next) if (!id || m->to == id) n++;
    unlock();
    *count = n;
    return DP_OK;
}
static HRESULT __stdcall dp_GetPlayerAddress(dpobj* o, DPID p, void* d, DWORD* n) { (void)o; (void)p; (void)d; (void)n; UNSUP(18); }
static HRESULT __stdcall dp_GetPlayerCaps(dpobj* o, DPID id, DPCAPS_* c, DWORD flags) {
    DPM(19);
    (void)o; (void)flags;
    if (!c || c->dwSize < sizeof *c) return DPERR_INVALIDPARAMS;
    lock();
    player_t* p = find_player(id);
    int local = p && p->owner == -1;
    unlock();
    if (!p) return DPERR_INVALIDPLAYER;
    fill_caps(c, local);
    return DP_OK;
}
static HRESULT __stdcall dp_GetPlayerData(dpobj* o, DPID p, void* d, DWORD* n, DWORD f) {
    DPM(20);
    (void)o; (void)p; (void)d; (void)f;
    if (n) *n = 0;   /* nothing is kept */
    return DP_OK;
}
static HRESULT __stdcall dp_GetPlayerName(dpobj* o, DPID id, void* data, DWORD* size) {
    DPM(21);
    if (!size) return DPERR_INVALIDPARAMS;
    lock();
    player_t* pp = find_player(id);
    if (!pp) { unlock(); return DPERR_INVALIDPLAYER; }
    player_t p = *pp;
    unlock();
    DWORD sb = p.has_short ? name_bytes(p.shortname, o->ansi) : 0, lb = p.has_long ? name_bytes(p.longname, o->ansi) : 0;
    DWORD need = sizeof(DPN) + sb + lb;
    if (!data || *size < need) { *size = need; return DPERR_BUFFERTOOSMALL; }
    DPN* dn = (DPN*)data;
    uint8_t* b = (uint8_t*)data + sizeof(DPN);
    dn->dwSize = sizeof(DPN);
    dn->dwFlags = 0;
    dn->lpszShortName = p.has_short ? b : NULL;
    if (p.has_short) { name_out(b, p.shortname, o->ansi); b += sb; }
    dn->lpszLongName = p.has_long ? b : NULL;
    if (p.has_long) name_out(b, p.longname, o->ansi);
    *size = need;
    return DP_OK;
}
static HRESULT __stdcall dp_GetSessionDesc(dpobj* o, void* data, DWORD* size) {
    DPM(22);
    if (!size) return DPERR_INVALIDPARAMS;
    if (!g_role) return DPERR_NOCONNECTION;
    lock();
    SD2 sd = g_sd;
    WCHAR name[NAMEN];
    lstrcpynW(name, g_sname, NAMEN);
    unlock();
    DWORD nb = name_bytes(name, o->ansi), need = sizeof(SD2) + nb;
    if (!data || *size < need) { *size = need; return DPERR_BUFFERTOOSMALL; }
    sd.dwSize = sizeof sd;
    sd.lpszSessionName = (uint8_t*)data + sizeof sd;
    sd.lpszPassword = NULL;
    memcpy(data, &sd, sizeof sd);
    name_out((uint8_t*)data + sizeof sd, name, o->ansi);
    *size = need;
    return DP_OK;
}
static HRESULT __stdcall dp_Initialize(dpobj* o, GUID* g) { (void)o; (void)g; DPM(23); return DPERR_ALREADYINITIALIZED; }

static HRESULT __stdcall dp_Open(dpobj* o, SD2* sd, DWORD flags) {
    DPM(24);
    if (!sd) return DPERR_INVALIDPARAMS;
    if (g_role) { LOG("Open while a session is open"); return DPERR_ALREADYINITIALIZED; }
    g_async = 0;
    if (flags & DPOPEN_CREATE) {
        SOCKET ls = INVALID_SOCKET, ds = INVALID_SOCKET;
        int port = 0;
        for (int k = 0; k < 4 && ls == INVALID_SOCKET; k++) {   /* a second host on this machine: the next port */
            struct sockaddr_in a = { AF_INET };
            a.sin_port = htons((u_short)(g_port + k));
            SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL one = TRUE;
#ifdef SO_EXCLUSIVEADDRUSE   /* POSIX: a bound port is taken without SO_REUSEADDR */
            setsockopt(t, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
            setsockopt(u, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#else
            (void)one;
#endif
            if (!bind(t, (struct sockaddr*)&a, sizeof a) && !listen(t, 8) && !bind(u, (struct sockaddr*)&a, sizeof a)) {
                ls = t, ds = u, port = g_port + k;
            } else { closesocket(t); closesocket(u); }
        }
        if (ls == INVALID_SOCKET) { LOG("Open(CREATE): ports %d-%d are all taken", g_port, g_port + 3); return DPERR_GENERIC; }
        lock();
        g_sd = *sd;
        g_sd.dwSize = sizeof g_sd;
        CoCreateGuid(&g_sd.guidInstance);
        g_sd.dwCurrentPlayers = 0;
        name_in(g_sname, NAMEN, sd->lpszSessionName, o->ansi);
        g_sd.lpszSessionName = g_sd.lpszPassword = NULL;
        g_listen = ls, g_disc = ds, g_tcp_port = port;
        g_npl = 0;
        g_next_id = 0x00010000u + (GetTickCount() & 0xFFF) * 16;
        g_role = 1;
        g_owner = o;
        unlock();
        net_start();
        LOG("Open(CREATE) \"%s\": hosting on TCP and UDP port %d (max %lu players, flags %lX, user %lX %lX %lX %lX)",
            u8(g_sname), port, sd->dwMaxPlayers, sd->dwFlags, sd->dwUser1, sd->dwUser2, sd->dwUser3, sd->dwUser4);
        return DP_OK;
    }
    if (flags & DPOPEN_JOIN) {
        found_t f;
        int i;
        lock();
        for (i = 0; i < g_nfound; i++) if (IsEqualGUID(&g_found[i].inst, &sd->guidInstance)) break;
        if (i < g_nfound) f = g_found[i];
        unlock();
        if (i == g_nfound) { LOG("Open(JOIN): no such session"); return DPERR_NOSESSIONS; }
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connect(s, (struct sockaddr*)&f.addr, sizeof f.addr)) {
            LOG("Open(JOIN): cannot connect to %s:%u (%d)", inet_ntoa(f.addr.sin_addr), ntohs(f.addr.sin_port), WSAGetLastError());
            closesocket(s);
            return DPERR_NOCONNECTION;
        }
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        /* a listener of its own, should this process become the host (host migration): any
         * free port, which the host passes to the others (T_PEERS) */
        SOCKET pl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in pa = { AF_INET };
        sock_len pal = sizeof pa;
        if (pl != INVALID_SOCKET && (bind(pl, (struct sockaddr*)&pa, sizeof pa) || listen(pl, 8) ||
                                     getsockname(pl, (struct sockaddr*)&pa, &pal))) { closesocket(pl); pl = INVALID_SOCKET; }
        lock();
        memset(&g_conn[0], 0, sizeof g_conn[0]);
        g_conn[0].s = s;
        g_conn[0].peer = f.addr;
        g_conn[0].last_rx = g_conn[0].last_tx = GetTickCount();
        g_nconn = 1;
        g_role = 2;
        g_owner = o;
        g_reply_type = 0;
        g_lost = 0;
        g_npeers = 0;
        if (g_plisten != INVALID_SOCKET) closesocket(g_plisten);
        g_plisten = pl;
        g_plisten_port = pl != INVALID_SOCKET ? ntohs(pa.sin_port) : 0;
        ResetEvent(g_reply_ev);
        wbuf w = { 0 };
        wb_begin(&w, T_HELLO); wb_put(&w, &f.inst, sizeof f.inst); wb_u32(&w, build_stamp()); wb_u32(&w, g_plisten_port); wb_end(&w);
        conn_send(&g_conn[0], &w);
        free(w.b);
        unlock();
        net_start();
        if (WaitForSingleObject(g_reply_ev, 10000) != WAIT_OBJECT_0 || g_reply_type != T_WELCOME || !g_reply_val) {
            LOG("Open(JOIN): the host did not let us in (full, started, or another build: its log says which)");
            dp_Close(o);
            return DPERR_NOSESSIONS;
        }
        LOG("Open(JOIN) \"%s\" at %s:%u: %lu players there", u8(g_sname), inet_ntoa(f.addr.sin_addr), ntohs(f.addr.sin_port), g_sd.dwCurrentPlayers);
        return DP_OK;
    }
    return DPERR_INVALIDFLAGS;
}

/* A chat line the game takes in, for the log (tools/mp_pair.py --chat checks every player got
 * every line). The game's own message: 'C', 2, ..., the sender's player number (a byte at 12; 13 is not set),
 * "Y"/"N" for each of the eight players it goes to (14-21), the text at 29 up to a 0. */
static void chat_seen(DPID from, DPID to, const uint8_t* d, DWORD n) {
    if (n < 31 || d[0] != 'C' || d[1] != 2) return;
    for (int i = 14; i < 22; i++) if (d[i] != 'Y' && d[i] != 'N') return;
    char text[128];
    int k = 0;
    for (DWORD i = 29; i < n && d[i] && k < (int)sizeof text - 5; i++)
        k += d[i] >= 0x20 && d[i] < 0x7F && d[i] != '"' && d[i] != '\\' ? (text[k] = (char)d[i], 1) : _snprintf(text + k, 5, "\\x%02X", d[i]);
    text[k] = 0;
    printf("[mp] chat from player %u (%08lX) to %08lX (%.8s): \"%s\"\n", d[12], from, to, (const char*)d + 14, text);
}

static HRESULT __stdcall dp_Receive(dpobj* o, DPID* from, DPID* to, DWORD flags, void* data, DWORD* size) {
    DPM(25);
    (void)o;
    if (!size) return DPERR_INVALIDPARAMS;
    lock();
    qmsg_t *m, *prev = NULL;
    for (m = g_qhead; m; prev = m, m = m->next) {
        if ((flags & DPRECEIVE_TOPLAYER) && to && m->to != *to) continue;
        if ((flags & DPRECEIVE_FROMPLAYER) && from && m->from != *from) continue;
        break;
    }
    if (!m) { unlock(); return DPERR_NOMESSAGES; }
    if (!data || *size < m->size) { *size = m->size; unlock(); return DPERR_BUFFERTOOSMALL; }
    memcpy(data, m->data, m->size);
    for (int k = 0; k < m->nreloc; k++) *(DWORD*)((uint8_t*)data + m->reloc[k]) += (DWORD)(uintptr_t)data;
    *size = m->size;
    if (from) *from = m->from;
    if (to) *to = m->to;
    DPID mfrom = m->from, mto = m->to;
    int taken = !(flags & DPRECEIVE_PEEK);
    if (taken) {
        if (prev) prev->next = m->next; else g_qhead = m->next;
        if (g_qtail == m) g_qtail = prev;
        g_qn--;
        free(m);
    }
    unlock();
    if (taken && mfrom) chat_seen(mfrom, mto, (const uint8_t*)data, *size);
    return DP_OK;
}

static HRESULT __stdcall dp_Send(dpobj* o, DPID from, DPID to, DWORD flags, void* data, DWORD size) {
    DPM(26);
    (void)o;
    if (!g_role) return DPERR_NOCONNECTION;
    if (size && !data) return DPERR_INVALIDPARAMS;
    lock();
    g_sent_msgs++, g_sent_bytes += size;
    if (g_role == 1) route_data(-1, from, to, flags, (const uint8_t*)data, size);
    else {
        if (to == 0)   /* other local players hear it too */
            for (int i = 0; i < g_npl; i++) if (g_pl[i].owner == -1 && g_pl[i].id != from) enqueue(from, g_pl[i].id, data, size);
        player_t* p = to ? find_player(to) : NULL;
        if (to && !p) VLOG("Send from %08lX to %08lX, who is not here (%lu bytes)", from, to, size);
        if (p && p->owner == -1) enqueue(from, to, data, size);
        else {
            wbuf w = { 0 };
            wb_begin(&w, T_DATA); wb_u32(&w, from); wb_u32(&w, to); wb_u32(&w, flags); wb_put(&w, data, size); wb_end(&w);
            int ok = conn_send(&g_conn[0], &w);
            if (!ok && !g_lost) { backlog_add(from, to, &w); ok = 1; }   /* the host is going: for the next one */
            free(w.b);
            if (!ok) { unlock(); return DPERR_NOCONNECTION; }
        }
    }
    unlock();
    return DP_OK;
}

static HRESULT __stdcall dp_SetGroupData(dpobj* o, DPID g, void* d, DWORD n, DWORD f) { (void)o; (void)g; (void)d; (void)n; (void)f; UNSUP(27); }
static HRESULT __stdcall dp_SetGroupName(dpobj* o, DPID g, DPN* n, DWORD f) { (void)o; (void)g; (void)n; (void)f; UNSUP(28); }
static HRESULT __stdcall dp_SetPlayerData(dpobj* o, DPID p, void* d, DWORD n, DWORD f) { (void)o; (void)p; (void)d; (void)n; (void)f; UNSUP(29); }
static HRESULT __stdcall dp_SetPlayerName(dpobj* o, DPID p, DPN* n, DWORD f) { (void)o; (void)p; (void)n; (void)f; UNSUP(30); }

static HRESULT __stdcall dp_SetSessionDesc(dpobj* o, SD2* sd, DWORD flags) {
    DPM(31);
    (void)flags;
    if (!sd) return DPERR_INVALIDPARAMS;
    if (g_role != 1) return DPERR_UNAVAILABLE;   /* only the host */
    lock();
    GUID inst = g_sd.guidInstance;
    DWORD cur = g_sd.dwCurrentPlayers;
    g_sd = *sd;
    g_sd.dwSize = sizeof g_sd;
    g_sd.guidInstance = inst;
    g_sd.dwCurrentPlayers = cur;
    name_in(g_sname, NAMEN, sd->lpszSessionName, o->ansi);
    g_sd.lpszSessionName = g_sd.lpszPassword = NULL;
    wbuf w = { 0 };
    wb_begin(&w, T_SDESC); wb_sd(&w); wb_end(&w);
    send_all(&w, -1);
    free(w.b);
    unlock();
    LOG("SetSessionDesc: flags %lX, max %lu, user %lX %lX %lX %lX", sd->dwFlags, sd->dwMaxPlayers, sd->dwUser1, sd->dwUser2, sd->dwUser3, sd->dwUser4);
    return DP_OK;
}

static HRESULT __stdcall dp_AddGroupToGroup(dpobj* o, DPID a, DPID b) { (void)o; (void)a; (void)b; UNSUP(32); }
static HRESULT __stdcall dp_CreateGroupInGroup(dpobj* o, DPID p, DPID* id, DPN* n, void* d, DWORD ds, DWORD f) { (void)o; (void)p; (void)id; (void)n; (void)d; (void)ds; (void)f; UNSUP(33); }
static HRESULT __stdcall dp_DeleteGroupFromGroup(dpobj* o, DPID a, DPID b) { (void)o; (void)a; (void)b; UNSUP(34); }
typedef BOOL (__stdcall *ENUMCONN_CB)(const GUID*, void*, DWORD, const DPN*, DWORD, void*);
static HRESULT __stdcall dp_EnumConnections(dpobj* o, const GUID* app, void* cb, void* ctx, DWORD flags) {
    DPM(35);
    (void)app; (void)flags;
    /* the TCP/IP connection, its address an empty INet string */
    struct { GUID g1; DWORD n1; DWORD total; GUID g2; DWORD n2; GUID sp; GUID g3; DWORD n3; char s[1]; } a;
    a.g1 = AID_TOTAL, a.n1 = 4, a.total = sizeof a, a.g2 = AID_SP, a.n2 = 16, a.sp = SP_TCPIP, a.g3 = AID_INET, a.n3 = 1, a.s[0] = 0;
    WCHAR wn[] = WSTR("Internet TCP/IP Connection For DirectPlay");
    char an[] = "Internet TCP/IP Connection For DirectPlay";
    DPN dn = { sizeof dn, 0, o->ansi ? (void*)an : (void*)wn, NULL };
    GUEST_CB6(ENUMCONN_CB, cb, &SP_TCPIP, &a, sizeof a, &dn, 0, ctx);
    return DP_OK;
}
static HRESULT __stdcall dp_EnumGroupsInGroup(dpobj* o, DPID g, GUID* i, void* cb, void* ctx, DWORD f) { (void)o; (void)g; (void)i; (void)cb; (void)ctx; (void)f; UNSUP(36); }
static HRESULT __stdcall dp_GetGroupConnectionSettings(dpobj* o, DWORD f, DPID g, void* d, DWORD* n) { (void)o; (void)f; (void)g; (void)d; (void)n; UNSUP(37); }

/* the address: DPADDRESS chunks ({GUID type, DWORD size} + data); the INet one names the host */
static HRESULT __stdcall dp_InitializeConnection(dpobj* o, void* conn, DWORD flags) {
    DPM(38);
    (void)o; (void)flags;
    if (!conn) return DPERR_INVALIDPARAMS;
    uint8_t* p = (uint8_t*)conn;
    DWORD total = 0;
    if (IsEqualGUID((GUID*)p, &AID_TOTAL)) total = *(DWORD*)(p + 20);
    g_target[0] = 0;
    for (DWORD off = 0; total && off + 20 <= total;) {
        GUID* g = (GUID*)(p + off);
        DWORD n = *(DWORD*)(p + off + 16);
        const uint8_t* d = p + off + 20;
        if (IsEqualGUID(g, &AID_INET)) lstrcpynA(g_target, (const char*)d, sizeof g_target);
        else if (IsEqualGUID(g, &AID_INETW)) WideCharToMultiByte(CP_ACP, 0, (const WCHAR*)d, -1, g_target, sizeof g_target, NULL, NULL);
        else if (IsEqualGUID(g, &AID_SP) && !IsEqualGUID((const GUID*)d, &SP_TCPIP)) return DPERR_UNAVAILABLE;
        off += 20 + n;
    }
    LOG("InitializeConnection: TCP/IP, address \"%s\"", g_target);
    return DP_OK;
}
static HRESULT __stdcall dp_SecureOpen(dpobj* o, SD2* sd, DWORD f, void* sec, void* cred) { (void)sec; (void)cred; DPM(39); return dp_Open(o, sd, f); }
static HRESULT __stdcall dp_SendChatMessage(dpobj* o, DPID a, DPID b, DWORD f, void* c) { (void)o; (void)a; (void)b; (void)f; (void)c; UNSUP(40); }
static HRESULT __stdcall dp_SetGroupConnectionSettings(dpobj* o, DWORD f, DPID g, void* c) { (void)o; (void)f; (void)g; (void)c; UNSUP(41); }
static HRESULT __stdcall dp_StartSession(dpobj* o, DWORD f, DPID g) { (void)o; (void)f; (void)g; UNSUP(42); }
static HRESULT __stdcall dp_GetGroupFlags(dpobj* o, DPID g, DWORD* f) { (void)o; (void)g; (void)f; UNSUP(43); }
static HRESULT __stdcall dp_GetGroupParent(dpobj* o, DPID g, DPID* p) { (void)o; (void)g; (void)p; UNSUP(44); }
static HRESULT __stdcall dp_GetPlayerAccount(dpobj* o, DPID p, DWORD f, void* d, DWORD* n) { (void)o; (void)p; (void)f; (void)d; (void)n; UNSUP(45); }
static HRESULT __stdcall dp_GetPlayerFlags(dpobj* o, DPID id, DWORD* f) {
    DPM(46);
    (void)o;
    if (!f) return DPERR_INVALIDPARAMS;
    lock();
    player_t* p = find_player(id);
    if (p) *f = p->owner == -1 ? 0x8 : 0;   /* DPPLAYER_LOCAL */
    unlock();
    return p ? DP_OK : DPERR_INVALIDPLAYER;
}
static HRESULT __stdcall dp_GetGroupOwner(dpobj* o, DPID g, DPID* p) { (void)o; (void)g; (void)p; UNSUP(47); }
static HRESULT __stdcall dp_SetGroupOwner(dpobj* o, DPID g, DPID p) { (void)o; (void)g; (void)p; UNSUP(48); }
static HRESULT __stdcall dp_SendEx(dpobj* o, DPID from, DPID to, DWORD flags, void* data, DWORD size, DWORD prio, DWORD timeout, void* ctx, DWORD* msgid) {
    DPM(49);
    (void)prio; (void)timeout; (void)ctx;
    if (msgid) *msgid = 0;
    return dp_Send(o, from, to, flags & ~0x200u /* DPSEND_ASYNC: done at once */, data, size);
}
static HRESULT __stdcall dp_GetMessageQueue(dpobj* o, DPID from, DPID to, DWORD flags, DWORD* nmsg, DWORD* nbytes) {
    DPM(50);
    (void)o; (void)from; (void)to; (void)flags;
    if (nmsg) *nmsg = 0;   /* nothing waits to be sent: sends are done at once */
    if (nbytes) *nbytes = 0;
    return DP_OK;
}
static HRESULT __stdcall dp_CancelMessage(dpobj* o, DWORD id, DWORD f) { (void)o; (void)id; (void)f; DPM(51); return DP_OK; }
static HRESULT __stdcall dp_CancelPriority(dpobj* o, DWORD a, DWORD b, DWORD f) { (void)o; (void)a; (void)b; (void)f; DPM(52); return DP_OK; }

/* ---- IDirectPlay, the first one (DirectX 3): DirectPlayCreate's answer, and what the game asks
 * for (at 13 places) to call EnableNewPlayers: off when a game starts, on in the setup screen.
 * Its own layout, over the same object. */
typedef struct { void** vtbl; LONG refs; dpobj* dp; } dp1obj;
static void* g_dp1_vtbl[25];
static const char* const k_dp1_names[25] = {
    "QueryInterface", "AddRef", "Release", "AddPlayerToGroup", "Close", "CreatePlayer", "CreateGroup",
    "DeletePlayerFromGroup", "DestroyPlayer", "DestroyGroup", "EnableNewPlayers", "EnumGroupPlayers",
    "EnumGroups", "EnumPlayers", "EnumSessions", "GetCaps", "GetMessageCount", "GetPlayerCaps", "GetPlayerName",
    "Initialize", "Open", "Receive", "SaveSession", "Send", "SetPlayerName" };
static volatile LONG g_dp1_calls[25];
#define DP1M(n) seen(&g_dp1_calls[n], "IDirectPlay", k_dp1_names[n])
#define UNSUP1(n) do { DP1M(n); LOG("IDirectPlay::%s is not implemented", k_dp1_names[n]); return DPERR_UNSUPPORTED; } while (0)

static HRESULT __stdcall dp1_QueryInterface(dp1obj* o, const GUID* r, void** out) { DP1M(0); return dp_QueryInterface(o->dp, r, out); }
static ULONG __stdcall dp1_AddRef(dp1obj* o) { DP1M(1); return (ULONG)InterlockedIncrement(&o->refs); }
static ULONG __stdcall dp1_Release(dp1obj* o) {
    DP1M(2);
    LONG r = InterlockedDecrement(&o->refs);
    if (!r) { dpobj* d = o->dp; free(o); dp_Release(d); }
    return (ULONG)r;
}
static HRESULT __stdcall dp1_AddPlayerToGroup(dp1obj* o, DPID g, DPID p) { (void)o; (void)g; (void)p; UNSUP1(3); }
static HRESULT __stdcall dp1_Close(dp1obj* o) { DP1M(4); return dp_Close(o->dp); }
static HRESULT __stdcall dp1_CreatePlayer(dp1obj* o, DPID* id, char* s, char* l, HANDLE* e) { (void)o; (void)id; (void)s; (void)l; (void)e; UNSUP1(5); }
static HRESULT __stdcall dp1_CreateGroup(dp1obj* o, DPID* id, char* s, char* l) { (void)o; (void)id; (void)s; (void)l; UNSUP1(6); }
static HRESULT __stdcall dp1_DeletePlayerFromGroup(dp1obj* o, DPID g, DPID p) { (void)o; (void)g; (void)p; UNSUP1(7); }
static HRESULT __stdcall dp1_DestroyPlayer(dp1obj* o, DPID p) { DP1M(8); return dp_DestroyPlayer(o->dp, p); }
static HRESULT __stdcall dp1_DestroyGroup(dp1obj* o, DPID g) { (void)o; (void)g; UNSUP1(9); }
/* The host's session takes no new players while off (DPSESSION_NEWPLAYERSDISABLED); a client's
 * call changes nothing, as with dplayx. */
static HRESULT __stdcall dp1_EnableNewPlayers(dp1obj* o, BOOL on) {
    DP1M(10);
    (void)o;
    if (g_role != 1) return DP_OK;
    lock();
    DWORD was = g_sd.dwFlags;
    if (on) g_sd.dwFlags &= ~DPSESSION_NEWPLAYERSDISABLED; else g_sd.dwFlags |= DPSESSION_NEWPLAYERSDISABLED;
    if (g_sd.dwFlags != was) {
        wbuf w = { 0 };
        wb_begin(&w, T_SDESC); wb_sd(&w); wb_end(&w);
        send_all(&w, -1);
        free(w.b);
        LOG("EnableNewPlayers(%s)", on ? "TRUE" : "FALSE");
    }
    unlock();
    return DP_OK;
}
static HRESULT __stdcall dp1_EnumGroupPlayers(dp1obj* o, DPID g, void* cb, void* c, DWORD f) { (void)o; (void)g; (void)cb; (void)c; (void)f; UNSUP1(11); }
static HRESULT __stdcall dp1_EnumGroups(dp1obj* o, DWORD s, void* cb, void* c, DWORD f) { (void)o; (void)s; (void)cb; (void)c; (void)f; UNSUP1(12); }
static HRESULT __stdcall dp1_EnumPlayers(dp1obj* o, DWORD s, void* cb, void* c, DWORD f) { (void)o; (void)s; (void)cb; (void)c; (void)f; UNSUP1(13); }
static HRESULT __stdcall dp1_EnumSessions(dp1obj* o, void* sd, DWORD t, void* cb, void* c, DWORD f) { (void)o; (void)sd; (void)t; (void)cb; (void)c; (void)f; UNSUP1(14); }
static HRESULT __stdcall dp1_GetCaps(dp1obj* o, DPCAPS_* c) { DP1M(15); return dp_GetCaps(o->dp, c, 0); }
static HRESULT __stdcall dp1_GetMessageCount(dp1obj* o, DPID p, DWORD* n) { DP1M(16); return dp_GetMessageCount(o->dp, p, n); }
static HRESULT __stdcall dp1_GetPlayerCaps(dp1obj* o, DPID p, DPCAPS_* c) { DP1M(17); return dp_GetPlayerCaps(o->dp, p, c, 0); }
static HRESULT __stdcall dp1_GetPlayerName(dp1obj* o, DPID p, char* s, DWORD* sn, char* l, DWORD* ln) { (void)o; (void)p; (void)s; (void)sn; (void)l; (void)ln; UNSUP1(18); }
static HRESULT __stdcall dp1_Initialize(dp1obj* o, GUID* g) { (void)o; (void)g; DP1M(19); return DPERR_ALREADYINITIALIZED; }
static HRESULT __stdcall dp1_Open(dp1obj* o, void* sd) { (void)o; (void)sd; UNSUP1(20); }
static HRESULT __stdcall dp1_Receive(dp1obj* o, DPID* f, DPID* t, DWORD fl, void* d, DWORD* n) { DP1M(21); return dp_Receive(o->dp, f, t, fl, d, n); }
static HRESULT __stdcall dp1_SaveSession(dp1obj* o, char* s) { (void)o; (void)s; UNSUP1(22); }
static HRESULT __stdcall dp1_Send(dp1obj* o, DPID f, DPID t, DWORD fl, void* d, DWORD n) { DP1M(23); return dp_Send(o->dp, f, t, fl, d, n); }
static HRESULT __stdcall dp1_SetPlayerName(dp1obj* o, DPID p, char* s, char* l) { (void)o; (void)p; (void)s; (void)l; UNSUP1(24); }

static void* dp1_new(dpobj* dp) {
    dp1obj* o = (dp1obj*)calloc(1, sizeof *o);
    o->vtbl = g_dp1_vtbl;
    o->refs = 1;
    o->dp = dp;
    return o;
}

/* ---- the lobby: only CreateAddress matters (the game is never lobbied) -------------- */
typedef struct { void** vtbl; LONG refs; } lobobj;
static HRESULT __stdcall lob_QueryInterface(lobobj* o, const GUID* riid, void** out) {
    LOBM(0);
    if (!out) return DPERR_INVALIDPARAMS;
    InterlockedIncrement(&o->refs);   /* IDirectPlayLobby, 2, 3, A or W: one layout */
    (void)riid;
    *out = o;
    return DP_OK;
}
static ULONG __stdcall lob_AddRef(lobobj* o) { LOBM(1); return (ULONG)InterlockedIncrement(&o->refs); }
static ULONG __stdcall lob_Release(lobobj* o) { LOBM(2); LONG r = InterlockedDecrement(&o->refs); if (!r) free(o); return (ULONG)r; }
#define LUNSUP(n) do { LOBM(n); LOG("IDirectPlayLobby::%s is not implemented", k_lob_names[n]); return DPERR_UNSUPPORTED; } while (0)
static HRESULT __stdcall lob_Connect(lobobj* o, DWORD f, void** dp, void* u) { (void)o; (void)f; (void)dp; (void)u; LUNSUP(3); }
static HRESULT __stdcall lob_CreateAddress(lobobj* o, const GUID* sp, const GUID* type, const void* data, DWORD n, void* out, DWORD* outn) {
    LOBM(4);
    (void)o;
    if (!sp || !type || !outn) return DPERR_INVALIDPARAMS;
    DWORD need = 20 + 4 + 20 + 16 + 20 + n;
    if (!out || *outn < need) { *outn = need; return DPERR_BUFFERTOOSMALL; }
    uint8_t* p = (uint8_t*)out;
    memcpy(p, &AID_TOTAL, 16); *(DWORD*)(p + 16) = 4; *(DWORD*)(p + 20) = need; p += 24;
    memcpy(p, &AID_SP, 16); *(DWORD*)(p + 16) = 16; memcpy(p + 20, sp, 16); p += 36;
    memcpy(p, type, 16); *(DWORD*)(p + 16) = n; if (n) memcpy(p + 20, data, n);
    *outn = need;
    return DP_OK;
}
static HRESULT __stdcall lob_EnumAddress(lobobj* o, void* cb, const void* a, DWORD n, void* ctx) { (void)o; (void)cb; (void)a; (void)n; (void)ctx; LUNSUP(5); }
static HRESULT __stdcall lob_EnumAddressTypes(lobobj* o, void* cb, const GUID* sp, void* ctx, DWORD f) { (void)o; (void)cb; (void)sp; (void)ctx; (void)f; LUNSUP(6); }
static HRESULT __stdcall lob_EnumLocalApplications(lobobj* o, void* cb, void* ctx, DWORD f) { (void)o; (void)cb; (void)ctx; (void)f; LUNSUP(7); }
static HRESULT __stdcall lob_GetConnectionSettings(lobobj* o, DWORD id, void* d, DWORD* n) { (void)o; (void)id; (void)d; (void)n; LOBM(8); return DPERR_NOTLOBBIED; }
static HRESULT __stdcall lob_ReceiveLobbyMessage(lobobj* o, DWORD f, DWORD id, DWORD* mf, void* d, DWORD* n) { (void)o; (void)f; (void)id; (void)mf; (void)d; (void)n; LOBM(9); return DPERR_NOMESSAGES; }
static HRESULT __stdcall lob_RunApplication(lobobj* o, DWORD f, DWORD* id, void* c, HANDLE e) { (void)o; (void)f; (void)id; (void)c; (void)e; LUNSUP(10); }
static HRESULT __stdcall lob_SendLobbyMessage(lobobj* o, DWORD f, DWORD id, void* d, DWORD n) { (void)o; (void)f; (void)id; (void)d; (void)n; LUNSUP(11); }
static HRESULT __stdcall lob_SetConnectionSettings(lobobj* o, DWORD f, DWORD id, void* c) { (void)o; (void)f; (void)id; (void)c; LUNSUP(12); }
static HRESULT __stdcall lob_SetLobbyMessageEvent(lobobj* o, DWORD f, DWORD id, HANDLE e) { (void)o; (void)f; (void)id; (void)e; LUNSUP(13); }
static HRESULT __stdcall lob_CreateCompoundAddress(lobobj* o, const void* el, DWORD n, void* out, DWORD* outn) { (void)o; (void)el; (void)n; (void)out; (void)outn; LUNSUP(14); }
static HRESULT __stdcall lob_ConnectEx(lobobj* o, DWORD f, const GUID* r, void** dp, void* u) { (void)o; (void)f; (void)r; (void)dp; (void)u; LUNSUP(15); }
static HRESULT __stdcall lob_RegisterApplication(lobobj* o, DWORD f, void* d) { (void)o; (void)f; (void)d; LUNSUP(16); }
static HRESULT __stdcall lob_UnregisterApplication(lobobj* o, DWORD f, const GUID* g) { (void)o; (void)f; (void)g; LUNSUP(17); }
static HRESULT __stdcall lob_WaitForConnectionSettings(lobobj* o, DWORD f) { (void)o; (void)f; LUNSUP(18); }

static lobobj* lob_new(void) {
    lobobj* o = (lobobj*)calloc(1, sizeof *o);
    o->vtbl = g_lob_vtbl;
    o->refs = 1;
    return o;
}

/* ---- the exports the game imports -------------------------------------------------------- */
static HRESULT WINAPI my_DirectPlayCreate(GUID* sp, void** out, void* unk) {
    (void)unk;
    if (!out) return DPERR_INVALIDPARAMS;
    *out = NULL;
    static const GUID none = { 0 };   /* GUID_NULL: an object for no SP yet (the game's version check) */
    if (sp && !IsEqualGUID(sp, &SP_TCPIP) && !IsEqualGUID(sp, &none)) { VLOG("DirectPlayCreate {%08lX-...}: only TCP/IP is here", sp->Data1); return DPERR_UNAVAILABLE; }
    *out = dp1_new(dp_new(0));   /* an IDirectPlay, as dplayx's; the game asks it for IDirectPlay4 */
    return DP_OK;
}
typedef BOOL (__stdcall *ENUMSP_CB)(GUID*, char*, DWORD, DWORD, void*);
static HRESULT WINAPI my_DirectPlayEnumerateA(void* cb, void* ctx) {
    /* The game keeps the GUID's pointer (in its connection list, for DirectPlayCreate at Create), as
     * dplayx's own lives as long as the DLL: it must outlive the call. */
    static GUID g;
    static char name[] = "Internet TCP/IP Connection For DirectPlay";
    if (!cb) return DPERR_INVALIDPARAMS;
    g = SP_TCPIP;
    GUEST_CB5(ENUMSP_CB, cb, &g, name, 6, 0, ctx);
    return DP_OK;
}
static HRESULT WINAPI my_DirectPlayLobbyCreateA(GUID* sp, void** out, void* unk, void* data, DWORD n) {
    (void)sp; (void)unk; (void)data; (void)n;
    if (!out) return DPERR_INVALIDPARAMS;
    *out = lob_new();
    return DP_OK;
}
static HRESULT WINAPI my_CoCreateInstance(const GUID* clsid, void* outer, DWORD ctx, const GUID* riid, void** out) {
    if (clsid && IsEqualGUID(clsid, &CLSID_DP)) {
        int ansi = 0;
        if (!out) return DPERR_INVALIDPARAMS;
        if (riid && riid->Data1 == 0x5454E9A0) { *out = dp1_new(dp_new(0)); return DP_OK; }
        if (!dp_iid(riid, &ansi)) { *out = NULL; return E_NOINTERFACE_; }
        *out = dp_new(ansi);
        return DP_OK;
    }
    if (clsid && IsEqualGUID(clsid, &CLSID_DPL)) { if (!out) return DPERR_INVALIDPARAMS; *out = lob_new(); return DP_OK; }
#ifdef SWGB_WIN32HLE
    (void)outer; (void)ctx;
    return 0x80040154;   /* REGDB_E_CLASSNOTREG: only ours come here */
#else
    return CoCreateInstance(clsid, (LPUNKNOWN)outer, ctx, riid, out);
#endif
}

#ifdef SWGB_WIN32HLE
/* ---- the win32hle host: each method a shim ---------------------------------------------
 * The guest's call reaches a shim holding the machine, with g_esp at [ret][this][args...]. The
 * bridge reads the arguments, lets the machine go (a method may wait: Open, CreatePlayer,
 * EnumSessions; the network thread takes the machine to set a player's event), calls the
 * method, takes the machine back and pops the arguments, as the stdcall method would.
 * X(slot, function, arguments counting this) */
#define DP_METHODS(X) \
    X(0, dp_QueryInterface, 3) \
    X(1, dp_AddRef, 1) \
    X(2, dp_Release, 1) \
    X(3, dp_AddPlayerToGroup, 3) \
    X(4, dp_Close, 1) \
    X(5, dp_CreateGroup, 6) \
    X(6, dp_CreatePlayer, 7) \
    X(7, dp_DeletePlayerFromGroup, 3) \
    X(8, dp_DestroyGroup, 2) \
    X(9, dp_DestroyPlayer, 2) \
    X(10, dp_EnumGroupPlayers, 6) \
    X(11, dp_EnumGroups, 5) \
    X(12, dp_EnumPlayers, 5) \
    X(13, dp_EnumSessions, 6) \
    X(14, dp_GetCaps, 3) \
    X(15, dp_GetGroupData, 5) \
    X(16, dp_GetGroupName, 4) \
    X(17, dp_GetMessageCount, 3) \
    X(18, dp_GetPlayerAddress, 4) \
    X(19, dp_GetPlayerCaps, 4) \
    X(20, dp_GetPlayerData, 5) \
    X(21, dp_GetPlayerName, 4) \
    X(22, dp_GetSessionDesc, 3) \
    X(23, dp_Initialize, 2) \
    X(24, dp_Open, 3) \
    X(25, dp_Receive, 6) \
    X(26, dp_Send, 6) \
    X(27, dp_SetGroupData, 5) \
    X(28, dp_SetGroupName, 4) \
    X(29, dp_SetPlayerData, 5) \
    X(30, dp_SetPlayerName, 4) \
    X(31, dp_SetSessionDesc, 3) \
    X(32, dp_AddGroupToGroup, 3) \
    X(33, dp_CreateGroupInGroup, 7) \
    X(34, dp_DeleteGroupFromGroup, 3) \
    X(35, dp_EnumConnections, 5) \
    X(36, dp_EnumGroupsInGroup, 6) \
    X(37, dp_GetGroupConnectionSettings, 5) \
    X(38, dp_InitializeConnection, 3) \
    X(39, dp_SecureOpen, 5) \
    X(40, dp_SendChatMessage, 5) \
    X(41, dp_SetGroupConnectionSettings, 4) \
    X(42, dp_StartSession, 3) \
    X(43, dp_GetGroupFlags, 3) \
    X(44, dp_GetGroupParent, 3) \
    X(45, dp_GetPlayerAccount, 5) \
    X(46, dp_GetPlayerFlags, 3) \
    X(47, dp_GetGroupOwner, 3) \
    X(48, dp_SetGroupOwner, 3) \
    X(49, dp_SendEx, 10) \
    X(50, dp_GetMessageQueue, 6) \
    X(51, dp_CancelMessage, 3) \
    X(52, dp_CancelPriority, 4) \

#define DP1_METHODS(X) \
    X(0, dp1_QueryInterface, 3) \
    X(1, dp1_AddRef, 1) \
    X(2, dp1_Release, 1) \
    X(3, dp1_AddPlayerToGroup, 3) \
    X(4, dp1_Close, 1) \
    X(5, dp1_CreatePlayer, 5) \
    X(6, dp1_CreateGroup, 4) \
    X(7, dp1_DeletePlayerFromGroup, 3) \
    X(8, dp1_DestroyPlayer, 2) \
    X(9, dp1_DestroyGroup, 2) \
    X(10, dp1_EnableNewPlayers, 2) \
    X(11, dp1_EnumGroupPlayers, 5) \
    X(12, dp1_EnumGroups, 5) \
    X(13, dp1_EnumPlayers, 5) \
    X(14, dp1_EnumSessions, 6) \
    X(15, dp1_GetCaps, 2) \
    X(16, dp1_GetMessageCount, 3) \
    X(17, dp1_GetPlayerCaps, 3) \
    X(18, dp1_GetPlayerName, 6) \
    X(19, dp1_Initialize, 2) \
    X(20, dp1_Open, 2) \
    X(21, dp1_Receive, 6) \
    X(22, dp1_SaveSession, 2) \
    X(23, dp1_Send, 6) \
    X(24, dp1_SetPlayerName, 4) \

#define LOB_METHODS(X) \
    X(0, lob_QueryInterface, 3) \
    X(1, lob_AddRef, 1) \
    X(2, lob_Release, 1) \
    X(3, lob_Connect, 4) \
    X(4, lob_CreateAddress, 7) \
    X(5, lob_EnumAddress, 5) \
    X(6, lob_EnumAddressTypes, 5) \
    X(7, lob_EnumLocalApplications, 4) \
    X(8, lob_GetConnectionSettings, 4) \
    X(9, lob_ReceiveLobbyMessage, 6) \
    X(10, lob_RunApplication, 5) \
    X(11, lob_SendLobbyMessage, 5) \
    X(12, lob_SetConnectionSettings, 4) \
    X(13, lob_SetLobbyMessageEvent, 4) \
    X(14, lob_CreateCompoundAddress, 5) \
    X(15, lob_ConnectEx, 5) \
    X(16, lob_RegisterApplication, 3) \
    X(17, lob_UnregisterApplication, 3) \
    X(18, lob_WaitForConnectionSettings, 2) \

typedef uint32_t (*fn1)(uint32_t);
typedef uint32_t (*fn2)(uint32_t, uint32_t);
typedef uint32_t (*fn3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn6)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn7)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*fn10)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t bridge(void* fn, int n) {
    uint32_t a[10] = { 0 }, r = DPERR_GENERIC;
    for (int i = 0; i < n; i++) a[i] = A32(i);
    hle_block_begin();
    switch (n) {   /* every argument is a dword (32-bit host): i386 cdecl takes them as declared */
    case 1: r = ((fn1)fn)(a[0]); break;
    case 2: r = ((fn2)fn)(a[0], a[1]); break;
    case 3: r = ((fn3)fn)(a[0], a[1], a[2]); break;
    case 4: r = ((fn4)fn)(a[0], a[1], a[2], a[3]); break;
    case 5: r = ((fn5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: r = ((fn6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 7: r = ((fn7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
    case 10: r = ((fn10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    default: fprintf(stderr, "[dplay] no bridge for %d arguments\n", n); break;
    }
    hle_block_end();
    return r;
}
#define BRIDGE(slot, fn, n) static void br_##fn(void) { uint32_t r_ = bridge((void*)fn, n); RET(r_, n); }
DP_METHODS(BRIDGE)
DP1_METHODS(BRIDGE)
LOB_METHODS(BRIDGE)
#define SHIM_DP(slot, fn, n) { "IDirectPlay4::" #fn, br_##fn },
#define SHIM_DP1(slot, fn, n) { "IDirectPlay::" #fn, br_##fn },
#define SHIM_LOB(slot, fn, n) { "IDirectPlayLobby::" #fn, br_##fn },
#define VT(slot, fn, n) vt[slot] = (void*)(uintptr_t)hle_resolve(names[slot].name);

/* The exports: dplayx's ordinals, as the game imports them. */
static void s_DirectPlayCreate(void) { uint32_t r = bridge((void*)my_DirectPlayCreate, 3); RET(r, 3); }
static void s_DirectPlayEnumerateA(void) { uint32_t r = bridge((void*)my_DirectPlayEnumerateA, 2); RET(r, 2); }
static void s_DirectPlayLobbyCreateA(void) { uint32_t r = bridge((void*)my_DirectPlayLobbyCreateA, 5); RET(r, 5); }

/* CoCreateInstance(CLSID_DirectPlay or CLSID_DirectPlayLobby): ole32.c asks its classes. */
static uint32_t create_dp(const uint8_t* iid, uint32_t* out) {
    void* o = NULL;
    HRESULT r = my_CoCreateInstance(&CLSID_DP, NULL, 0, (const GUID*)iid, &o);
    *out = (uint32_t)(uintptr_t)o;
    return (uint32_t)r;
}
static uint32_t create_dpl(const uint8_t* iid, uint32_t* out) {
    void* o = NULL;
    HRESULT r = my_CoCreateInstance(&CLSID_DPL, NULL, 0, (const GUID*)iid, &o);
    *out = (uint32_t)(uintptr_t)o;
    return (uint32_t)r;
}
#endif

void dplay_init(void) {
    if (g_native) { LOG("Windows' DirectPlay (--dplay native)"); return; }
#ifdef SWGB_WIN32HLE
    static const win32hle_shim dp_names[] = { DP_METHODS(SHIM_DP) { 0, 0 } };
    static const win32hle_shim dp1_names[] = { DP1_METHODS(SHIM_DP1) { 0, 0 } };
    static const win32hle_shim lob_names[] = { LOB_METHODS(SHIM_LOB) { 0, 0 } };
    static const win32hle_shim exports[] = {
        { "dplayx.dll#1", s_DirectPlayCreate },          /* DirectPlayCreate */
        { "dplayx.dll#2", s_DirectPlayEnumerateA },      /* DirectPlayEnumerateA */
        { "dplayx.dll#4", s_DirectPlayLobbyCreateA },    /* DirectPlayLobbyCreateA */
        { "DirectPlayCreate", s_DirectPlayCreate },
        { "DirectPlayEnumerateA", s_DirectPlayEnumerateA },
        { "DirectPlayLobbyCreateA", s_DirectPlayLobbyCreateA },
        { 0, 0 } };
    win32hle_register(dp_names);
    win32hle_register(dp1_names);
    win32hle_register(lob_names);
    win32hle_register(exports);
    { void** vt = g_dp_vtbl; const win32hle_shim* names = dp_names; DP_METHODS(VT) }
    { void** vt = g_dp1_vtbl; const win32hle_shim* names = dp1_names; DP1_METHODS(VT) }
    { void** vt = g_lob_vtbl; const win32hle_shim* names = lob_names; LOB_METHODS(VT) }
    hle_com_register_class((const uint8_t*)&CLSID_DP, create_dp);
    hle_com_register_class((const uint8_t*)&CLSID_DPL, create_dpl);
    if (!g_inited) {
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_send_cs);
        g_reply_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_inited = 1;
    }
    LOG("the host's DirectPlay: TCP/IP on port %d%s%s%s", g_port, g_join[0] ? ", also probing " : "", g_join,
        g_cross ? "; --mp-cross: the lift's stamp" : "");
#else
    void* dp[53] = {
        dp_QueryInterface, dp_AddRef, dp_Release, dp_AddPlayerToGroup, dp_Close, dp_CreateGroup, dp_CreatePlayer,
        dp_DeletePlayerFromGroup, dp_DestroyGroup, dp_DestroyPlayer, dp_EnumGroupPlayers, dp_EnumGroups,
        dp_EnumPlayers, dp_EnumSessions, dp_GetCaps, dp_GetGroupData, dp_GetGroupName, dp_GetMessageCount,
        dp_GetPlayerAddress, dp_GetPlayerCaps, dp_GetPlayerData, dp_GetPlayerName, dp_GetSessionDesc,
        dp_Initialize, dp_Open, dp_Receive, dp_Send, dp_SetGroupData, dp_SetGroupName, dp_SetPlayerData,
        dp_SetPlayerName, dp_SetSessionDesc, dp_AddGroupToGroup, dp_CreateGroupInGroup, dp_DeleteGroupFromGroup,
        dp_EnumConnections, dp_EnumGroupsInGroup, dp_GetGroupConnectionSettings, dp_InitializeConnection,
        dp_SecureOpen, dp_SendChatMessage, dp_SetGroupConnectionSettings, dp_StartSession, dp_GetGroupFlags,
        dp_GetGroupParent, dp_GetPlayerAccount, dp_GetPlayerFlags, dp_GetGroupOwner, dp_SetGroupOwner, dp_SendEx,
        dp_GetMessageQueue, dp_CancelMessage, dp_CancelPriority };
    void* lob[19] = {
        lob_QueryInterface, lob_AddRef, lob_Release, lob_Connect, lob_CreateAddress, lob_EnumAddress,
        lob_EnumAddressTypes, lob_EnumLocalApplications, lob_GetConnectionSettings, lob_ReceiveLobbyMessage,
        lob_RunApplication, lob_SendLobbyMessage, lob_SetConnectionSettings, lob_SetLobbyMessageEvent,
        lob_CreateCompoundAddress, lob_ConnectEx, lob_RegisterApplication, lob_UnregisterApplication,
        lob_WaitForConnectionSettings };
    void* dp1[25] = {
        dp1_QueryInterface, dp1_AddRef, dp1_Release, dp1_AddPlayerToGroup, dp1_Close, dp1_CreatePlayer,
        dp1_CreateGroup, dp1_DeletePlayerFromGroup, dp1_DestroyPlayer, dp1_DestroyGroup, dp1_EnableNewPlayers,
        dp1_EnumGroupPlayers, dp1_EnumGroups, dp1_EnumPlayers, dp1_EnumSessions, dp1_GetCaps,
        dp1_GetMessageCount, dp1_GetPlayerCaps, dp1_GetPlayerName, dp1_Initialize, dp1_Open, dp1_Receive,
        dp1_SaveSession, dp1_Send, dp1_SetPlayerName };
    memcpy(g_dp_vtbl, dp, sizeof dp);
    memcpy(g_dp1_vtbl, dp1, sizeof dp1);
    memcpy(g_lob_vtbl, lob, sizeof lob);
    if (!g_inited) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_send_cs);
        g_reply_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_inited = 1;
    }
    native32_redirect("DirectPlayCreate", my_DirectPlayCreate);
    native32_redirect("DirectPlayEnumerateA", my_DirectPlayEnumerateA);
    native32_redirect("DirectPlayLobbyCreateA", my_DirectPlayLobbyCreateA);
    native32_redirect("CoCreateInstance", my_CoCreateInstance);
    LOG("the host's DirectPlay: TCP/IP on port %d%s%s%s", g_port, g_join[0] ? ", also probing " : "", g_join,
        g_cross ? "; --mp-cross: the lift's stamp" : "");
#endif
}

void dplay_finish(void) {
    if (g_native || !g_inited) return;
    if (g_sent_msgs || g_recv_msgs)
        LOG("sent %lld messages (%lld bytes), received %lld (%lld bytes); %lld sync records logged", g_sent_msgs, g_sent_bytes, g_recv_msgs, g_recv_bytes, g_sync_lines);
}
