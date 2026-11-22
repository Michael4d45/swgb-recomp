/*
 * Star Wars: Galactic Battlegrounds - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks,
 * machine lock and guest exceptions; see its header). Every import goes to
 * the real DLL -- DirectSound, DirectPlay, winmm, VfW -- except DirectDraw,
 * which is softdd (softdd.c): the game's fullscreen 8-bit mode in a window,
 * or nowhere with --headless, and the desktop's mode never changes. What is
 * here is only what is specific to this game: where the image goes, the
 * command line, the play disc, the window, scripted input, muting and the
 * fault report. Taken from X-Wing's host (xwing-recomp), which is Crazy
 * Taxi's.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's
 * 0x00400000..0x00A5A000 is free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <shlwapi.h>
#include <windowsx.h>

#include "native32.h"
#include "recomp_trace.h"
#include "softdd.h"
#include "softdd_int.h"
#include "widescreen.h"
#include "framerate.h"
#include "movie.h"
#include "present.h"
#include "coverage.h"

extern const uint32_t swgb_entry_va;  /* recomp_dispatch.c */

#define IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_headless;
static int   g_hide;   /* --headless without a private desktop: keep windows off the screen */
static HWND  g_game_wnd;

/* --shot SECONDS FILE: the next frame presented after SECONDS, as a .bmp. */
static DWORD g_shot_s[16];
static char  g_shot_file[16][MAX_PATH];
static int   g_shots;
static DWORD WINAPI shot_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_shots; i++) {
        DWORD at = t0 + g_shot_s[i] * 1000, now = GetTickCount();
        if ((int)(at - now) > 0) Sleep(at - now);
        softdd_shot(g_shot_file[i]);
    }
    return 0;
}

/* ---- the process is the host, the module is the guest ----------------------
 * GetModuleHandleA(NULL) and GetModuleFileNameA(NULL) would name the host;
 * the game finds its own folder through them. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 512];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0)) : IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* A shim runs holding the machine, and a window call can wait on another
 * thread answering messages: run it with the machine let go. */
#define UNLOCKED(call) do { uint32_t esp_ = g_esp; mach_leave(); \
    call; DWORD err_ = GetLastError(); mach_enter(); \
    g_esp = esp_; SetLastError(err_); } while (0)

static void shim_MessageBoxA(void) {
    const char* text = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
    const char* cap = ARG(2) ? (const char*)(uintptr_t)ARG(2) : "";
    printf("[host] MessageBoxA \"%s\": %s\n", cap, text);
    if (g_headless || g_watchdog_s) {   /* test runs never put a box on the screen */
        uint32_t b = ARG(3) & MB_TYPEMASK;
        g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO
              : (b == MB_RETRYCANCEL || b == MB_OKCANCEL) ? IDCANCEL
              : b == MB_ABORTRETRYIGNORE ? IDABORT : IDOK;
    } else {
        HWND w = (HWND)(uintptr_t)ARG(0);
        UINT type = ARG(3);
        int r;
        UNLOCKED(r = MessageBoxA(w, text, cap, type));
        g_eax = r;
    }
    g_esp += 4 + 4 * 4;
}

/* The game allows one copy: CreateMutexA with its name, and on
 * ERROR_ALREADY_EXISTS it finds the other window and exits (0x004279E0).
 * --multi makes every named mutex this process's own, so several runs (test
 * agents working side by side) do not end each other. Without it, nothing
 * changes. */
static int g_multi;
static void shim_CreateMutexA(void) {
    LPSECURITY_ATTRIBUTES sa = (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(0);
    BOOL own = (BOOL)ARG(1);
    const char* name = (const char*)(uintptr_t)ARG(2);
    char unique[MAX_PATH];
    if (g_multi && name) {
        _snprintf(unique, sizeof unique - 1, "%s.%lu", name, GetCurrentProcessId());
        unique[sizeof unique - 1] = 0;
        name = unique;
    }
    HANDLE h = CreateMutexA(sa, own, name);
    DWORD err = GetLastError();
    SetLastError(err);   /* the game reads ERROR_ALREADY_EXISTS from here */
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 3 * 4;
}

/* The game keeps its options (Screen Size, speeds, volumes) in HKCU. With
 * --multi, this process's HKCU is a private copy kept per game folder
 * (HKCU\Software\swgb-recomp\hives\<folder>, seeded from the real keys on its
 * first use): a run that changes Screen Size does not change it for the
 * user's game or for another agent's run, and the setting still persists
 * across runs of the same folder. Without --multi, nothing changes. */
static void reg_isolate(const char* game_full) {
    char name[MAX_PATH], key[MAX_PATH + 64];
    static const char lucas[] = "Software\\LucasArts Entertainment Company LLC";
    HKEY hive, sub;
    DWORD created;
    int n = 0;
    for (const char* p = game_full; *p && n < MAX_PATH - 1; p++) name[n++] = (*p == '\\' || *p == '/' || *p == ':') ? '_' : *p;
    name[n] = 0;
    _snprintf(key, sizeof key - 1, "Software\\swgb-recomp\\hives\\%s", name);
    key[sizeof key - 1] = 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hive, &created)) return;
    if (created && !RegCreateKeyExA(hive, lucas, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &sub, NULL)) {
        RegCopyTreeA(HKEY_CURRENT_USER, lucas, sub);
        RegCloseKey(sub);
    }
    if (RegOverridePredefKey(HKEY_CURRENT_USER, hive)) printf("  registry: could not isolate HKCU\n");
    else printf("  registry: HKCU is private to %s\n", game_full);
}

