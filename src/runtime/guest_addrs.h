/*
 * guest_addrs.h: where the engine keeps what the host reads or hooks, per exe.
 *
 * The host runs Battlegrounds.exe v1.1 (swgbrecomp.exe) or Clone Campaigns'
 * battlegrounds_x1.exe (swgbrecomp_cc.exe, SWGB_CC). The expansion is the same
 * engine with code added: its functions moved, and two classes grew. Each x1
 * value was found from the base game's (tools/x1_addrs.py; docs/lifting.md,
 * "Clone Campaigns"): virtual functions by their slot in the matching vtable,
 * others by their bytes and their call sites, struct offsets by aligning every
 * method of the class in both exes and reading the displacement each
 * instruction uses.
 *
 * Offsets that are the same in both are written in place in the files that
 * use them: the game object's +0x24 (program info), +0x1DC (mode), +0x420
 * (world), +0x17B4 (game screen); program info's +0x888 and +0x8E4..+0x8FC;
 * TPanel's +0x0C..+0x58; the draw area's +0x08, +0x18, +0x20; the unit-info
 * panel's +0xF8.
 */
#pragma once

#ifdef SWGB_CC
/* ---- battlegrounds_x1.exe (Clone Campaigns, 2002-05-29) ---- */
#define G_GAME              0x006A3684u   /* the game object pointer */
#define G_COMM              0x006A35E0u   /* the communications handler (multiplayer) */
#define G_CODE_END          0x00631D70u   /* game code below, the CRT above */
#define G_SCREEN_VTABLE     0x0065FE48u   /* the game screen; constructor sub_004F2BA0 */
#define G_SCREEN_GATE       0x1270        /* ms between passes (base +0x1258) */
#define G_SCREEN_LAST       0x1274        /* timeGetTime of the last pass (base +0x125C) */
#define G_SCREEN_IFACE      0x10BC        /* nonzero once the interface exists (base +0x10A4) */
#define G_SCREEN_ART0       0x100C        /* the interface art shapes */
#define G_SCREEN_ART1       0x10A4        /* base +0x108C */
#define G_SCREEN_ART2       0x10B8        /* base +0x10A0 */
/* Hooked or called functions: VA and lifted symbol. */
#define G_SET_SCREEN_SIZE_VA  0x005F1030u   /* game vtable 0x00668B10 + 0x68 */
#define G_SET_SCREEN_SIZE     sub_005F1030
#define G_SCREEN_SIZE_VA      0x004F6600u   /* screen vtable + 0x5C, handle_size */
#define G_SCREEN_SIZE         sub_004F6600
#define G_SCREEN_DRAW_VA      0x00500230u   /* screen vtable + 0x3C */
#define G_SCREEN_DRAW         sub_00500230
#define G_SCREEN_PAINT_VA     0x004F7E60u   /* screen vtable + 0x60 */
#define G_SCREEN_PAINT        sub_004F7E60
#define G_SCREEN_DELETE_VA    0x004F5A30u   /* screen vtable + 0x00 */
#define G_SCREEN_DELETE       sub_004F5A30
#define G_INFO_DRAW_VA        0x005D98A0u   /* unit-info panel vtable 0x006680D8 + 0x3C */
#define G_INFO_DRAW           sub_005D98A0
#define G_RESOURCES_DRAW_VA   0x005D84D0u   /* resource counters vtable 0x00667EA8 + 0x3C */
#define G_RESOURCES_DRAW      sub_005D84D0
#define G_GAME_INTERFACE_VA   0x005E68B0u   /* game vtable 0x00668B10 + 0x14 */
#define G_GAME_INTERFACE      sub_005E68B0
#define G_PANEL_SET_RECT      0x004B5BB0u   /* TPanel vtable + 0x0C */
#define G_AREA_FILL           0x00474160u
#define G_AREA_LOCK           0x00472A70u
#define G_AREA_UNLOCK         0x00472B20u
#define G_OBJ_PASS            0x00496E80u   /* base 0x00496470 */
#define G_OBJ_DRAW_VA         0x0054C9E0u   /* base 0x0054AD10 */
#define G_OBJ_DRAW            sub_0054C9E0
#define G_OBJ_DRAW2_VA        0x00557F60u   /* base 0x00556040 (vtable 0x0066311C) */
#define G_OBJ_DRAW2           sub_00557F60
#define G_OBJ_ID              0x10
#define G_WORLD_STEP_T        0x20          /* the world update's step, 0x0061E946 */
#define G_MAP_HALF_W          0xBF40        /* new_location (0x0054F820) matches the base byte for byte */
#define G_MAP_HALF_H          0xBF3E
#else
/* ---- Battlegrounds.exe v1.1 (2001-11-28) ---- */
#define G_GAME              0x006A0FD4u
#define G_COMM              0x006A0F30u   /* the communications handler; its out-of-sync check is sub_00440260 */
#define G_CODE_END          0x0062E000u
#define G_SCREEN_VTABLE     0x0065CE24u   /* constructor sub_004F1820 */
#define G_SCREEN_GATE       0x1258
#define G_SCREEN_LAST       0x125C
#define G_SCREEN_IFACE      0x10A4
#define G_SCREEN_ART0       0x100C
#define G_SCREEN_ART1       0x108C
#define G_SCREEN_ART2       0x10A0
#define G_SET_SCREEN_SIZE_VA  0x005ED2D0u   /* game vtable 0x00665A20 + 0x68 */
#define G_SET_SCREEN_SIZE     sub_005ED2D0
#define G_SCREEN_SIZE_VA      0x004F5210u   /* screen vtable + 0x5C, handle_size */
#define G_SCREEN_SIZE         sub_004F5210
#define G_SCREEN_DRAW_VA      0x004FEDB0u   /* screen vtable + 0x3C */
#define G_SCREEN_DRAW         sub_004FEDB0
#define G_SCREEN_PAINT_VA     0x004F6A70u   /* screen vtable + 0x60 */
#define G_SCREEN_PAINT        sub_004F6A70
#define G_SCREEN_DELETE_VA    0x004F46B0u   /* screen vtable + 0x00 */
#define G_SCREEN_DELETE       sub_004F46B0
#define G_INFO_DRAW_VA        0x005D5D00u   /* unit-info panel vtable 0x00665088 + 0x3C */
#define G_INFO_DRAW           sub_005D5D00
#define G_RESOURCES_DRAW_VA   0x005D4930u   /* resource counters vtable 0x00664E58 + 0x3C */
#define G_RESOURCES_DRAW      sub_005D4930
#define G_GAME_INTERFACE_VA   0x005E2C50u   /* game vtable 0x00665A20 + 0x14 */
#define G_GAME_INTERFACE      sub_005E2C50
#define G_PANEL_SET_RECT      0x004B5660u   /* TPanel vtable + 0x0C */
#define G_AREA_FILL           0x004736C0u
#define G_AREA_LOCK           0x00471FD0u
#define G_AREA_UNLOCK         0x00472080u
/* --smooth (smooth.c): the map view's object pass and the objects' draw. */
#define G_OBJ_PASS            0x00496470u   /* per tile: each object's vtable +0x70 */
#define G_OBJ_DRAW_VA         0x0054AD10u   /* RGE_Static_Object draw(area, x, y, colors) */
#define G_OBJ_DRAW            sub_0054AD10
#define G_OBJ_DRAW2_VA        0x00556040u   /* vtable 0x006600E0's draw, which ends in the one above */
#define G_OBJ_DRAW2           sub_00556040
#define G_OBJ_ID              0x10          /* the object's id */
#define G_WORLD_STEP_T        0x20          /* timeGetTime of the world's last step */
#define G_MAP_HALF_W          0xBF40        /* a tile's half width and half height, pixels */
#define G_MAP_HALF_H          0xBF3E
#endif
