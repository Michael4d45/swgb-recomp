#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

void reg_hive_subkey(const char* game_full, char* out, size_t out_sz);
LONG reg_seed_missing(HKEY src, HKEY dst);
LONG reg_prepare_hive(HKEY root, const char* hive_subkey, const char* src_subkey, HKEY* out_hive);
int reg_isolate(const char* game_full);