static void shim_OutputDebugStringA(void) {
    if (ARG(0)) printf("[dbg] %s", (const char*)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static void shim_ExitProcess(void) {
    UINT code = ARG(0);
    printf("[host] ExitProcess(%u) from sub_%08X\n", code, g_cur_func);
    softdd_finish();
    recomp_trace_flush();
    fflush(NULL);   /* ExitProcess skips the CRT's own flush: a piped log lost its tail */
    ExitProcess(code);
}

/* ---- the play disc -------------------------------------------------------------
 * The game wants disc 2, the play disc, in a real CD-ROM drive (your disc, or
 * your disc image mounted) and reads its movies from it (%sgame\avi\%s.avi).
 * The host only finds that drive (CD_DRIVE=E or --cd E, or the first CD-ROM
 * drive with Game\Avi on it) and stores its letter in CDPath, the one letter
 * the game checks after its first run (HKLM, virtualised to HKCU's
 * VirtualStore for this process as for the game). */
static char g_cd = 0;

static void cd_pick(const char* want) {
    char env[8], root[] = "?:\\", probe[16];
    if (want && *want) g_cd = (char)(want[0] & ~0x20);
    else if (GetEnvironmentVariableA("CD_DRIVE", env, sizeof env)) g_cd = (char)(env[0] & ~0x20);
    for (char c = 'D'; c <= 'Z' && !g_cd; c++) {
        root[0] = c;
        _snprintf(probe, sizeof probe, "%sGame\\Avi", root);
        if (GetDriveTypeA(root) == DRIVE_CDROM && GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) g_cd = c;
    }
    if (!g_cd) { printf("  no play disc found: put disc 2 in the drive or mount its image (or set CD_DRIVE)\n"); return; }
    root[0] = g_cd;
    RegSetKeyValueA(HKEY_LOCAL_MACHINE, "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0",
                    "CDPath", REG_SZ, root, sizeof root);
    printf("  play disc: %c:\n", g_cd);
}

/* "on:NAME" in --keys waits until the game opens a file whose name has NAME
 * in it, and the times after it count from then. */
static char g_wait_file[64];
static HANDLE g_file_seen;
static int g_log_files;      /* --files: every open */

static void file_seen(const char* name, int ok) {
    if (g_log_files) printf("[host] open %s -> %s\n", name ? name : "(null)", ok ? "ok" : "missing");
    if (g_wait_file[0] && name && StrStrIA(name, g_wait_file)) { g_wait_file[0] = 0; SetEvent(g_file_seen); }
}

static void shim_CreateFileA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    char film[MAX_PATH];
    g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    if (g_eax == (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE && movie_find(name, film, MAX_PATH)) {   /* --films */
        if (g_log_files) printf("[host] open %s -> missing, --films has %s\n", name, film);
        name = film;
        g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                                 ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    }
    file_seen(name, g_eax != (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE);
    g_esp += 4 + 7 * 4;
}

static void shim_OpenFile(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = (uint32_t)OpenFile(name, (LPOFSTRUCT)(uintptr_t)ARG(1), ARG(2));
    file_seen(name, g_eax != (uint32_t)HFILE_ERROR);
    g_esp += 4 + 3 * 4;
}

/* MCIWndCreateA plays the movies (the logos, the intro, the campaigns'):
 * an MCI window over the game's. Ours (movie.c): FFmpeg decodes the Indeo 5
 * that Windows no longer has, and softdd composes the window's frames. */
static void shim_MCIWndCreateA(void) {
    const char* file = (const char*)(uintptr_t)ARG(3);
    printf("[host] MCIWndCreateA %s\n", file ? file : "(null)");
    HWND h;
    HWND parent = (HWND)(uintptr_t)ARG(0);
    DWORD style = ARG(2);
    if (g_hide) style &= ~WS_VISIBLE;   /* never on the screen: the movie still plays */
    UNLOCKED(h = movie_create(parent, style, file));
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4;   /* cdecl: the caller pops the four */
}

/* The game plays films only if ICInfo finds Indeo 5 (0x005E104B). */
static void shim_ICInfo(void) {
    g_eax = movie_icinfo(ARG(0), ARG(1), (void*)(uintptr_t)ARG(2));
    g_esp += 4 + 3 * 4;
}

/* ---- the window ---------------------------------------------------------------
 * The game made its window for a fullscreen mode; windowed, it is told it is
 * always active (it pauses otherwise), and mouse messages are scaled from the
 * client area to the mode softdd presents. */
static WNDPROC g_game_proc;
static HCURSOR g_game_cursor;   /* what the game last SetCursor'd (shim_SetCursor) */
static int     g_cursor_known;
static int g_scripted;   /* --keys (below) */

/* A client pixel -> a game pixel, through the picture's letterboxed rectangle
 * (present.c): the same one the frame is drawn into. */
static void to_game(POINT* p) {
    DD* d = softdd_dd();
    if (!d || !d->w || !g_game_wnd) return;
    present_to_game(g_game_wnd, d->w, d->h, p);
}

#define WM_SWGB_SIZE (WM_APP + 0x51)   /* --keys size:W:H, run on the window's own thread */
#define WM_SWGB_CURSOR (WM_APP + 0x52) /* --keys setcursor: what is the game thread's cursor? */
static volatile HCURSOR g_cursor_probe;

/* Text fields are real EDIT children that softdd draws into the frame and
 * that never take part in hit-testing themselves (an empty window region),
 * so a click on one reaches the game window. Under real DirectDraw the click
 * went to the control: send it there, as a click (down and up together: the
 * edit would capture the mouse and read the scaled window's coordinates for
 * a drag). x, y are game pixels. */
static int click_child(UINT m, WPARAM w, int x, int y) {
    POINT p = { x, y };
    for (HWND c = GetWindow(g_game_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        if (!IsWindowVisible(c) || !IsWindowEnabled(c)) continue;
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, g_game_wnd, (POINT*)&r, 2);
        if (!PtInRect(&r, p)) continue;
        POINT q = p;
        MapWindowPoints(g_game_wnd, c, &q, 1);   /* game pixels -> the control's client */
        SetFocus(c);
        SendMessageA(c, m, w, MAKELPARAM(q.x, q.y));
        SendMessageA(c, WM_LBUTTONUP, 0, MAKELPARAM(q.x, q.y));
        if (GetCapture() == c) ReleaseCapture();
        return 1;
    }
    return 0;
}

/* The window is never told it lost activation (the game would pause), so the
 * default WM_ACTIVATE puts the focus on the window itself: a field that had
 * it stops taking keys. Keep it across a trip to another window. */
static HWND g_focus_child;

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    /* resized, uncovered or a new display mode: the picture is laid out and shown again */
    if (m == WM_SIZE || m == WM_PAINT || m == WM_DISPLAYCHANGE) present_invalidate(h);
    switch (m) {
    case WM_ACTIVATEAPP: case WM_NCACTIVATE: w = TRUE; break;
    case WM_ACTIVATE: {
        HWND f = GetFocus();
        int active = LOWORD(w) != WA_INACTIVE;
        if (!active && f && IsChild(h, f)) g_focus_child = f;
        present_cursor_clip(h, active);   /* fullscreen: the cursor stays on this monitor while active */
        LRESULT r = CallWindowProcA(g_game_proc, h, m, MAKEWPARAM(WA_ACTIVE, HIWORD(w)), l);
        if (active && g_focus_child) {
            if (IsWindow(g_focus_child) && IsChild(h, g_focus_child) && IsWindowVisible(g_focus_child))
                SetFocus(g_focus_child);
            g_focus_child = NULL;
        }
        return r;
    }
    case WM_KILLFOCUS: return 0;
    case WM_WINDOWPOSCHANGING: present_fs_keep(h, m, w, l); return DefWindowProcA(h, m, w, l);   /* fullscreen survives the game's own moves */
    case WM_STYLECHANGING: present_fs_keep(h, m, w, l); return DefWindowProcA(h, m, w, l);
    case WM_WINDOWPOSCHANGED: case WM_GETMINMAXINFO:
    case WM_NCCALCSIZE: case WM_STYLECHANGED: case WM_SIZE:
        return DefWindowProcA(h, m, w, l);
    case WM_SETCURSOR:   /* the game's cursor, also back from another window and over the bars */
        if (LOWORD(l) == HTCLIENT && g_cursor_known) { SetCursor(g_game_cursor); return TRUE; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: if (w == SOFTDD_TIMER || w == SOFTDD_PACE) { softdd_tick(); return 0; } break;
    /* Alt+Enter: borderless fullscreen; Alt+F12: the next scaling filter. */
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN || w == VK_F12) {
            if (!(l & (1 << 30))) { if (w == VK_RETURN) present_set_fullscreen(h, -1); else present_cycle_filter(h); }
            return 0;
        }
        break;
    case WM_SYSKEYUP: case WM_SYSCHAR: if (w == VK_RETURN || w == VK_F12) return 0; break;
    case WM_SWGB_SIZE: present_set_client_size(h, (int)w, (int)l); return 0;
    case WM_SWGB_CURSOR:   /* w: 1 = the pointer was over another window (it set the arrow); then: what is the cursor? */
        if (w == 1) SetCursor(LoadCursorA(NULL, IDC_ARROW));
        g_cursor_probe = GetCursor();
        return 0;
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL) {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        to_game(&p);   /* scripted too: --keys posts window pixels, mapped as a real mouse is */
        l = MAKELPARAM(p.x, p.y);
        if ((m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) && click_child(m, w, p.x, p.y)) return 0;
    }
    return CallWindowProcA(g_game_proc, h, m, w, l);
}

static void shim_CreateWindowExA(void) {
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    if (g_hide) a[3] &= ~WS_VISIBLE;
    if (!a[8] && (a[3] & WS_POPUP)) { a[4] = a[5] = 64; a[6] = 800; a[7] = 600; }
    printf("[host] CreateWindowExA \"%s\" style %08X at %d,%d %dx%d parent %08X\n",
           a[2] && a[2] > 0xFFFF ? (const char*)(uintptr_t)a[2] : "", a[3], a[4], a[5], a[6], a[7], a[8]);
    HWND h;
    UNLOCKED(h = CreateWindowExA(a[0], (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], a[3],
                                 a[4], a[5], a[6], a[7], (HWND)(uintptr_t)a[8],
                                 (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]));
    DWORD err = h ? 0 : GetLastError();
    if (h && !a[8] && !g_game_proc) {
        /* The game's procedure is guest code: Windows' call to it faults on
         * the non-executable page and native32 runs the lifted body. */
        g_game_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)host_wndproc);
        g_game_wnd = h;
        PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        SetTimer(h, SOFTDD_TIMER, 100, NULL);   /* text fields: softdd_tick */
    }
    SetLastError(err);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    BOOL r;
    if (g_hide && cmd != SW_HIDE) cmd = SW_HIDE;
    UNLOCKED(r = ShowWindow(w, cmd));
    g_eax = r;
    g_esp += 4 + 2 * 4;
}

