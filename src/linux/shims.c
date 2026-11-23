/*
 * Galactic Battlegrounds' own answers on win32hle (Linux, macOS), registered
 * ahead of win32hle's so they win.
 *
 * Two kinds. First, what the Windows host shims for this game too, done the
 * win32hle way: the play disc as a CD-ROM drive, file opens seen by --files
 * and --keys' on:NAME, message boxes answered on a test run. Second, the 21
 * imports win32hle does not answer yet (docs/linux-macos.md, "The import
 * census"), each the smallest answer that lets the game go on. DirectPlay
 * is dplay.c's and the films movie.c's (both registered first).
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
#include "widescreen.h"
#include "dplay.h"

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
    char name[512], film[1024], guest[1024];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    uint32_t esp = g_esp, arg0 = A32(0);
    g_CreateFileA();
    if (g_eax == 0xFFFFFFFFu && swgb_movie_find(name, film, sizeof film)) {   /* --films DIR */
        hle_guest_path(film, guest, sizeof guest);
        g_esp = esp;
        MEM32(g_esp + 4) = hle_guest_strdup(guest);
        g_CreateFileA();
        MEM32(esp + 4) = arg0;
        if (swgb_log_files) printf("[host] open %s -> missing, --films has %s\n", name, film);
        snprintf(name, sizeof name, "%s", guest);
    }
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

static void s_OpenFile(void) {                            /* 3 arguments */
    char name[512];
    snprintf(name, sizeof name, "%s", ASTR(0) ? ASTR(0) : "(null)");
    g_OpenFile();
    swgb_file_seen(name, g_eax != 0xFFFFFFFFu);
}

/* ---- windows: logged as the Windows host logs them ---------------------------
 * tools/conformance.py's "menu" milestone is the main menu's text-entry
 * window (InputDisabledWindow). */
static recomp_func_t g_CreateWindowExA;
static void s_CreateWindowExA(void) {                     /* 12 arguments */
    uint32_t cls = A32(2);
    printf("[host] CreateWindowExA \"%s\" style %08X at %d,%d %dx%d parent %08X\n",
           cls > 0xFFFF && ASTR(2) ? ASTR(2) : "", A32(3), (int)A32(4), (int)A32(5), (int)A32(6), (int)A32(7), A32(8));
    g_CreateWindowExA();
}

/* ---- strings: --res names its size in the Options dialog (widescreen.c), and
 * the game's out-of-sync messages are logged (dplay.c) ---- */
static recomp_func_t g_LoadStringA;
static void s_LoadStringA(void) {                         /* (instance, id, buf, max) */
    uint32_t id = A32(1), buf = A32(2);
    int max = (int)A32(3);
    dplay_string(id);                                     /* the out-of-sync messages, logged */
    g_LoadStringA();
    if (widescreen_w && buf && max > 0 && (int)g_eax > 0)
        g_eax = (uint32_t)widescreen_string((char*)hle_ptr(buf), max, (int)g_eax);
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

/* MSVFW32 (MCIWndCreateA, ICInfo): movie.c's films, registered first. */

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

/* DPLAYX: the host's own DirectPlay (src/runtime/dplay.c, as on Windows),
 * registered by dplay_init ahead of these: LAN games between recomps. */

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
    { "CreateWindowExA", s_CreateWindowExA },
    { "LoadStringA", s_LoadStringA },
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
    g_CreateWindowExA = hle_shim(win32hle_user32, "CreateWindowExA");
    g_LoadStringA = hle_shim(win32hle_module, "LoadStringA");
    g_DrawTextA = hle_shim(win32hle_user32, "DrawTextA");
    win32hle_register(g_shims);
}
