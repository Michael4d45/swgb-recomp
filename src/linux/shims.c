/*
 * Galactic Battlegrounds' own answers on win32hle (Linux, macOS), registered
 * ahead of win32hle's so they win.
 *
 * Two kinds. First, what the Windows host shims for this game too, done the
 * win32hle way: the play disc as a CD-ROM drive, file opens seen by --files
 * and --keys' on:NAME, message boxes answered on a test run. Second, the 21
 * imports win32hle does not answer yet (docs/linux-macos.md, "The import
 * census"), each the smallest answer that lets the game go on. DirectPlay
 * Create returns a stub IDirectPlay4 (multiplayer stays empty); movies
 * (MCIWnd) are refused until Track 6's player (ROADMAP.md).
 *
 * Every shim is stdcall unless it says otherwise: g_esp -> [ret][arg0]...,
 * and RET pops the return address and its arguments (win32hle.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "win32hle.h"
#include "swgb_linux.h"

/* ---- calling win32hle's own answer ------------------------------------------
 * A wrapper runs win32hle's shim with the stack as it found it: the shim
 * reads the same arguments and pops them. */
static recomp_func_t hle_shim(const win32hle_shim* table, const char* name) {
    for (; table->name; table++)
        if (!strcmp(table->name, name)) return table->fn;
    hle_fatal("win32hle has no %s to wrap", name);
    return NULL;
}

/* ---- the play disc --------------------------------------------------------------
 * The game checks the drive letter in CDPath (main.c seeds it) for a CD-ROM
 * with Game\Avi on it. win32hle calls every mapped drive a fixed disk, so the
 * disc's letter is answered here. */
static void s_GetDriveTypeA(void) {
    const char* r = ASTR(0);
    char root[4] = "?:\\", host[1024];
    if (!r || !*r) RET(3u, 1);                            /* DRIVE_FIXED: the current drive */
    char c = (char)(r[0] & ~0x20);
    if (swgb_cd_letter && c == swgb_cd_letter) RET(5u, 1);   /* DRIVE_CDROM */
    root[0] = c;
    RET(hle_host_path(root, host, sizeof host) && host[0] ? 3u : 1u, 1);   /* DRIVE_NO_ROOT_DIR */
}

static void s_GetLogicalDrives(void) {
    uint32_t m = 1u << 2 | 1u << 25;                      /* C: (the game) and Z: (/) */
    if (swgb_cd_letter) m |= 1u << (swgb_cd_letter - 'A');
    RET(m, 0);
}

/* ---- files: --files and on:NAME --------------------------------------------- */
static recomp_func_t g_CreateFileA, g_OpenFile;