static void shim_GetForegroundWindow(void) {
    g_eax = (uint32_t)(uintptr_t)(g_game_wnd ? g_game_wnd : GetForegroundWindow());
    g_esp += 4;
}

static void shim_GetActiveWindow(void) {
    g_eax = (uint32_t)(uintptr_t)(g_game_wnd ? g_game_wnd : GetActiveWindow());
    g_esp += 4;
}

static void shim_DirectDrawCreate(void) {
    g_eax = (uint32_t)softdd_create((void**)(uintptr_t)ARG(1));
    framerate_on_ddraw(*(void**)(uintptr_t)ARG(1));
    g_esp += 4 + 3 * 4;
}

/* ---- scripted input (--keys) ---------------------------------------------------
 * "SECONDS:what,..." where what is a key name (esc, enter, space, up, down,
 * left, right, tab, f1..f10, a letter or digit), "click:X:Y", "rclick:X:Y" or "move:X:Y"
 * (game pixels), "shot=FILE", or "on:NAME" (wait for a file open). While a
 * script runs, the real mouse is not read: only the script reaches the game. */
typedef struct { DWORD at; int vk, x, y, click, move, alt, win, size, wshot, setcursor; char xs[24], ys[24], on[64], shot[MAX_PATH]; } step_t;

#define MAX_STEPS 16384   /* tools/soak.py: a 30-minute monkey run is a few thousand steps */
static step_t g_steps[MAX_STEPS];
static int g_nsteps;
static volatile DWORD g_vk_until[256];
static volatile LONG g_cur_x = 400, g_cur_y = 300;

