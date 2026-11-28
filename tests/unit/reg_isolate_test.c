/* src/runtime/reg_isolate.c without the game: per-folder HKCU hive seeding
 * preserves existing private values while seeding missing ones, and never
 * mutates the source key. Uses only an isolated scratch subkey under HKCU.
 *
 *   py -3 tools\test.py --level unit        (builds and runs every unit)
 */
#include "reg_isolate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static LONG set_dword(HKEY root, const char* sub, const char* name, DWORD val) {
    HKEY k = NULL;
    LONG r = RegCreateKeyExA(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof val);
    RegCloseKey(k);
    return r;
}

static LONG set_sz(HKEY root, const char* sub, const char* name, const char* val) {
    HKEY k = NULL;
    LONG r = RegCreateKeyExA(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)strlen(val) + 1);
    RegCloseKey(k);
    return r;
}

static DWORD get_dword(HKEY root, const char* sub, const char* name, DWORD fallback) {
    HKEY k = NULL;
    DWORD val = fallback, sz = sizeof val, type = 0;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return fallback;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE*)&val, &sz) != ERROR_SUCCESS || type != REG_DWORD)
        val = fallback;
    RegCloseKey(k);
    return val;
}

static int get_sz(HKEY root, const char* sub, const char* name, char* out, DWORD out_sz) {
    HKEY k = NULL;
    DWORD type = 0, sz = out_sz;
    if (RegOpenKeyExA(root, sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExA(k, name, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_SZ;
}

int main(void) {
    char scratch[128], hive_key[192], missing_key[192], preset_key[192];
    static const char src_sub[] = "Software\\LucasArts Entertainment Company LLC";
    static const char game_sub[] = "Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0";
    char san[MAX_PATH + 64], strbuf[128];
    HKEY root = NULL, hive = NULL;

    _snprintf(scratch, sizeof scratch - 1, "Software\\swgb-recomp\\unit-test-%lu", GetCurrentProcessId());
    scratch[sizeof scratch - 1] = 0;
    RegDeleteTreeA(HKEY_CURRENT_USER, scratch);

    /* 1. Path sanitization */
    reg_hive_subkey("A:/Games/star/games/foo-base-0", san, sizeof san);
    CHECK(strcmp(san, "Software\\swgb-recomp\\hives\\A__Games_star_games_foo-base-0") == 0);

    CHECK(RegCreateKeyExA(HKEY_CURRENT_USER, scratch, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &root, NULL) == ERROR_SUCCESS);
    CHECK(set_dword(root, game_sub, "Screen Size", 1280) == ERROR_SUCCESS);
    CHECK(set_dword(root, game_sub, "Music Volume", 5600) == ERROR_SUCCESS);
    CHECK(set_sz(root, game_sub, "Default SWGB Multiplayer Service", "Local (LAN) TCP/IP Connection") == ERROR_SUCCESS);

    /* 2. Fresh hive gets all source keys and values */
    _snprintf(hive_key, sizeof hive_key - 1, "hives\\fresh");
    CHECK(reg_prepare_hive(root, hive_key, src_sub, &hive) == ERROR_SUCCESS);
    CHECK(get_dword(hive, game_sub, "Screen Size", 0) == 1280);
    CHECK(get_dword(hive, game_sub, "Music Volume", 0) == 5600);
    CHECK(get_sz(hive, game_sub, "Default SWGB Multiplayer Service", strbuf, sizeof strbuf) &&
          strcmp(strbuf, "Local (LAN) TCP/IP Connection") == 0);

    /* 3. Subsequent launches preserve modified private Screen Size even if source stays 1280,
     * while seeding newly added missing values and leaving source untouched. */
    CHECK(set_dword(hive, game_sub, "Screen Size", 800) == ERROR_SUCCESS);
    CHECK(set_dword(root, game_sub, "Sound Volume", 3100) == ERROR_SUCCESS);
    RegCloseKey(hive);
    hive = NULL;

    CHECK(reg_prepare_hive(root, hive_key, src_sub, &hive) == ERROR_SUCCESS);
    CHECK(get_dword(hive, game_sub, "Screen Size", 0) == 800);
    CHECK(get_dword(hive, game_sub, "Music Volume", 0) == 5600);
    CHECK(get_dword(hive, game_sub, "Sound Volume", 0) == 3100);
    CHECK(get_dword(root, game_sub, "Screen Size", 0) == 1280);
    RegCloseKey(hive);
    hive = NULL;

    /* 4. A pre-seeded hive (e.g. Screen Size = 800 written by ensure_game) keeps 800
     * and still inherits all other values from source on first launch. */
    _snprintf(preset_key, sizeof preset_key - 1, "hives\\preseeded");
    CHECK(set_dword(root, "hives\\preseeded\\Software\\LucasArts Entertainment Company LLC\\Star Wars Galactic Battlegrounds\\1.0",
                    "Screen Size", 800) == ERROR_SUCCESS);
    CHECK(reg_prepare_hive(root, preset_key, src_sub, &hive) == ERROR_SUCCESS);
    CHECK(get_dword(hive, game_sub, "Screen Size", 0) == 800);
    CHECK(get_dword(hive, game_sub, "Music Volume", 0) == 5600);
    CHECK(get_dword(hive, game_sub, "Sound Volume", 0) == 3100);
    CHECK(get_dword(root, game_sub, "Screen Size", 0) == 1280);
    RegCloseKey(hive);
    hive = NULL;

    /* 5. Missing source key still yields a valid empty hive */
    _snprintf(missing_key, sizeof missing_key - 1, "hives\\nosrc");
    CHECK(reg_prepare_hive(root, missing_key, "Software\\NoSuchSourceKey", &hive) == ERROR_SUCCESS);
    RegCloseKey(hive);

    RegCloseKey(root);
    RegDeleteTreeA(HKEY_CURRENT_USER, scratch);

    if (fails) return 1;
    printf("OK\n");
    return 0;
}