static void s_CreateFileA(void) {                         /* 7 arguments */
    char name[512];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    g_CreateFileA();
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

static void s_OpenFile(void) {                            /* 3 arguments */
    char name[512];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    g_OpenFile();
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

/* ---- message boxes ---------------------------------------------------------------
 * Printed; a test run answers No or Cancel, as the Windows host does. */
static recomp_func_t g_MessageBoxA;
static void s_MessageBoxA(void) {                         /* (hwnd, text, caption, type) */
    printf("[host] MessageBoxA \"%s\": %s\n", ASTR(2) ? ASTR(2) : "", ASTR(1) ? ASTR(1) : "");
    if (!swgb_test_run) { g_MessageBoxA(); return; }
    uint32_t b = A32(3) & 0xFu;
    RET(b == 4 || b == 3 ? 7u                             /* MB_YESNO(CANCEL) -> IDNO */
        : b == 5 || b == 1 ? 2u                           /* MB_RETRYCANCEL, MB_OKCANCEL -> IDCANCEL */
        : b == 2 ? 3u : 1u, 4);                           /* MB_ABORTRETRYIGNORE -> IDABORT; IDOK */
}

/* ---- the imports win32hle has no answer for ------------------------------------ */

/* MSVFW32: no video codecs, and no movie window yet. MCIWndCreateA is cdecl:
 * the caller pops its four arguments. NULL is what the game sees on a PC
 * whose movies are missing, and it goes on without them. */
static void s_ICInfo(void) { RET(0, 3); }
static void s_MCIWndCreateA(void) {
    printf("[host] MCIWndCreateA %s: no movie player on this host yet\n", ASTR(3) ? ASTR(3) : "(null)");
    g_eax = 0;
    g_esp += 4;
}

/* WINMM: the CD music. MCI's cdaudio device is not here (the Ogg Vorbis
 * tracks of the Steam release are the plan: docs/linux-macos.md, "Music"). */
static void s_mciSendCommandA(void) {                     /* (device, msg, flags, params) */
    static int once;
    if (!once++) printf("[host] mciSendCommandA(0x%X): no CD audio on this host yet\n", A32(1));
    RET(263u, 4);                                         /* MCIERR_INVALID_DEVICE_NAME */
}

/* KERNEL32 */
static void s_IsDBCSLeadByte(void) { RET(0, 1); }         /* code page 1252: no lead bytes */
static void s_GetPriorityClass(void) { RET(0x20u, 1); }   /* NORMAL_PRIORITY_CLASS */
static void s_WinExec(void) {
    printf("[host] WinExec(%s): not on this host\n", ASTR(0) ? ASTR(0) : "");
    RET(2u, 2);                                           /* ERROR_FILE_NOT_FOUND */
}

/* USER32 */
static recomp_func_t g_DrawTextA;
static void s_GetLastActivePopup(void) { RET(A32(0), 1); }
static void s_GetWindowThreadProcessId(void) {            /* (hwnd, &pid) */
    if (A32(1)) MEM32(A32(1)) = 1;
    RET(hle_thread_id(), 2);
}
static void s_IsClipboardFormatAvailable(void) { RET(0, 1); }
static void s_SystemParametersInfoA(void) {               /* (action, ui, pv, winini) */
    uint32_t pv = A32(2);
    int w = 800, h = 600, bpp;
    switch (A32(0)) {
    case 0x30u:                                           /* SPI_GETWORKAREA: the display mode */
        hle_dd_mode(&w, &h, &bpp);
        if (pv) MEM32(pv) = 0, MEM32(pv + 4) = 0, MEM32(pv + 8) = (uint32_t)w, MEM32(pv + 12) = (uint32_t)h;
        break;
    default:                                              /* a GET answers off/zero, a SET is taken */
        if (pv && (A32(0) & 1u) == 0) MEM32(pv) = 0;
        break;
    }
    RET(1, 4);
}
static void s_SetClassLongA(void) { RET(0, 3); }
static void s_GetCaretBlinkTime(void) { RET(530u, 0); }   /* Windows' default */
static void s_DrawTextExA(void) {                         /* DrawTextA's five, and DRAWTEXTPARAMS */
    g_DrawTextA();                                        /* pops ret + 5 arguments */
    g_esp += 4;                                           /* the sixth */
}
static void s_SetSysColors(void) { RET(1, 3); }
static void s_IsIconic(void) { RET(0, 1); }

/* GDI32: an information context is a DC that answers GetDeviceCaps. */
static void s_CreateICA(void) {
    static uint8_t* px;
    if (!px) px = (uint8_t*)hle_alloc(4);
    RET(hle_gdi_surface_dc(px, 1, 1, 4, 32), 4);
}

/* DPLAYX by ordinal: multiplayer is out of scope (ROADMAP.md, "Later"), but
 * the Genie boot path calls DirectPlayCreate as a version check and then keeps
 * a live IDirectPlay4 for the process. Refuse Create and the game MessageBox-
 * exits ("requires DirectPlay 6.1a"). Serve a stub COM object whose methods
 * succeed as no-ops / empty enums so single-player can run; EnumConnections
 * and EnumSessions never report a provider or session. */
#define DP_OK 0u
#define DPERR_UNAVAILABLE 0x887703E8u   /* MAKE_DPHRESULT(1000) */
static uint32_t g_vt_dp4;

#define DP_STUB(name, argc) \
    static void name(void) { RET(DP_OK, argc); }

DP_STUB(dp_AddPlayerToGroup, 3)
DP_STUB(dp_Close, 1)
DP_STUB(dp_CreateGroup, 6)
DP_STUB(dp_CreatePlayer, 7)
DP_STUB(dp_DeletePlayerFromGroup, 3)
DP_STUB(dp_DestroyGroup, 2)
DP_STUB(dp_DestroyPlayer, 2)
DP_STUB(dp_EnumGroupPlayers, 6)
DP_STUB(dp_EnumGroups, 5)
DP_STUB(dp_EnumPlayers, 5)
DP_STUB(dp_EnumSessions, 6)
DP_STUB(dp_GetCaps, 3)
DP_STUB(dp_GetGroupData, 5)
DP_STUB(dp_GetGroupName, 4)
DP_STUB(dp_GetMessageCount, 3)
DP_STUB(dp_GetPlayerAddress, 4)
DP_STUB(dp_GetPlayerCaps, 4)
DP_STUB(dp_GetPlayerData, 5)
DP_STUB(dp_GetPlayerName, 4)
DP_STUB(dp_GetSessionDesc, 3)
DP_STUB(dp_Initialize, 2)
DP_STUB(dp_Open, 3)
DP_STUB(dp_Receive, 6)
DP_STUB(dp_Send, 6)
DP_STUB(dp_SetGroupData, 5)
DP_STUB(dp_SetGroupName, 4)
DP_STUB(dp_SetPlayerData, 5)
DP_STUB(dp_SetPlayerName, 4)
DP_STUB(dp_SetSessionDesc, 3)
DP_STUB(dp_AddGroupToGroup, 3)
DP_STUB(dp_CreateGroupInGroup, 7)
DP_STUB(dp_DeleteGroupFromGroup, 3)
DP_STUB(dp_EnumConnections, 5)
DP_STUB(dp_EnumGroupsInGroup, 6)
DP_STUB(dp_GetGroupConnectionSettings, 5)
DP_STUB(dp_InitializeConnection, 3)
DP_STUB(dp_SecureOpen, 5)
DP_STUB(dp_SendChatMessage, 5)
DP_STUB(dp_SetGroupConnectionSettings, 4)
DP_STUB(dp_StartSession, 3)
DP_STUB(dp_GetGroupFlags, 3)
DP_STUB(dp_GetGroupParent, 3)
DP_STUB(dp_GetPlayerAccount, 5)
DP_STUB(dp_GetPlayerFlags, 3)
DP_STUB(dp_GetGroupOwner, 3)
DP_STUB(dp_SetGroupOwner, 3)
DP_STUB(dp_SendEx, 10)
DP_STUB(dp_GetMessageQueue, 6)
DP_STUB(dp_CancelMessage, 3)
DP_STUB(dp_CancelPriority, 4)

static uint32_t dp4_vtable(void) {
    if (g_vt_dp4) return g_vt_dp4;
    static const win32hle_shim methods[] = {
        { "IDirectPlay4::QueryInterface", hle_com_QueryInterface },
        { "IDirectPlay4::AddRef", hle_com_AddRef },
        { "IDirectPlay4::Release", hle_com_Release },
        { "IDirectPlay4::AddPlayerToGroup", dp_AddPlayerToGroup },
        { "IDirectPlay4::Close", dp_Close },
        { "IDirectPlay4::CreateGroup", dp_CreateGroup },
        { "IDirectPlay4::CreatePlayer", dp_CreatePlayer },
        { "IDirectPlay4::DeletePlayerFromGroup", dp_DeletePlayerFromGroup },
        { "IDirectPlay4::DestroyGroup", dp_DestroyGroup },
        { "IDirectPlay4::DestroyPlayer", dp_DestroyPlayer },
        { "IDirectPlay4::EnumGroupPlayers", dp_EnumGroupPlayers },
        { "IDirectPlay4::EnumGroups", dp_EnumGroups },
        { "IDirectPlay4::EnumPlayers", dp_EnumPlayers },
        { "IDirectPlay4::EnumSessions", dp_EnumSessions },
        { "IDirectPlay4::GetCaps", dp_GetCaps },
        { "IDirectPlay4::GetGroupData", dp_GetGroupData },
        { "IDirectPlay4::GetGroupName", dp_GetGroupName },
        { "IDirectPlay4::GetMessageCount", dp_GetMessageCount },
        { "IDirectPlay4::GetPlayerAddress", dp_GetPlayerAddress },
        { "IDirectPlay4::GetPlayerCaps", dp_GetPlayerCaps },
        { "IDirectPlay4::GetPlayerData", dp_GetPlayerData },
        { "IDirectPlay4::GetPlayerName", dp_GetPlayerName },
        { "IDirectPlay4::GetSessionDesc", dp_GetSessionDesc },
        { "IDirectPlay4::Initialize", dp_Initialize },
        { "IDirectPlay4::Open", dp_Open },
        { "IDirectPlay4::Receive", dp_Receive },
        { "IDirectPlay4::Send", dp_Send },
        { "IDirectPlay4::SetGroupData", dp_SetGroupData },
        { "IDirectPlay4::SetGroupName", dp_SetGroupName },
        { "IDirectPlay4::SetPlayerData", dp_SetPlayerData },
        { "IDirectPlay4::SetPlayerName", dp_SetPlayerName },
        { "IDirectPlay4::SetSessionDesc", dp_SetSessionDesc },
        { "IDirectPlay4::AddGroupToGroup", dp_AddGroupToGroup },
        { "IDirectPlay4::CreateGroupInGroup", dp_CreateGroupInGroup },
        { "IDirectPlay4::DeleteGroupFromGroup", dp_DeleteGroupFromGroup },
        { "IDirectPlay4::EnumConnections", dp_EnumConnections },
        { "IDirectPlay4::EnumGroupsInGroup", dp_EnumGroupsInGroup },
        { "IDirectPlay4::GetGroupConnectionSettings", dp_GetGroupConnectionSettings },
        { "IDirectPlay4::InitializeConnection", dp_InitializeConnection },
        { "IDirectPlay4::SecureOpen", dp_SecureOpen },
        { "IDirectPlay4::SendChatMessage", dp_SendChatMessage },
        { "IDirectPlay4::SetGroupConnectionSettings", dp_SetGroupConnectionSettings },
        { "IDirectPlay4::StartSession", dp_StartSession },
        { "IDirectPlay4::GetGroupFlags", dp_GetGroupFlags },
        { "IDirectPlay4::GetGroupParent", dp_GetGroupParent },
        { "IDirectPlay4::GetPlayerAccount", dp_GetPlayerAccount },
        { "IDirectPlay4::GetPlayerFlags", dp_GetPlayerFlags },
        { "IDirectPlay4::GetGroupOwner", dp_GetGroupOwner },
        { "IDirectPlay4::SetGroupOwner", dp_SetGroupOwner },
        { "IDirectPlay4::SendEx", dp_SendEx },
        { "IDirectPlay4::GetMessageQueue", dp_GetMessageQueue },
        { "IDirectPlay4::CancelMessage", dp_CancelMessage },
        { "IDirectPlay4::CancelPriority", dp_CancelPriority },
        { 0, 0 }
    };
    g_vt_dp4 = hle_com_vtable(methods);
    return g_vt_dp4;
}

static void s_DirectPlayCreate(void) {                    /* #1 (guid, &dp, unk) */
    uint32_t obj = hle_com_new(dp4_vtable(), 8);
    if (A32(1)) MEM32(A32(1)) = obj;
    RET(DP_OK, 3);
}
static void s_DirectPlayEnumerateA(void) { RET(0, 2); }   /* #2 (callback, context): none */
static void s_DirectPlayLobbyCreateA(void) {              /* #4 (guid, &lobby, unk, data, size) */
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(DPERR_UNAVAILABLE, 5);
}

/* VERSION: win32hle returns "no resource". After DirectPlayCreate the game
 * versions dplayx.dll and MessageBox-exits unless FileVersion looks like
 * DirectX 6.1a (4.06.03.0516). Serve a fixed VS_FIXEDFILEINFO for any path. */
typedef struct {
    uint32_t dwSignature, dwStrucVersion;
    uint32_t dwFileVersionMS, dwFileVersionLS;
    uint32_t dwProductVersionMS, dwProductVersionLS;
    uint32_t dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype;
    uint32_t dwFileDateMS, dwFileDateLS;
} vs_fixedfileinfo;
static vs_fixedfileinfo g_ver_fixed = {
    0xFEEF04BDu, 0x00010000u,
    0x00040006u, 0x00030516u,             /* 4.06.03.0516 — DirectX 6.1a */
    0x00040006u, 0x00030516u,
    0x0000003Fu, 0, 0x00040004u, 2u, 0, 0, 0
};
static uint8_t g_ver_block[256];          /* GetFileVersionInfo's opaque blob */
static int g_ver_ready;

static void ver_ensure(void) {
    if (g_ver_ready) return;
    /* Minimal root block: a VS_FIXEDFILEINFO the VerQueryValue("\\") finds. */
    memset(g_ver_block, 0, sizeof g_ver_block);
    memcpy(g_ver_block, &g_ver_fixed, sizeof g_ver_fixed);
    g_ver_ready = 1;
}

static void s_GetFileVersionInfoSizeA(void) {             /* (path, &handle) */
    ver_ensure();
    if (A32(1)) MEM32(A32(1)) = 0;
    RET((uint32_t)sizeof g_ver_fixed, 2);
}
static void s_GetFileVersionInfoA(void) {                 /* (path, handle, len, buf) */
    uint32_t len = A32(2), buf = A32(3);
    ver_ensure();
    if (!buf || len < sizeof g_ver_fixed) RET(0, 4);
    memcpy(hle_ptr(buf), &g_ver_fixed, sizeof g_ver_fixed);
    RET(1, 4);
}
static void s_VerQueryValueA(void) {                      /* (block, subblock, &p, &len) */
    const char* sub = ASTR(1);
    ver_ensure();
    if (!sub || sub[0] != '\\' || sub[1] != '\0') {       /* only "\\" (root FIXEDFILEINFO) */
        if (A32(2)) MEM32(A32(2)) = 0;
        if (A32(3)) MEM32(A32(3)) = 0;
        RET(0, 4);
    }
    /* Point at the FIXEDFILEINFO inside the caller's GetFileVersionInfo buffer. */
    if (A32(2)) MEM32(A32(2)) = A32(0);
    if (A32(3)) MEM32(A32(3)) = (uint32_t)sizeof g_ver_fixed;
    RET(1, 4);
}

/* IMM32: no input method. */
static void s_ImmGetOpenStatus(void) { RET(0, 1); }
static void s_ImmGetDefaultIMEWnd(void) { RET(0, 1); }

static const win32hle_shim g_shims[] = {
    { "GetDriveTypeA", s_GetDriveTypeA },
    { "GetLogicalDrives", s_GetLogicalDrives },
    { "CreateFileA", s_CreateFileA },
    { "OpenFile", s_OpenFile },
    { "MessageBoxA", s_MessageBoxA },
    { "ICInfo", s_ICInfo },
    { "MCIWndCreateA", s_MCIWndCreateA },
    { "mciSendCommandA", s_mciSendCommandA },
    { "IsDBCSLeadByte", s_IsDBCSLeadByte },
    { "GetPriorityClass", s_GetPriorityClass },
    { "WinExec", s_WinExec },
    { "GetLastActivePopup", s_GetLastActivePopup },
    { "GetWindowThreadProcessId", s_GetWindowThreadProcessId },
    { "IsClipboardFormatAvailable", s_IsClipboardFormatAvailable },
    { "SystemParametersInfoA", s_SystemParametersInfoA },
    { "SetClassLongA", s_SetClassLongA },
    { "GetCaretBlinkTime", s_GetCaretBlinkTime },
    { "DrawTextExA", s_DrawTextExA },
    { "SetSysColors", s_SetSysColors },
    { "IsIconic", s_IsIconic },
    { "CreateICA", s_CreateICA },
    { "dplayx.dll#1", s_DirectPlayCreate },
    { "dplayx.dll#2", s_DirectPlayEnumerateA },
    { "dplayx.dll#4", s_DirectPlayLobbyCreateA },
    { "GetFileVersionInfoSizeA", s_GetFileVersionInfoSizeA },
    { "GetFileVersionInfoA", s_GetFileVersionInfoA },
    { "VerQueryValueA", s_VerQueryValueA },
    { "ImmGetOpenStatus", s_ImmGetOpenStatus },
    { "ImmGetDefaultIMEWnd", s_ImmGetDefaultIMEWnd },
    { 0, 0 }
};

void swgb_register_shims(void) {
    g_CreateFileA = hle_shim(win32hle_kernel32_ext, "CreateFileA");
    g_OpenFile = hle_shim(win32hle_kernel32_ext, "OpenFile");
    g_MessageBoxA = hle_shim(win32hle_user32, "MessageBoxA");
    g_DrawTextA = hle_shim(win32hle_user32, "DrawTextA");
    win32hle_register(g_shims);
}