static const struct { const char* n; int vk; } g_keynames[] = {
    { "esc", VK_ESCAPE }, { "enter", VK_RETURN }, { "space", VK_SPACE }, { "tab", VK_TAB },
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "bksp", VK_BACK }, { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 },
    { "f5", VK_F5 }, { "f6", VK_F6 }, { "f7", VK_F7 }, { "f8", VK_F8 }, { "f9", VK_F9 }, { "f10", VK_F10 },
    { "f11", VK_F11 }, { "f12", VK_F12 }, { "home", VK_HOME }, { "end", VK_END },
};

static int key_by_name(const char* n) {
    for (int i = 0; i < (int)(sizeof g_keynames / sizeof g_keynames[0]); i++)
        if (!_stricmp(n, g_keynames[i].n)) return g_keynames[i].vk;    if (n[0] && !n[1] && isalnum((unsigned char)n[0])) return toupper((unsigned char)n[0]);
    return 0;
}

/* --keys @FILE reads the script from a file (a long one does not fit a command line); a newline
 * separates steps like a comma. */
static char* keys_text(const char* spec) {
    if (spec[0] != '@') return _strdup(spec);
    FILE* f = fopen(spec + 1, "rb");
    if (!f) { fprintf(stderr, "--keys: cannot read %s\n", spec + 1); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc(n + 1);
    n = (long)fread(b, 1, n, f);
    b[n] = 0;
    fclose(f);
    for (long i = 0; i < n; i++) if (b[i] == '\r' || b[i] == '\n') b[i] = ',';
    return b;
}

static void parse_keys(const char* spec) {
    char* buf = keys_text(spec);
    for (char* tok = strtok(buf, ","); tok && g_nsteps < MAX_STEPS; tok = strtok(NULL, ",")) {
        step_t st = { 0 };
        char* c = strchr(tok, ':');
        if (!c) continue;
        if (!strncmp(tok, "on:", 3)) {
            lstrcpynA(st.on, tok + 3, sizeof st.on);
            g_steps[g_nsteps++] = st;
            continue;
        }
        st.at = (DWORD)(atof(tok) * 1000);
        char* what = c + 1;
        if (!strncmp(what, "wshot=", 6)) {   /* the window as drawn: letterbox bars and all */
            GetFullPathNameA(what + 6, MAX_PATH, st.shot, NULL);
            st.wshot = 1;
        } else if (!strncmp(what, "shot=", 5)) {
            GetFullPathNameA(what + 5, MAX_PATH, st.shot, NULL);
        } else if (!strncmp(what, "size:", 5)) {   /* the window's client area: W:H */
            st.size = 1;
            sscanf(what + 5, "%d:%d", &st.x, &st.y);
        } else if (!strncmp(what, "wclick:", 7) || !strncmp(what, "wmove:", 6)) {   /* window pixels, mapped like the mouse */
            st.win = 1;
            st.click = what[1] == 'c';
            st.move = 1;
            /* X:Y, each a number or a place in the window: L R C (edges, centre), PL PR PT PB (the picture's
             * edges), BL BR BT BB (the middle of a bar), then +N or -N: wmove:R:C, wmove:L+40:T-3 */
            if (sscanf(strchr(what, ':') + 1, "%23[^:]:%23s", st.xs, st.ys) != 2) { fprintf(stderr, "--keys: %s\n", what); exit(1); }
        } else if (!strcmp(what, "setcursor")) {   /* the pointer comes back into the window (WM_SETCURSOR) */
            st.setcursor = 1;
        } else if (!strncmp(what, "click:", 6) || !strncmp(what, "rclick:", 7) || !strncmp(what, "move:", 5)) {
            st.click = what[0] == 'c' ? 1 : what[0] == 'r' ? 2 : 0;
            st.move = 1;
            sscanf(strchr(what, ':') + 1, "%d:%d", &st.x, &st.y);
        } else if (!strncmp(what, "alt+", 4) && (st.vk = key_by_name(what + 4))) {
            st.alt = 1;   /* Alt+Enter, Alt+F12: the presenter's hotkeys */
        } else if (!(st.vk = key_by_name(what))) {
            fprintf(stderr, "--keys: unknown key \"%s\"\n", what);
            exit(1);
        }
        g_steps[g_nsteps++] = st;
    }
    g_scripted = 1;
}

/* The scripted cursor, in screen pixels, the way a real one is: it is where the script put it
 * (a window place or a game pixel, turned into a screen pixel through the window's real
 * placement), held inside the screen (the monitor the window is on) and, in fullscreen, inside
 * the rectangle the cursor is confined to. GetCursorPos reads it back through the same path as
 * the real cursor: screen -> client -> the letterbox -> game pixels. */
static volatile LONG g_vcur_x, g_vcur_y;
static volatile int  g_vcur_valid;

static POINT vcur_set_client(HWND w, int cx, int cy) {
    POINT s = { cx, cy };
    RECT lim;
    ClientToScreen(w, &s);
    if (!present_clip_get(&lim)) {
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) lim = mi.rcMonitor;
        else { lim.left = lim.top = -32768; lim.right = lim.bottom = 32767; }
    }
    if (s.x < lim.left) s.x = lim.left; else if (s.x >= lim.right) s.x = lim.right - 1;
    if (s.y < lim.top) s.y = lim.top; else if (s.y >= lim.bottom) s.y = lim.bottom - 1;
    g_vcur_x = s.x, g_vcur_y = s.y, g_vcur_valid = 1;
    ScreenToClient(w, &s);
    return s;   /* the client pixel the cursor ended on (may be outside the window) */
}

