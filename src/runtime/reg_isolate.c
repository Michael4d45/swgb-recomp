#include "reg_isolate.h"
#include <stdio.h>
#include <stdlib.h>

void reg_hive_subkey(const char* game_full, char* out, size_t out_sz) {
    char name[MAX_PATH];
    int n = 0;
    if (!out || !out_sz) return;
    for (const char* p = game_full; p && *p && n < MAX_PATH - 1; p++)
        name[n++] = (*p == '\\' || *p == '/' || *p == ':') ? '_' : *p;
    name[n] = 0;
    _snprintf(out, out_sz - 1, "Software\\swgb-recomp\\hives\\%s", name);
    out[out_sz - 1] = 0;
}

/* Copy only missing keys and values from src to dst so settings persisted in a
 * test folder's private hive (e.g. Screen Size = 800) survive later launches. */
LONG reg_seed_missing(HKEY src, HKEY dst) {
    DWORD nkeys = 0, max_key = 0, nvals = 0, max_val = 0, max_data = 0;
    LONG err = RegQueryInfoKeyA(src, NULL, NULL, NULL, &nkeys, &max_key, NULL,
                                &nvals, &max_val, &max_data, NULL, NULL);
    if (err != ERROR_SUCCESS) return err;

    DWORD key_cap = max_key + 1;
    DWORD val_cap = max_val + 1;
    DWORD data_cap = max_data + 1;
    char* sub_name = (char*)malloc(key_cap);
    char* val_name = (char*)malloc(val_cap);
    BYTE* data = (BYTE*)malloc(data_cap);
    if (!sub_name || !val_name || !data) {
        free(sub_name);
        free(val_name);
        free(data);
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    for (DWORD i = 0; i < nvals; i++) {
        DWORD vn_len = val_cap, type = 0, d_len = data_cap;
        LONG r = RegEnumValueA(src, i, val_name, &vn_len, NULL, &type, data, &d_len);
        if (r != ERROR_SUCCESS) {
            if (err == ERROR_SUCCESS) err = r;
            continue;
        }
        LONG q = RegQueryValueExA(dst, val_name, NULL, NULL, NULL, NULL);
        if (q == ERROR_FILE_NOT_FOUND) {
            r = RegSetValueExA(dst, val_name, 0, type, data, d_len);
            if (r != ERROR_SUCCESS && err == ERROR_SUCCESS) err = r;
        } else if (q != ERROR_SUCCESS && err == ERROR_SUCCESS) {
            err = q;
        }
    }

    for (DWORD i = 0; i < nkeys; i++) {
        DWORD sn_len = key_cap;
        LONG r = RegEnumKeyExA(src, i, sub_name, &sn_len, NULL, NULL, NULL, NULL);
        if (r != ERROR_SUCCESS) {
            if (err == ERROR_SUCCESS) err = r;
            continue;
        }
        HKEY src_sub = NULL, dst_sub = NULL;
        r = RegOpenKeyExA(src, sub_name, 0, KEY_READ, &src_sub);
        if (r != ERROR_SUCCESS) {
            if (err == ERROR_SUCCESS) err = r;
            continue;
        }
        r = RegCreateKeyExA(dst, sub_name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dst_sub, NULL);
        if (r == ERROR_SUCCESS) {
            r = reg_seed_missing(src_sub, dst_sub);
            RegCloseKey(dst_sub);
        }
        RegCloseKey(src_sub);
        if (r != ERROR_SUCCESS && err == ERROR_SUCCESS) err = r;
    }

    free(sub_name);
    free(val_name);
    free(data);
    return err;
}

LONG reg_prepare_hive(HKEY root, const char* hive_subkey, const char* src_subkey, HKEY* out_hive) {
    HKEY hive = NULL, src = NULL, dst = NULL;
    LONG r = RegCreateKeyExA(root, hive_subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hive, NULL);
    if (r != ERROR_SUCCESS) return r;

    r = RegOpenKeyExA(root, src_subkey, 0, KEY_READ, &src);
    if (r == ERROR_SUCCESS) {
        r = RegCreateKeyExA(hive, src_subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &dst, NULL);
        if (r == ERROR_SUCCESS) {
            r = reg_seed_missing(src, dst);
            RegCloseKey(dst);
        }
        RegCloseKey(src);
    } else if (r == ERROR_FILE_NOT_FOUND) {
        r = ERROR_SUCCESS;
    }

    if (r != ERROR_SUCCESS) {
        RegCloseKey(hive);
        return r;
    }
    *out_hive = hive;
    return ERROR_SUCCESS;
}

int reg_isolate(const char* game_full) {
    char key[MAX_PATH + 64];
    HKEY hive = NULL;
    reg_hive_subkey(game_full, key, sizeof key);
    if (reg_prepare_hive(HKEY_CURRENT_USER, key, "Software\\LucasArts Entertainment Company LLC", &hive) != ERROR_SUCCESS)
        return 0;
    LONG r = RegOverridePredefKey(HKEY_CURRENT_USER, hive);
    RegCloseKey(hive);
    return r == ERROR_SUCCESS;
}