/* "R-3", "C", "PL+10", "120": a place on one axis of the window's client area (see parse_keys) */
static int place(const char* t, int axis, const RECT* cl, const RECT* pic) {
    char name[8] = "";
    int n = 0, base = 0;
    const char* q = t;
    while (isalpha((unsigned char)*q) && n < 7) name[n++] = *q++;
    name[n] = 0;
    int cw = cl->right, ch = cl->bottom;
    if (!n) return atoi(t);
    if (axis == 0) {
        if (!_stricmp(name, "L")) base = 0;
        else if (!_stricmp(name, "R")) base = cw - 1;
        else if (!_stricmp(name, "C")) base = cw / 2;
        else if (!_stricmp(name, "PL")) base = pic->left;
        else if (!_stricmp(name, "PR")) base = pic->right - 1;
        else if (!_stricmp(name, "BL")) base = pic->left / 2;
        else if (!_stricmp(name, "BR")) base = (pic->right + cw) / 2;
        else { fprintf(stderr, "--keys: no place %s on x\n", name); exit(1); }
    } else {
        if (!_stricmp(name, "T")) base = 0;
        else if (!_stricmp(name, "B")) base = ch - 1;
        else if (!_stricmp(name, "C")) base = ch / 2;
        else if (!_stricmp(name, "PT")) base = pic->top;
        else if (!_stricmp(name, "PB")) base = pic->bottom - 1;
        else if (!_stricmp(name, "BT")) base = pic->top / 2;
        else if (!_stricmp(name, "BB")) base = (pic->bottom + ch) / 2;
        else { fprintf(stderr, "--keys: no place %s on y\n", name); exit(1); }
    }
    return base + (*q ? atoi(q) : 0);
}

static DWORD WINAPI keys_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nsteps; i++) {
        step_t* st = &g_steps[i];
        if (st->on[0]) {
            if (!st->x) { lstrcpynA(g_wait_file, st->on, sizeof g_wait_file); st->x = 1; }
            WaitForSingleObject(g_file_seen, INFINITE);
            t0 = GetTickCount();
            printf("[keys] %s opened: times count from here\n", st->on);
            continue;
        }
        for (int j = i + 1; j < g_nsteps; j++)   /* the next wait listens from now */
            if (g_steps[j].on[0]) {
                if (!g_steps[j].x) { lstrcpynA(g_wait_file, g_steps[j].on, sizeof g_wait_file); g_steps[j].x = 1; }
                break;
            }
        DWORD at = t0 + st->at, now = GetTickCount();
        if ((int)(at - now) > 0) Sleep(at - now);
        if (st->shot[0]) { if (st->wshot) present_wshot_request(st->shot); else softdd_shot(st->shot); continue; }
        HWND w = g_game_wnd;
        if (st->size) {
            if (w) PostMessageA(w, WM_SWGB_SIZE, (WPARAM)st->x, (LPARAM)st->y);
            printf("[keys] %.1fs size %dx%d\n", st->at / 1000.0, st->x, st->y);
            continue;
        }
        if (st->setcursor) {   /* the pointer left the window and is back: what is the cursor now? */
            HCURSOR before = NULL, after = NULL;
            if (w) { g_cursor_probe = (HCURSOR)(intptr_t)-1; PostMessageA(w, WM_SWGB_CURSOR, 0, 0); Sleep(300); before = g_cursor_probe; }
            if (w) { PostMessageA(w, WM_SWGB_CURSOR, 1, 0); Sleep(100); }   /* the pointer was elsewhere: the arrow */
            if (w) PostMessageA(w, WM_SETCURSOR, (WPARAM)w, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));   /* and is back */
            Sleep(300);
            if (w) { g_cursor_probe = (HCURSOR)(intptr_t)-1; PostMessageA(w, WM_SWGB_CURSOR, 0, 0); Sleep(300); after = g_cursor_probe; }
            printf("[cursor] re-entry: the game's cursor %p, after WM_SETCURSOR %p (the arrow is %p): %s\n",
                   (void*)before, (void*)after, (void*)LoadCursorA(NULL, IDC_ARROW), before == after ? "kept" : "CHANGED");
            continue;
        }
        int cx = st->x, cy = st->y;   /* the client pixel the script means */
        if (st->move) {
            DD* d = softdd_dd();
            if (st->win && w) {
                RECT cl, pic;
                GetClientRect(w, &cl);
                if (d && d->w) present_layout(present_filter, cl.right, cl.bottom, d->w, d->h, &pic); else pic = cl;
                cx = place(st->xs, 0, &cl, &pic);
                cy = place(st->ys, 1, &cl, &pic);
            } else if (w && d && d->w) {   /* game pixels: the client pixel that maps to them */
                POINT p = { st->x, st->y };
                present_to_client(w, d->w, d->h, &p);
                cx = p.x, cy = p.y;
            }
            if (w) {
                POINT c = vcur_set_client(w, cx, cy);   /* held inside the screen, or the clip */
                cx = c.x, cy = c.y;
                if (d && d->w) { POINT g = c; present_to_game(w, d->w, d->h, &g); g_cur_x = g.x, g_cur_y = g.y; st->x = g.x, st->y = g.y; }
            } else g_cur_x = st->x, g_cur_y = st->y;
            if (st->win) printf("[keys] window %d,%d -> game %d,%d\n", cx, cy, st->x, st->y);
            if (w) PostMessageA(w, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
            if (st->click) {
                Sleep(100);
                int r = st->click == 2;
                g_vk_until[r ? VK_RBUTTON : VK_LBUTTON] = GetTickCount() + 150;
                if (w) PostMessageA(w, r ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, r ? MK_RBUTTON : MK_LBUTTON, MAKELPARAM(cx, cy));
                Sleep(150);
                if (w) PostMessageA(w, r ? WM_RBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
            }
            printf("[keys] %.1fs %s %d,%d\n", st->at / 1000.0, st->click == 2 ? "rclick" : st->click ? "click" : "move", st->x, st->y);
        } else {
            UINT sc = MapVirtualKeyA(st->vk, 0);
            if (st->alt) {   /* Alt+key: a system key, to the window itself */
                if (w) PostMessageA(w, WM_SYSKEYDOWN, st->vk, 1 | (sc << 16) | (1 << 29));
                Sleep(150);
                if (w) PostMessageA(w, WM_SYSKEYUP, st->vk, 1 | (sc << 16) | (1 << 29) | 0xC0000000u);
                printf("[keys] %.1fs alt+key %02X\n", st->at / 1000.0, st->vk);
                continue;
            }
            {   /* keys go where the game's focus is: a text field is a real edit control */
                GUITHREADINFO gi = { sizeof gi };
                if (w && GetGUIThreadInfo(GetWindowThreadProcessId(w, NULL), &gi) && gi.hwndFocus) w = gi.hwndFocus;
            }
            g_vk_until[st->vk & 0xFF] = GetTickCount() + 150;
            /* WM_KEYDOWN only: the game's loop calls TranslateMessage, which
             * makes the WM_CHAR (posting one too typed every letter twice). */
            if (w) PostMessageA(w, WM_KEYDOWN, st->vk, 1 | (sc << 16));
            Sleep(150);
            if (w) PostMessageA(w, WM_KEYUP, st->vk, 1 | (sc << 16) | 0xC0000000u);
            printf("[keys] %.1fs key %02X\n", st->at / 1000.0, st->vk);
        }
    }
    return 0;
}

static int held(int vk) { return (int)(g_vk_until[vk & 0xFF] - GetTickCount()) > 0; }

static void shim_GetAsyncKeyState(void) {
    int vk = (int)ARG(0);
    SHORT r = g_scripted ? 0 : GetAsyncKeyState(vk);
    if (held(vk)) r |= (SHORT)0x8000;
    g_eax = (uint16_t)r;
    g_esp += 4 + 1 * 4;
}

static void shim_GetKeyState(void) {
    int vk = (int)ARG(0);
    SHORT r = g_scripted ? 0 : GetKeyState(vk);
    if (held(vk)) r |= (SHORT)0x8000;
    g_eax = (uint16_t)r;
    g_esp += 4 + 1 * 4;
}

/* The cursor, in game pixels. Scripted, it is wherever the script put it. */
static void shim_GetCursorPos(void) {
    POINT* pt = (POINT*)(uintptr_t)ARG(0);
    if (g_scripted && g_game_wnd && g_vcur_valid) {   /* the scripted cursor: read back as the real one is */
        pt->x = g_vcur_x, pt->y = g_vcur_y;
        ScreenToClient(g_game_wnd, pt);
        to_game(pt);
    }
    else if (g_scripted || !g_game_wnd) { pt->x = g_cur_x; pt->y = g_cur_y; }
    else if (GetForegroundWindow() != g_game_wnd) {
        DD* d = softdd_dd();   /* another window is active: the cursor is nowhere near an edge */
        pt->x = d ? d->w / 2 : 400, pt->y = d ? d->h / 2 : 300;
    }
    else { GetCursorPos(pt); ScreenToClient(g_game_wnd, pt); to_game(pt); }
    g_eax = TRUE;
    g_esp += 4 + 1 * 4;
}

static void shim_SetCursorPos(void) {
    g_cur_x = (LONG)ARG(0), g_cur_y = (LONG)ARG(1);
    if (g_scripted && g_game_wnd) {   /* the game moves the cursor (game pixels): the scripted one follows */
        DD* d = softdd_dd();
        POINT p = { g_cur_x, g_cur_y };
        if (d && d->w) { present_to_client(g_game_wnd, d->w, d->h, &p); vcur_set_client(g_game_wnd, p.x, p.y); }
    }
    g_eax = TRUE;
    g_esp += 4 + 2 * 4;
}

/* The game's client <-> screen conversions work in its mode's pixels: the
 * window is wherever softdd put it, so its client origin is the game's 0,0. */
/* The game sees its window as the original did on a fullscreen mode: at 0,0,
 * exactly the mode's size, window and client alike. Its edge scrolling compares
 * GetCursorPos (above: game pixels, 0..W-1) with GetWindowRect, so the window's
 * (or the monitor's) edge is the game's edge on all four sides; and at a mode
 * change it sizes its window from GetWindowRect - GetClientRect, so both must
 * agree (a real client and a faked window shrank the window at every change). */
static void shim_GetWindowRect(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    RECT* r = (RECT*)(uintptr_t)ARG(1);
    DD* d = softdd_dd();
    if (w && w == g_game_wnd && d && d->w && r) {
        r->left = 0, r->top = 0, r->right = d->w, r->bottom = d->h;
        g_eax = TRUE;
    } else { BOOL ok; UNLOCKED(ok = GetWindowRect(w, r)); g_eax = ok; }
    g_esp += 4 + 2 * 4;
}

/* The game's cursor, kept: after the pointer leaves the window and comes
 * back, Windows asks WM_SETCURSOR and the default answer is the class arrow. */
static void shim_GetClientRect(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    RECT* r = (RECT*)(uintptr_t)ARG(1);
    DD* d = softdd_dd();
    if (w && w == g_game_wnd && d && d->w && r) {
        r->left = 0, r->top = 0, r->right = d->w, r->bottom = d->h;
        g_eax = TRUE;
    } else g_eax = GetClientRect(w, r);
    g_esp += 4 + 2 * 4;
}

/* The game moves and sizes its own window at every mode change (to the mode,
 * as a fullscreen mode would be). The window is the player's, and softdd's
 * (fit_window: the height kept, the new mode's shape; or fullscreen): the
 * game's moves of it are let through without the move and the size. */
static void shim_MoveWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    BOOL ok = TRUE;
    if (w && w == g_game_wnd) {
        static int once;
        if (!once++) printf("[host] the game's MoveWindow of its window: left to softdd\n");
    } else UNLOCKED(ok = MoveWindow(w, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (BOOL)ARG(5)));
    g_eax = ok;
    g_esp += 4 + 6 * 4;
}
static void shim_SetWindowPos(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    UINT flags = ARG(6);
    BOOL ok;
    if (w && w == g_game_wnd) flags |= SWP_NOMOVE | SWP_NOSIZE;   /* z-order, showing, the frame: still the game's */
    UNLOCKED(ok = SetWindowPos(w, (HWND)(uintptr_t)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), flags));
    g_eax = ok;
    g_esp += 4 + 7 * 4;
}

static void shim_SetCursor(void) {
    HCURSOR c = (HCURSOR)(uintptr_t)ARG(0), prev;
    g_game_cursor = c;
    g_cursor_known = 1;
    prev = SetCursor(c);
    g_eax = (uint32_t)(uintptr_t)prev;
    g_esp += 4 + 1 * 4;
}

static void shim_ScreenToClient(void) { g_eax = TRUE; g_esp += 4 + 2 * 4; }
static void shim_ClientToScreen(void) { g_eax = TRUE; g_esp += 4 + 2 * 4; }

/* LoadIconA/LoadCursorA with the game's own instance come from the
 * executable on disk, loaded as data: the mapped image is not a loaded module. */
static HMODULE game_resources(void) {
    static HMODULE h;
    if (!h) h = LoadLibraryExA(g_guest_exe, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    return h;
}
static void shim_LoadIconA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    LPCSTR name = (LPCSTR)(uintptr_t)ARG(1);
    HICON h = NULL;
    if ((uint32_t)(uintptr_t)inst == IMAGE_BASE) { if (game_resources()) h = LoadIconA(game_resources(), name); if (!h) h = LoadIconA(NULL, IDI_APPLICATION); }
    else h = LoadIconA(inst, name);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 2 * 4;
}
static void shim_LoadCursorA(void) {
    HINSTANCE inst = (HINSTANCE)(uintptr_t)ARG(0);
    LPCSTR name = (LPCSTR)(uintptr_t)ARG(1);
    HCURSOR h = NULL;
    if ((uint32_t)(uintptr_t)inst == IMAGE_BASE) { if (game_resources()) h = LoadCursorA(game_resources(), name); if (!h) h = LoadCursorA(NULL, IDC_ARROW); }
    else h = LoadCursorA(inst, name);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 2 * 4;
}

static native32_shim_t g_shims[] = {
    { "LoadIconA", shim_LoadIconA },
    { "LoadCursorA", shim_LoadCursorA },
    { "GetCursorPos", shim_GetCursorPos },
    { "SetCursorPos", shim_SetCursorPos },
    { "ScreenToClient", shim_ScreenToClient },
    { "GetWindowRect", shim_GetWindowRect },
    { "GetClientRect", shim_GetClientRect },
    { "MoveWindow", shim_MoveWindow },
    { "SetWindowPos", shim_SetWindowPos },
    { "SetCursor", shim_SetCursor },
    { "ClientToScreen", shim_ClientToScreen },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetKeyState", shim_GetKeyState },
    { "DirectDrawCreate", shim_DirectDrawCreate },
    { "GetForegroundWindow", shim_GetForegroundWindow },
    { "GetActiveWindow", shim_GetActiveWindow },
    { "ShowWindow", shim_ShowWindow },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "MCIWndCreateA", shim_MCIWndCreateA },
    { "ICInfo", shim_ICInfo },
    { "CreateFileA", shim_CreateFileA },
    { "OpenFile", shim_OpenFile },
    { "MessageBoxA", shim_MessageBoxA },
    { "OutputDebugStringA", shim_OutputDebugStringA },
    { "CreateMutexA", shim_CreateMutexA },
    { "ExitProcess", shim_ExitProcess },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "LoadStringA", widescreen_LoadStringA },
    { "timeGetTime", framerate_timeGetTime },
    { "PeekMessageA", framerate_PeekMessageA },
};

/* ---- muted by default -------------------------------------------------------
 * Test runs are silent unless --sound. */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

/* 1: muted; 2: there is no audio device; 0: failed. */
static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) { IMMDeviceEnumerator_Release(en); return 2; }
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* ---- reports ---------------------------------------------------------------- */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    return widescreen_lookup(va);
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X  (called from 0x%08X)\n", va, g_cur_func);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and only
 * real faults get here. */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    softdd_finish();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, softdd_frames);
    native32_dump_icalls(8);
    recomp_trace_flush();
    softdd_finish();
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* exe = "work\\Battlegrounds.exe";
    const char* game = "game";
    const char* cd = NULL;
    char exe_full[MAX_PATH], game_full[MAX_PATH], guest_args[512] = "";
    int run = 0, sound = 0;
    setvbuf(stdout, NULL, _IONBF, 0);   /* a fault ends the run with TerminateProcess */
    /* --headless runs again on a private desktop (native32_headless): there the
     * window can be visible, so it gets WM_PAINT, which the game's dialogs
     * draw from; muted unless PCRECOMP_MUTE says otherwise. */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--headless")) {
            int rc;
            if (!GetEnvironmentVariableA("PCRECOMP_MUTE", NULL, 0)) SetEnvironmentVariableA("PCRECOMP_MUTE", "1");
            if ((rc = native32_headless()) >= 0) return rc;
            g_hide = !GetEnvironmentVariableA("NATIVE32_HEADLESS", NULL, 0);
        }
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if ((n = framerate_arg(argc, argv, i))) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = softdd_headless = present_noclip = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec[MAX_PATH];   /* the game runs in its own folder */
            GetFullPathNameA(argv[++i], MAX_PATH, rec, NULL);
            softdd_record = rec;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) softdd_stop_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) softdd_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            if ((present_filter = present_filter_by_name(argv[++i])) < 0) { fprintf(stderr, "--filter: sharp, smooth, nearest or integer\n"); return 1; }
        }
        else if (!strcmp(argv[i], "--fullscreen")) present_fullscreen_start = 1;
        else if (!strcmp(argv[i], "--refresh") && i + 1 < argc) present_refresh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--present-log")) present_log = 1;
        else if (!strcmp(argv[i], "--files")) g_log_files = 1;
        else if (!strcmp(argv[i], "--multi")) g_multi = 1;
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc && g_shots < 16) {
            g_shot_s[g_shots] = strtoul(argv[++i], NULL, 0);
            GetFullPathNameA(argv[++i], MAX_PATH, g_shot_file[g_shots++], NULL);
        }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cd = argv[++i];
        else if (!strcmp(argv[i], "--films") && i + 1 < argc) {
            static char films[MAX_PATH];   /* the game runs in its own folder */
            GetFullPathNameA(argv[++i], MAX_PATH, films, NULL);
            movie_dir = films;
        }
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) _snprintf(guest_args, sizeof guest_args - 1, " %s", argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) parse_keys(argv[++i]);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) { if (!widescreen_parse(argv[++i])) return 1; }
        else {
            printf("usage: swgbrecomp [--run] [--headless] [--record out.mp4] [--frames N] [--sound]\n"
                   "                  [--shot S file.bmp] [--keys SCRIPT] [--watchdog S] [--files] [--multi]\n"
                   "                  [--exe work\\Battlegrounds.exe] [--game game] [--cd E] [--films DIR]\n"
                   "                  [--res WxH|desktop] [--fps N] [--fps-log]\n"
                   "                  [--scale N] [--filter sharp|smooth|nearest|integer] [--fullscreen]\n"
                   "                  [--refresh HZ] [--present-log]\n"
                   "                  [--args \"<game switches>\"] [--callbacks] [--native-trace]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    cd_pick(cd);
    if (g_multi && run) reg_isolate(game_full);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\Battlegrounds.exe", game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s", g_guest_exe, guest_args);

    native32_init();
    /* __try/__except frames: RaiseException goes to the guest's own chain
     * (pcrecomp seh32.c); the lift gives each __try function its frame. */
    native32_guest_seh = 1;
    coverage_init();    /* an unresolved indirect call stops the run there, loudly */
    AddVectoredExceptionHandler(0, crash);
    printf("Galactic Battlegrounds recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, IMAGE_BASE, IMAGE_BASE + span);
    SetDllDirectoryA(game_full);   /* language.dll */
    if (native32_bind(IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", swgb_entry_va);
        return 0;
    }
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    CoInitialize(NULL);
    if (!g_nsteps && !g_watchdog_s && !g_headless) sound = 1;   /* playing: sound on */
    int m = sound ? 1 : mute_process();
    if (!m) {
        fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
        return 1;
    }
    printf("  audio: %s\n", sound ? "on" : m == 2 ? "muted (no audio device)" : "muted (a test run; --sound to hear it)");
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    framerate_start();
    if (g_shots) CloseHandle(CreateThread(NULL, 0, shot_thread, NULL, 0, NULL));
    g_file_seen = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_nsteps) CloseHandle(CreateThread(NULL, 0, keys_thread, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", swgb_entry_va);
    native32_call_guest(swgb_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    softdd_finish();
    return (int)g_eax;
}
