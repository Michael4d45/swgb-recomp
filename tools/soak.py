#!/usr/bin/env python3
"""Long headless games against the computer, to find what a short one never runs.

    py -3 tools/soak.py [--mode skirmish|monkey|campaign] [--runs N] [--minutes M]
                        [--jobs J] [--seed S] [--run EXE] [--game DIR] [--out DIR]
                        [--cc] [--civs A,B] [--my-civ C]

--cc plays Clone Campaigns (swgbrecomp_cc.exe, battlegrounds_x1.exe): its
Single Player menu (Standard Game and Saved Game a row lower, the campaigns
are Episode II's: Confederacy and Galactic Republic) and its nine
civilizations. --civs gives the computer players' civilizations in turn
instead of drawn ones, --my-civ the player's (e.g. --civs
Republic,Confederacy --my-civ Republic).

Every run is `--headless --multi` (never a window, never the user's game):

  skirmish  Single Player -> Standard Game, with the setup varied per run from
            the seed (1-7 computer players, their civilizations, the
            location, map size and difficulty), Start Game, then an idle
            player's few habits: the command center trains workers, the view
            jumps home, a shot every five minutes. The computers play.
  monkey    the same start, then random input over the whole screen: clicks
            and right-clicks on the map, the command grid, the HUD's
            buttons (the game menu, objectives, chat, diplomacy), the
            minimap, letter and function keys, Escape and Enter. The menu's
            Quit, Restart and Load are never clicked on purpose.
  campaign  Campaigns -> one of the six campaigns -> its first mission ->
            OK, the briefing, Escape for the Objectives, then the monkey (a
            first mission has no command center for the idle habits;
            --idle-campaign for them anyway).

In every mode the game speed is set to Fast (--speed) and the game is saved
once, 40% of the way in (--no-save).

Each run's log, its --keys script and its shots go to OUT/<run>/; the
summary prints every unresolved indirect call, fault and not-lifted stub
with the keys that came before it, and how long the game ran. Exit codes of
the host: 4 the watchdog ended it (the run survived), 0 the game exited,
2 a not-lifted stub, 3 a fault, 5 an unresolved indirect call (coverage.c).

Then `py -3 tools/seeds_from_logs.py` turns the unresolved addresses into
seeds (it reads work/soak/**/*.log).

Coordinates are the 800x600 screens (the menus, the setup screen, the HUD):
a --game folder whose Screen Size is 1024 or 1280 is moved off by the HUD's
layout, so the tool checks the first SetDisplayMode in the game and warns.
"""
import argparse
import datetime
import json
import os
import random
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- the screens, in 800x600 game pixels ------------------------------------
MAIN_SINGLE = (345, 250)            # main menu: Single Player
SP_STANDARD = (600, 168)            # Single Player: Standard Game
SP_CAMPAIGNS = (603, 112)           # Single Player: Campaigns
SETUP_START = (118, 568)            # Standard Game: Start Game
# The setup screen's drop-downs: the arrow, then the first item and the step.
DD_OTHERS = ((770, 211), (690, 236), 21)        # 1..7 computer players
DD_LOCATION = ((770, 124), (680, 149), 21)      # Forced Deployment .. Shoreline (16 shown)
DD_SIZE = ((770, 153), (690, 178), 21)          # Tiny, Small, Medium, Normal, Large, Giant
DD_DIFFICULTY = ((770, 182), (690, 207), 21)    # Easiest .. Hardest
DD_REVEAL = ((770, 297), (690, 322), 21)        # Normal, Explored, All Visible
CIV_ROW0, CIV_ROW_STEP = 169, 24                # computer player rows; the arrow at x 249
CIV_ITEMS = 7                                   # 6 civilizations and Random, x 290, from row + 25
LOCATIONS = ['Forced Deployment', 'Forest', 'Fortress', 'Ice Lake', 'Islands', 'LandMass', 'Large Sea',
             'Motherlode', 'Nova Assault', 'Nova Lake', 'Precipice', 'Raiders', 'Rivers', 'Savannah',
             'Sea', 'Shoreline']
SIZES = ['Tiny', 'Small', 'Medium', 'Normal', 'Large', 'Giant']
DIFFICULTIES = ['Easiest', 'Easy', 'Moderate', 'Hard', 'Hardest']
CIVS = ['Galactic Empire', 'Gungans', 'Rebel Alliance', 'Royal Naboo', 'Trade Federation', 'Wookiees',
        'Random']
# Campaigns: the six planets, then the first mission and the difficulty dialog's OK.
CAMPAIGNS = {'Attichitcuk': (160, 322), 'OOM-9': (187, 212), 'Boss Nass': (272, 118),
             'Darth Vader': (512, 118), 'Princess Leia': (595, 212), 'Chewbacca': (625, 322)}
CAMPAIGN_MISSION1 = (150, 140)
CAMPAIGN_OK = (318, 481)             # the difficulty dialog (Easy is chosen)
OBJECTIVES_OK = (589, 502)           # the mission's opening Objectives (Escape closes it too)
# In the game (the 800x600 HUD).
MAP_VIEW = (0, 20, 799, 425)        # the map, under the resource bar's row and above the panel
COMMAND_GRID = [(x, y) for y in (452, 495, 538, 577) for x in (25, 70, 115, 152)]
HUD_RIGHT = {'menu': (767, 478), 'objectives': (767, 501), 'chat': (767, 525),
             'diplomacy': (767, 550), 'last': (767, 576)}
MINIMAP = (585, 515, 120)           # centre and half-width of the diamond
MINIMAP_BUTTONS = [(664, 480), (692, 480), (470, 565)]
TOP_ICONS = [(190, 441), (212, 441), (233, 441), (254, 441)]
GAME_MENU = {'save': (408, 218), 'options': (408, 317), 'about': (408, 350), 'cancel': (408, 390)}
OPTIONS_FAST, OPTIONS_OK = (135, 248), (305, 455)     # the game menu's Options: Speed
SAVE_OK = (178, 568)                 # the Save Game screen; the name field has the focus
SP_SAVED = (603, 286)                # Single Player: Saved Game
SAVED_ROW0, SAVED_STEP, SAVED_OK = 121, 21, (178, 568)   # its list (alphabetical) and OK
TEAM_X, TEAM_ME_Y = 472, 94          # the setup screen's Team buttons: each click is the next team
SCORE_TABS = [(130, 562), (238, 562), (346, 562), (454, 562), (562, 562), (672, 562)]
SCORE_MENU = (710, 514)              # the score screen: Main Menu (Scenario Menu in a campaign)
# OOM-9's missions on its campaign screen, in order (each appears when the one before is won).
OOM9_MISSIONS = [(150, 140), (372, 140), (135, 265), (368, 265), (90, 430)]
WIN_CHEAT = 'skywalker'              # typed in the chat: instant victory
COMMAND_CENTER = (400, 170)         # where the view starts: the command center
FORCED_CIVS = []                    # --civs: the computers' civilizations, in turn
MY_CIV = [None]                     # --my-civ
MY_CIV_ROW = 94                     # the player's row on the setup screen


def clone_campaigns():
    """--cc: Clone Campaigns' screens. Its Single Player menu has Episode
    I,IV,V,VI Campaigns (the base game's six, at SP_CAMPAIGNS), Episode II
    Campaigns, Standard Game, Custom Campaign, Saved Game; its setup screen is
    the base game's with nine civilizations; its Episode II campaign screen has
    two; the mission screen and difficulty dialog are the base game's."""
    global SP_STANDARD, SP_CAMPAIGNS, SP_SAVED, CIVS, CIV_ITEMS, CAMPAIGNS
    SP_STANDARD, SP_CAMPAIGNS, SP_SAVED = (603, 228), (603, 170), (603, 344)
    CIVS = ['Confederacy', 'Galactic Empire', 'Gungans', 'Rebel Alliance', 'Republic', 'Royal Naboo',
            'Trade Federation', 'Wookiees', 'Random']
    CIV_ITEMS = len(CIVS)
    CAMPAIGNS = {'Confederacy': (225, 315), 'Galactic Republic': (555, 315)}
MONKEY_KEYS = list('abcdefghijklmnopqrstuvwxyz0123456789') + \
    ['esc', 'enter', 'space', 'tab', 'up', 'down', 'left', 'right', 'home', 'end', 'bksp'] + \
    ['f1', 'f2', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f11']   # F3 pauses, F10 is the menu: kept off

# What the host prints when the game reaches code it cannot run.
EVENTS = re.compile(r'ICALL: unresolved|ITAIL: unresolved|=== fault|\[not-lifted\]|=== unresolved|'
                    r'\[coverage\]|\[eh32\] funclet .* not lifted|STUB:')
EXIT = {0: 'exited', 2: 'not-lifted', 3: 'fault', 4: 'survived (watchdog)', 5: 'unresolved icall'}


class Script:
    def __init__(self):
        self.steps = []

    def at(self, t, what):
        self.steps.append((round(t, 2), what))

    def click(self, t, xy, button='click'):
        self.at(t, '%s:%d:%d' % (button, xy[0], xy[1]))

    def text(self):
        return '\n'.join('%g:%s' % s for s in sorted(self.steps, key=lambda s: s[0]))


def pick(dd, k, s, t):
    """Open a drop-down and click its k-th item. Two seconds."""
    (ax, ay), (ix, iy), step = dd
    s.click(t, (ax, ay))
    s.click(t + 1.2, (ix, iy + step * k))
    return t + 2.5


def setup_skirmish(s, rnd, t):
    """Main menu -> Standard Game -> a varied setup -> Start Game. Returns (t, what)."""
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, SP_STANDARD)
    t += 33
    others = rnd.randint(1, 7)
    size = rnd.randint(max(0, (others + 1 + 1) // 2 - 1), 5)   # big enough for the players, more or less
    loc = rnd.randrange(len(LOCATIONS))
    diff = rnd.randrange(len(DIFFICULTIES))
    t = pick(DD_OTHERS, others - 1, s, t)
    t = pick(DD_LOCATION, loc, s, t)
    t = pick(DD_SIZE, size, s, t)
    t = pick(DD_DIFFICULTY, diff, s, t)
    civs = []
    if MY_CIV[0]:                                   # the player's own row, above the computers'
        k = CIVS.index(MY_CIV[0])
        s.click(t, (249, MY_CIV_ROW))
        s.click(t + 1.2, (290, MY_CIV_ROW + 25 + 21 * k))
        t += 2.5
    for p in range(others):
        k = CIVS.index(FORCED_CIVS[p % len(FORCED_CIVS)]) if FORCED_CIVS else rnd.randrange(CIV_ITEMS)
        civs.append(CIVS[k])
        y = CIV_ROW0 + CIV_ROW_STEP * p
        s.click(t, (249, y))
        s.click(t + 1.2, (290, y + 25 + 21 * k))
        t += 2.5
    s.click(t + 1, SETUP_START)
    what = {'others': others, 'location': LOCATIONS[loc], 'size': SIZES[size],
            'difficulty': DIFFICULTIES[diff], 'civs': civs}
    if MY_CIV[0]:
        what['my_civ'] = MY_CIV[0]
    return t + 1, what


def setup_campaign(s, rnd, t, name=None):
    name = name or rnd.choice(sorted(CAMPAIGNS))
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, SP_CAMPAIGNS)
    s.click(t + 30, CAMPAIGNS[name])
    s.click(t + 40, CAMPAIGN_MISSION1)
    s.click(t + 46, CAMPAIGN_OK)
    return t + 46, {'campaign': name, 'mission': 1}


def fast_speed(s, t):
    """The game menu -> Options -> Speed: Fast -> OK. Six seconds."""
    s.click(t, HUD_RIGHT['menu'])
    s.click(t + 2, GAME_MENU['options'])
    s.click(t + 4, OPTIONS_FAST)
    s.click(t + 5, OPTIONS_OK)
    return t + 6


def save_game(s, t, name):
    """The game menu -> Save -> a name -> OK. Saving runs the whole world through the writer."""
    s.click(t, HUD_RIGHT['menu'])
    s.click(t + 2, GAME_MENU['save'])
    for k, ch in enumerate(name):
        s.at(t + 5 + 0.3 * k, ch)
    s.click(t + 6 + 0.3 * len(name), SAVE_OK)
    return t + 20


def chat(s, t, words):
    """Enter, the words, Enter: the chat line, where the cheats go too."""
    s.at(t, 'enter')
    for k, ch in enumerate(words):
        s.at(t + 1 + 0.3 * k, 'space' if ch == ' ' else ch)
    s.at(t + 2 + 0.3 * len(words), 'enter')
    return t + 3 + 0.3 * len(words)


SHOT_DIR = ['.']                     # setup_fixed's shot of the setup screen: run_one sets it


def setup_fixed(s, t, others, size, diff, teams=None, loc=None, reveal=None):
    """Standard Game with a fixed setup. teams: team clicks per row, me first (0 = no team)."""
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, SP_STANDARD)
    t += 33
    t = pick(DD_OTHERS, others - 1, s, t)
    if loc is not None:
        t = pick(DD_LOCATION, loc, s, t)
    t = pick(DD_SIZE, size, s, t)
    t = pick(DD_DIFFICULTY, diff, s, t)
    if reveal is not None:
        t = pick(DD_REVEAL, reveal, s, t)
    # A click just after a drop-down closes, or a second one on a button the cursor has
    # not left, did not count (a 6-allies game started with me and the 7th on no team).
    t += 2
    for row, n in enumerate(teams or []):
        y = TEAM_ME_Y if row == 0 else CIV_ROW0 + CIV_ROW_STEP * (row - 1)
        for k in range(n):
            s.at(t, 'move:%d:%d' % (TEAM_X - 60, y))
            s.click(t + 0.7, (TEAM_X, y))
            t += 1.6
    s.at(t + 1, 'shot=%s' % os.path.join(SHOT_DIR[0], 'shot_setup.bmp'))
    t += 1
    s.click(t + 1, SETUP_START)
    return t + 1, {'others': others, 'size': SIZES[size], 'difficulty': DIFFICULTIES[diff], 'teams': teams,
                   'location': LOCATIONS[loc] if loc is not None else '(as left)'}


def score_tour(s, t, out, tag):
    """Each tab of the score screen, a shot of the first and the last."""
    s.at(t, 'shot=%s' % os.path.join(out, 'score_%s.bmp' % tag))
    for k, xy in enumerate(SCORE_TABS[1:] + SCORE_TABS[:1]):
        s.click(t + 2 + 2 * k, xy)
    s.at(t + 2 + 2 * len(SCORE_TABS), 'shot=%s' % os.path.join(out, 'score_%s_tabs.bmp' % tag))
    return t + 4 + 2 * len(SCORE_TABS)


def shots(s, t0, t1, out, every=300):
    for ts in range(int(t0) + 30, int(t1), every):
        s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))


def habits(s, rnd, t0, t1, shots, out):
    """An idle player: train a worker now and then, jump home, a shot every five minutes."""
    t = t0
    while t < t1:
        s.at(t, 'h')                                   # the command center, selected and centred
        s.click(t + 1, COMMAND_GRID[0])                # its first button: a worker
        s.click(t + 3, (rnd.randint(150, 650), rnd.randint(60, 400)), 'rclick')  # rally / gather point
        t += rnd.uniform(25, 45)


def monkey(s, rnd, t0, t1, out):
    """Random input over the game screen and the HUD."""
    t = t0
    while t < t1:
        r = rnd.random()
        if r < 0.30:
            s.click(t, (rnd.randint(*MAP_VIEW[0::2]), rnd.randint(*MAP_VIEW[1::2])))
        elif r < 0.45:
            s.click(t, (rnd.randint(*MAP_VIEW[0::2]), rnd.randint(*MAP_VIEW[1::2])), 'rclick')
        elif r < 0.62:
            s.click(t, rnd.choice(COMMAND_GRID))
        elif r < 0.66:
            cx, cy, w = MINIMAP
            s.click(t, (cx + rnd.randint(-w // 2, w // 2), cy + rnd.randint(-w // 4, w // 4)),
                    rnd.choice(['click', 'rclick']))
        elif r < 0.68:
            s.click(t, rnd.choice(MINIMAP_BUTTONS + TOP_ICONS))
        elif r < 0.71:
            # A HUD panel (objectives, chat, diplomacy, ...): a click or two inside it, then Escape.
            s.click(t, HUD_RIGHT[rnd.choice(['objectives', 'chat', 'diplomacy', 'last'])])
            for k in range(rnd.randint(0, 3)):
                s.click(t + 1.5 + k, (rnd.randint(150, 650), rnd.randint(120, 450)))
            s.at(t + 5, 'esc')
            t += 5
        elif r < 0.72:
            # The game menu: Options or About, then out again; never Quit, Restart or Load.
            s.click(t, HUD_RIGHT['menu'])
            s.click(t + 1.5, GAME_MENU[rnd.choice(['options', 'about', 'cancel'])])
            s.at(t + 5, 'esc')
            s.at(t + 6, 'esc')
            t += 6
        elif r < 0.95:
            s.at(t, rnd.choice(MONKEY_KEYS))
        else:
            s.at(t, 'h')                       # home: keeps the view where there is something to click
        t += rnd.uniform(0.4, 2.0)
    for ts in range(int(t0) + 30, int(t1), 300):
        s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))


def kill_tree(proc):
    """End this run's process tree, by its PID (Linux: its process group, started as its own)."""
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
    else:
        import signal
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass


def ensure_game(template, dst):
    """A game folder of its own: hard links to the template (as worktree_env.ps1), copies of
    what the game writes."""
    if os.path.isdir(dst):
        return
    for dirpath, _, files in os.walk(template):
        rel = os.path.relpath(dirpath, template)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        top = rel.split(os.sep)[0]
        for f in files:
            src, d = os.path.join(dirpath, f), os.path.join(dst, rel, f)
            if top in ('SaveGame', 'Scenario') or (rel == '.' and f.lower().startswith('player.nf')):
                shutil.copy2(src, d)
            else:
                os.link(src, d)


def analyse(log, keys_text):
    text = open(log, encoding='utf-8', errors='replace').read()
    lines = text.splitlines()
    keyed = [(i, l) for i, l in enumerate(lines) if l.startswith('[keys]')]
    events = []
    for i, l in enumerate(lines):
        if EVENTS.search(l):
            before = [k for j, k in keyed if j < i][-8:]
            events.append({'line': l.strip(), 'context': [x.rstrip() for x in lines[i:i + 30]],
                           'keys_before': before})
    m = re.search(r'\[watchdog\] (\d+) s', text)
    last_key = keyed[-1][1] if keyed else ''
    modes = re.findall(r'SetDisplayMode\((\d+)(?:, |x)(\d+)', text)
    frames = re.findall(r'(\d+) frames', text)
    # --firsthit (a SWGB_TRACE build): the first entry to each lifted function, timed by the
    # [keys] step before it (stdout and stderr share the log, in order).
    first, now = [], 0.0
    for l in lines:
        if l.startswith('[keys] '):
            mm = re.match(r'\[keys\] ([0-9.]+)s', l)
            if mm:
                now = float(mm.group(1))
        elif l.startswith('[first] '):
            first.append((now, l.split()[-1]))
    fh = None
    if first:
        by5 = {}
        for t, _ in first:
            by5[int(t // 300) * 5] = by5.get(int(t // 300) * 5, 0) + 1
        fh = {'functions': len(first), 'new_per_5_minutes': {'%d-%d' % (k, k + 5): v for k, v in sorted(by5.items())},
              'timeline': [['%.1f' % t, va] for t, va in first]}
    # How games ended: the music the game opens (--files) for a victory or a defeat.
    endings, now = [], 0.0
    for l in lines:
        if l.startswith('[keys] '):
            mm = re.match(r'\[keys\] ([0-9.]+)s', l)
            now = float(mm.group(1)) if mm else now
        elif l.startswith('[host] open') and re.search(r'stream\\(won\d|lost)\.mp3 -> ok', l, re.I):
            kind = 'defeat' if 'lost' in l.lower() else 'victory'
            if not endings or endings[-1][1] != kind or now - endings[-1][0] > 30:
                endings.append((now, kind))
    return {'events': events, 'watchdog': bool(m), 'last_key': last_key,
            'endings': [['%.0f' % t, k] for t, k in endings],
            'modes': ['%sx%s' % mm for mm in modes], 'frames': int(frames[-1]) if frames else None,
            'firsthit': fh}


def plan_play(s, rnd, out, args, mode, rnd_seed):
    """skirmish, monkey, campaign: a setup, then the idle habits or the monkey. Returns (end, what)."""
    if mode == 'campaign':
        t, what = setup_campaign(s, rnd, 20, args.campaign or None)
        start = t + 100                  # the briefing (~35 s), then the mission loads
        # Its Objectives dialog: Escape closes it. Not its OK button: once the dialog is gone,
        # that spot is the minimap, and the view jumps into unexplored black.
        for k in range(4):
            s.at(start - 30 + 10 * k, 'esc')
    else:
        t, what = setup_skirmish(s, rnd, 20)
        start = t + 80                   # the map loads
    end = args.minutes * 60 + start
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = start + 2
    if args.speed == 'fast':
        t = fast_speed(s, t)
        what['speed'] = 'fast'
    save_at = start + (end - start) * 0.4 if args.save else None
    if save_at:
        what['save'] = 'soak%d' % rnd_seed
    if mode == 'monkey' or (mode == 'campaign' and not args.idle_campaign):
        if save_at:
            monkey(s, rnd, t, save_at - 3, out)
            save_game(s, save_at, what['save'])
            monkey(s, rnd, save_at + 22, end - 5, out)
        else:
            monkey(s, rnd, t, end - 5, out)
    else:
        if save_at:
            habits(s, rnd, t, save_at - 3, None, out)
            save_game(s, save_at, what['save'])
            habits(s, rnd, save_at + 22, end - 5, None, out)
        else:
            habits(s, rnd, t, end - 5, None, out)
        for ts in range(int(start) + 30, int(end), 300):
            s.at(ts, 'shot=%s' % os.path.join(out, 'shot_%04d.bmp' % ts))
    return end, what


def plan_defeat(s, rnd, out, args, game, seed):
    """Alone against two Hardest computers on one team, a Tiny map, and no defence: the
    computers win. The run goes on for --minutes; the log says when (lost.mp3)."""
    t, what = setup_fixed(s, 20, others=2, size=0, diff=4, teams=[0, 1, 1])
    start = t + 80
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, start + 2) if args.speed == 'fast' else start + 2
    end = start + args.minutes * 60
    shots(s, start, end, out, 120)
    return end, dict(what, goal='defeat')


def plan_allied(s, rnd, out, args, game, seed):
    """Me and --allies Hardest computers (team 1) against one (team 2): the allies win the
    game for an idle player, a victory with nothing typed (won1/won2.mp3). In practice the
    one Hardest enemy rushes the idle player first, with 2 allies on a Medium map and with
    6 on a Large one (defeats at 7-12 minutes): my defeat does not wait for my team. It is
    a natural defeat with allies on the map; a victory by fighting needs a defence."""
    n = args.allies
    size = min(4, max(2, (n + 2 + 1) // 2))            # Medium .. Large: the allies close in sooner
    t, what = setup_fixed(s, 20, others=n + 1, size=size, diff=4, teams=[1] * (n + 1) + [2])
    start = t + 80
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, start + 2) if args.speed == 'fast' else start + 2
    end = start + args.minutes * 60
    habits(s, rnd, t, end - 5, None, out)
    shots(s, start, end, out, 120)
    return end, dict(what, goal='allied victory')


def plan_victory(s, rnd, out, args, game, seed):
    """A varied skirmish played for --minutes with a save at 40%, then won (the cheat), the
    score screen's tabs, Main Menu, and the save loaded from Single Player -> Saved Game
    and played for --after minutes."""
    t, what = setup_skirmish(s, rnd, 20)
    start = t + 80
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, start + 2) if args.speed == 'fast' else start + 2
    won = start + args.minutes * 60
    name = 'soak%d' % seed
    save_at = start + (won - start) * 0.4
    habits(s, rnd, t, save_at - 3, None, out)
    save_game(s, save_at, name)
    habits(s, rnd, save_at + 22, won - 5, None, out)
    shots(s, start, won, out)
    t = chat(s, won, WIN_CHEAT)
    t = score_tour(s, t + 25, out, 'victory')
    s.click(t, SCORE_MENU)
    # The saved games, as the list shows them: alphabetical.
    saved = os.path.join(game, 'SaveGame')
    names = sorted({f[:-4].lower() for f in os.listdir(saved) if f.lower().endswith('.gax')} | {name})
    row = names.index(name)
    s.click(t + 10, MAIN_SINGLE)
    s.click(t + 25, SP_SAVED)
    s.click(t + 35, (300, SAVED_ROW0 + SAVED_STEP * row))
    s.at(t + 37, 'shot=%s' % os.path.join(out, 'shot_saved_list.bmp'))
    s.click(t + 38, SAVED_OK)
    loaded = t + 110
    s.at(loaded, 'shot=%s' % os.path.join(out, 'shot_loaded.bmp'))
    end = loaded + args.after * 60
    habits(s, rnd, loaded + 2, end - 5, None, out)
    shots(s, loaded, end, out)
    return end, dict(what, goal='victory, then load', save=name, save_row=row, speed=args.speed)


def plan_chain(s, rnd, out, args, game, seed):
    """OOM-9's missions --first..--last in a row: each played with the monkey for --minutes,
    won (the cheat), its score screen, Scenario Menu, then the next. A mission appears once
    the one before it is won, in this game folder's player profile."""
    s.click(20, MAIN_SINGLE)
    s.click(35, SP_CAMPAIGNS)
    s.click(50, CAMPAIGNS['OOM-9'])
    t = 60
    done = []
    for m in range(args.first, args.last + 1):
        s.click(t, OOM9_MISSIONS[m - 1])
        if m == 1:
            s.click(t + 6, CAMPAIGN_OK)          # the difficulty, the first time only
        start = t + 100                          # the briefing, then the mission
        for k in range(4):
            s.at(start - 30 + 10 * k, 'esc')     # its Objectives
        s.at(start, 'shot=%s' % os.path.join(out, 'shot_m%d_start.bmp' % m))
        t = fast_speed(s, start + 2) if args.speed == 'fast' else start + 2
        won = start + args.minutes * 60
        monkey(s, rnd, t, won - 5, out)
        t = chat(s, won, WIN_CHEAT)
        t = score_tour(s, t + 30, out, 'm%d' % m)
        s.click(t, SCORE_MENU)
        s.at(t + 12, 'shot=%s' % os.path.join(out, 'shot_m%d_after.bmp' % m))
        t += 15
        done.append(m)
    return t + 5, {'campaign': 'OOM-9', 'missions': done, 'speed': args.speed}


def plan_load(s, rnd, out, args, game, seed):
    """Single Player -> Saved Game -> the --row'th save in the list -> OK, then the monkey
    for --minutes."""
    saved = os.path.join(game, 'SaveGame')
    names = sorted(f[:-4].lower() for f in os.listdir(saved) if f.lower().endswith('.gax'))
    s.click(20, MAIN_SINGLE)
    s.click(35, SP_SAVED)
    s.click(45, (300, SAVED_ROW0 + SAVED_STEP * args.row))
    s.at(47, 'shot=%s' % os.path.join(out, 'shot_saved_list.bmp'))
    s.click(48, SAVED_OK)
    start = 120
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_loaded.bmp'))
    end = start + args.minutes * 60
    monkey(s, rnd, start + 2, end - 5, out)
    return end, {'load': names[args.row] if args.row < len(names) else None, 'row': args.row}


def sweep_points(radius=0.8, n=8):
    """Minimap points on the ring where the players start, clear of the minimap's own
    buttons."""
    import math
    cx, cy, w = MINIMAP
    pts = []
    for k in range(n):
        a = 2 * math.pi * k / n
        x, y = cx + radius * w * math.cos(a), cy + radius * w * 0.47 * math.sin(a)
        if any(abs(x - bx) < 16 and abs(y - by) < 12 for bx, by in MINIMAP_BUTTONS):
            x, y = cx + 0.4 * w * math.cos(a), cy + 0.4 * w * 0.47 * math.sin(a)
        pts.append((int(x), int(y)))
    return pts


# The players' colours on the minimap (exact palette entries), by player number.
PLAYER_RGB = {1: (0, 0, 255), 2: (255, 0, 0), 3: (0, 255, 0), 4: (255, 255, 0), 5: (0, 255, 255),
              6: (255, 0, 255), 7: (185, 185, 185), 8: (255, 130, 1)}
ARGS = [None]                        # run_one's args, for wave()


def wave(s, t, group, out):
    """simonsays x --ewoks, H, a box select around the command center, Ctrl+group."""
    for e in range(ARGS[0].ewoks):
        t = chat(s, t + 3, 'simonsays')
    s.at(t + 1, 'h')
    s.at(t + 3, 'shot=%s' % os.path.join(out, 'wave%d.bmp' % group))
    s.at(t + 5, 'ldown:5:25')
    s.at(t + 5.6, 'move:400:220')
    s.at(t + 6.2, 'move:795:420')
    s.at(t + 6.8, 'lup:795:420')
    s.at(t + 8, 'ctrl+%d' % group)
    s.at(t + 9, 'shot=%s' % os.path.join(out, 'wave%d_selected.bmp' % group))
    return t + 11


def plan_fight(s, rnd, out, args, game, seed):
    """A victory won by fighting, the game's own check (won1/won2.mp3); no `skywalker`.

    Me and --allies computers (team 1) against one (team 2, player allies + 2: with 4
    allies, magenta on the minimap), a Large Savannah map (land all over: on Shoreline
    half a blind sweep was in the sea), Easiest (one setting for every computer), Reveal
    Map: All Visible. Then waves of Simon the Killer Ewok (`simonsays`
    typed 3 s apart, beside the command center; the game stops at nine alive, so a wave
    tops the pack up), each boxed up around the command center and made a control group
    (Ctrl+N). This script has the setup and the first wave; the rest is live
    (`--keys-live`, fight_control): every --dwell seconds a shot, the densest cluster of
    the enemy's colour on the minimap, and each group sent there by a right-click on the
    minimap; a new wave every --wave-every seconds; until the game plays its victory (or
    defeat) music. A blind sweep around the ring of start positions found nothing in
    40 minutes."""
    n = args.allies
    # Reveal Map: All Visible, no fog: the enemy's buildings are on the minimap from the start
    # (`forceexplore` shows the terrain, but the fog still hides what nobody of ours sees).
    t, what = setup_fixed(s, 20, others=n + 1, size=4, diff=0, teams=[1] * (n + 1) + [2],
                          loc=LOCATIONS.index('Savannah'), reveal=2)
    start = t + 80
    s.at(start, 'shot=%s' % os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, start + 2) if args.speed == 'fast' else start + 2
    t = wave(s, t, 1, out)
    end = start + args.minutes * 60
    return end, dict(what, goal='victory by fighting', enemy_player=n + 2, ewoks=args.ewoks, dwell=args.dwell,
                     wave_every=args.wave_every, live_from=round(t + 2))


def enemy_on_minimap(bmp, rgb):
    """The densest 12x12 cluster of the enemy's colour on the minimap, and how
    many such pixels there are in all; (None, 0) if none."""
    from PIL import Image
    im = Image.open(bmp).convert('RGB')
    # The minimap's box: only one player draws in that exact colour, so no diamond test (a
    # tight one cut off a base at the diamond's edge and aimed at a lone scout instead).
    bins = {}
    for y in range(456, 588):
        for x in range(448, 724):
            if im.getpixel((x, y)) == rgb:
                bins.setdefault((x // 12, y // 12), []).append((x, y))
    if not bins:
        return None, 0
    best = max(bins.values(), key=len)
    total = sum(len(v) for v in bins.values())
    return (sum(p[0] for p in best) // len(best), sum(p[1] for p in best) // len(best)), total


GAME_OVER = re.compile(r'stream\\(won\d|lost)\.mp3', re.I)


def fight_control(args, out, log, live, proc, t_launch, what):
    """The live half of --mode fight (see plan_fight)."""
    def say(*steps):
        with open(live, 'a', newline='\n') as f:
            for st in steps:
                f.write('0:%s\n' % st)

    def ended():
        try:
            return GAME_OVER.search(open(log, encoding='utf-8', errors='replace').read())
        except OSError:
            return None

    rgb = PLAYER_RGB[what['enemy_player']]
    pts = sweep_points()
    while time.time() - t_launch < what['live_from'] and proc.poll() is None:
        time.sleep(1)
    groups, last_wave, k, aims = 1, time.time(), 0, []
    while proc.poll() is None and not ended():
        shot = os.path.join(out, 'live_%04d.bmp' % k)
        say('move:400:300')                         # the cursor off the minimap: it hid what was under it
        time.sleep(0.5)
        say('shot=' + shot)
        for _ in range(40):
            if os.path.exists(shot):
                break
            time.sleep(0.25)
        time.sleep(0.5)
        target, seen = None, 0
        if os.path.exists(shot):
            try:
                from PIL import Image
                Image.open(shot).crop((440, 450, 730, 585)).save(os.path.join(out, 'minimap_%04d.png' % k))
                target, seen = enemy_on_minimap(shot, rgb)
            except Exception as e:                  # noqa: BLE001 -- a half-written shot: sweep this time
                print('  fight: %s: %s' % (shot, e))
            try:
                os.remove(shot)
            except OSError:
                pass
        if target is None:
            target = pts[k % len(pts)]
        aims.append([round(time.time() - t_launch), target[0], target[1], seen])
        if time.time() - last_wave > args.wave_every and groups < 9:
            groups += 1
            for e in range(args.ewoks):
                say('enter')
                time.sleep(0.6)
                for ch in 'simonsays':
                    say(ch)
                    time.sleep(0.3)
                say('enter')
                time.sleep(2.5)
            for st, pause in (('h', 2), ('ldown:5:25', 0.6), ('move:400:220', 0.6), ('move:795:420', 0.6),
                              ('lup:795:420', 1.2), ('ctrl+%d' % groups, 1)):
                say(st)
                time.sleep(pause)
            say('shot=' + os.path.join(out, 'wave%d_selected.bmp' % groups))
            last_wave = time.time()
        for g in range(1, groups + 1):
            say('%d' % g)
            time.sleep(0.6)
            say('rclick:%d:%d' % target)
            time.sleep(0.9)
        if not 495 <= target[1] <= 535:              # the score screen's buttons: never left-click there
            time.sleep(args.dwell * 0.5)
            if ended():
                break
            say('click:%d:%d' % target)
            time.sleep(2)
            say('shot=' + os.path.join(out, 'fight_%04d.bmp' % round(time.time() - t_launch)))
            time.sleep(args.dwell * 0.5)
        else:
            time.sleep(args.dwell)
        k += 1
    what['aims'] = aims
    what['waves'] = groups
    if ended() and proc.poll() is None:
        # The game is over: its own message, then the score screen. Shoot both, the score
        # screen's tabs, and end the run (this run's process tree only, by its PID).
        say('shot=' + os.path.join(out, 'game_over.bmp'))
        time.sleep(25)
        say('shot=' + os.path.join(out, 'score.bmp'))
        for i, xy in enumerate(SCORE_TABS[1:]):
            time.sleep(2)
            say('click:%d:%d' % xy)
            time.sleep(2)
            say('shot=' + os.path.join(out, 'score_tab%d.bmp' % (i + 1)))
        time.sleep(4)
        what['ended_by'] = 'the game over, then this tool'
        kill_tree(proc)


ENDGAME = {'fight': plan_fight, 'load': plan_load, 'defeat': plan_defeat, 'allied': plan_allied, 'victory': plan_victory, 'chain': plan_chain}


def run_one(args, i, mode, rnd_seed, out_root, game):
    rnd = random.Random(rnd_seed)
    name = '%02d-%s-%d' % (i, mode, rnd_seed)
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    s = Script()
    SHOT_DIR[0] = out
    ARGS[0] = args
    if mode in ENDGAME:
        end, what = ENDGAME[mode](s, rnd, out, args, game, rnd_seed)
    else:
        end, what = plan_play(s, rnd, out, args, mode, rnd_seed)
    s.at(end - 2, 'shot=%s' % os.path.join(out, 'shot_end.bmp'))
    keys = os.path.join(out, 'keys.txt')
    open(keys, 'w', newline='\n').write(s.text())
    log = os.path.join(out, 'run.log')
    cmd = [args.run, '--run', '--multi', '--headless', '--files', '--exe', args.exe, '--game', game,
           '--watchdog', str(int(end + 5)), '--keys', '@' + keys] + args.extra.split()
    live = os.path.join(out, 'live.txt')
    if mode == 'fight':
        open(live, 'w').close()
        cmd += ['--keys-live', live]
    meta = {'name': name, 'mode': mode, 'seed': rnd_seed, 'setup': what, 'minutes': args.minutes,
            'planned_seconds': round(end),
            'game': game, 'cmd': cmd, 'started': datetime.datetime.now().isoformat(timespec='seconds')}
    t0 = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=out, start_new_session=os.name != 'nt')
        meta['pid'] = p.pid
        if mode == 'fight':
            fight_control(args, out, log, live, p, t0, what)
        rc = p.wait()
    meta['seconds'] = round(time.time() - t0)
    meta['exit'] = rc
    meta['outcome'] = EXIT.get(rc, 'exit %d' % rc)
    meta.update(analyse(log, s.text()))
    if what.get('ended_by') and meta['endings']:   # the fight's controller ended it after the game over
        meta['outcome'] = 'game over: %s' % meta['endings'][-1][1]
    for f in sorted(os.listdir(out)):
        if f.endswith('.bmp') and shutil.which('ffmpeg'):
            png = os.path.join(out, f[:-4] + '.png')
            if subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', os.path.join(out, f), png]).returncode == 0:
                os.remove(os.path.join(out, f))
    json.dump(meta, open(os.path.join(out, 'summary.json'), 'w'), indent=1)
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mode', default='skirmish', choices=['skirmish', 'monkey', 'campaign', 'mixed'] + sorted(ENDGAME))
    ap.add_argument('--after', type=float, default=10, help='victory: minutes played after loading the save')
    ap.add_argument('--allies', type=int, default=6, help='allied, fight: computer allies (1-6)')
    ap.add_argument('--ewoks', type=int, default=9, help='fight: simonsays typed per wave (9 alive at most)')
    ap.add_argument('--wave-every', type=float, default=300, help='fight: seconds between waves (up to 9)')
    ap.add_argument('--dwell', type=float, default=90, help='fight: seconds at each sweep point')
    ap.add_argument('--row', type=int, default=0, help='load: the save to load, by its row in the list')
    ap.add_argument('--first', type=int, default=1, help='chain: the first OOM-9 mission (1-5)')
    ap.add_argument('--last', type=int, default=3, help='chain: the last OOM-9 mission (1-5)')
    ap.add_argument('--runs', type=int, default=1)
    ap.add_argument('--minutes', type=float, default=25)
    ap.add_argument('--jobs', type=int, default=1, help='runs at once (each with its own game folder)')
    ap.add_argument('--seed', type=int, default=None)
    ap.add_argument('--idle-campaign', action='store_true', help='the idle habits in a campaign, not the monkey')
    ap.add_argument('--speed', default='fast', choices=['fast', 'normal'],
                    help='the game speed (the game menu: Options); fast plays more game per hour')
    ap.add_argument('--no-save', dest='save', action='store_false',
                    help='skip the save at 40%% of the run (the game menu: Save)')
    ap.add_argument('--cc', action='store_true', help="Clone Campaigns: its host, exe, menus and civilizations")
    ap.add_argument('--civs', default='', help='the computer players\' civilizations, in turn (comma-separated)')
    ap.add_argument('--my-civ', default='', help="the player's civilization")
    ap.add_argument('--campaign', default='', help='campaign mode: this campaign, not a drawn one')
    ap.add_argument('--run', default=None,
                    help=r'the host (A:\Games\star\builds\coverage\swgbrecomp.exe; --cc: ...\clone-campaigns\swgbrecomp_cc.exe)')
    ap.add_argument('--exe', default=None,
                    help=r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe (--cc: work\battlegrounds_x1.exe here)')
    ap.add_argument('--game', default=None,
                    help=r'the game folder (A:\Games\star\games\coverage; --cc: ...\clone-campaigns); '
                         'job k > 0 uses a copy, DIR-soakK (hard links)')
    ap.add_argument('--extra', default='',
                    help='more host options, e.g. "--firsthit 0x401000 0x651000" with a SWGB_TRACE build')
    ap.add_argument('--out', default=os.path.join(ROOT, 'work', 'soak',
                                                  datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    args = ap.parse_args()
    if args.cc:
        clone_campaigns()
        if args.mode in ENDGAME:
            sys.exit('--cc: --mode %s plays the base game\'s OOM-9 campaign; skirmish, monkey or campaign' % args.mode)
    if os.name == 'nt':
        args.run = args.run or (r'A:\Games\star\builds\clone-campaigns\swgbrecomp_cc.exe' if args.cc
                                else r'A:\Games\star\builds\coverage\swgbrecomp.exe')
        args.exe = args.exe or (os.path.join(ROOT, 'work', 'battlegrounds_x1.exe') if args.cc
                                else r'A:\Games\star\swgb-recomp\work\Battlegrounds.exe')
        args.game = args.game or (r'A:\Games\star\games\clone-campaigns' if args.cc else r'A:\Games\star\games\coverage')
    else:   # Linux and macOS: this checkout's build, lift and a game folder under work/
        build = os.path.join(ROOT, 'build-macos' if sys.platform == 'darwin' else 'build-linux')
        args.run = args.run or os.path.join(build, 'swgbrecomp_cc' if args.cc else 'swgbrecomp')
        args.exe = args.exe or os.path.join(ROOT, 'work', 'battlegrounds_x1.exe' if args.cc else 'Battlegrounds.exe')
        args.game = args.game or os.path.join(ROOT, 'work', 'games', 'soak-cc' if args.cc else 'soak')
        if not os.path.isdir(args.game):
            ensure_game(os.path.join(ROOT, 'game'), args.game)
    for c in [c for c in args.civs.split(',') if c] + ([args.my_civ] if args.my_civ else []):
        if c not in CIVS:
            sys.exit('no civilization %r: %s' % (c, ', '.join(CIVS)))
    if args.campaign and args.campaign not in CAMPAIGNS:
        sys.exit('no campaign %r: %s' % (args.campaign, ', '.join(sorted(CAMPAIGNS))))
    FORCED_CIVS[:] = [c for c in args.civs.split(',') if c]
    MY_CIV[0] = args.my_civ or None
    sys.stdout.reconfigure(line_buffering=True)    # a run's line as it ends, into a redirected log too
    args.out = os.path.abspath(args.out)        # the game runs in its own folder
    seed = args.seed if args.seed is not None else random.randrange(1 << 20)
    rnd = random.Random(seed)
    os.makedirs(args.out, exist_ok=True)
    games = [args.game] + ['%s-soak%d' % (args.game.rstrip('\\/'), k) for k in range(1, args.jobs)]
    for g in games[1:]:
        ensure_game(args.game, g)
    modes = ['skirmish', 'monkey', 'campaign']
    plan = [(i, args.mode if args.mode != 'mixed' else modes[i % 3], rnd.randrange(1 << 20))
            for i in range(args.runs)]
    print('soak: %d run(s), %s, %g min each, %d at once, seed %d -> %s'
          % (args.runs, args.mode, args.minutes, args.jobs, seed, args.out))
    results, lock = [], threading.Lock()
    free = list(games)

    def worker(item):
        with lock:
            g = free.pop()
        try:
            m = run_one(args, item[0], item[1], item[2], args.out, g)
        finally:
            with lock:
                free.append(g)
        with lock:
            results.append(m)
            print('  %-28s %-20s %5ds  %d event(s)  %s' % (m['name'], m['outcome'], m['seconds'],
                                                          len(m['events']), json.dumps(m['setup'])))
            for e in m['events'][:6]:
                print('      ' + e['line'])
    threads = []
    pending = list(plan)
    while pending or threads:
        threads = [t for t in threads if t.is_alive()]
        while pending and len(threads) < args.jobs:
            th = threading.Thread(target=worker, args=(pending.pop(0),))
            th.start()
            threads.append(th)
        time.sleep(1)
    json.dump(sorted(results, key=lambda m: m['name']), open(os.path.join(args.out, 'summary.json'), 'w'), indent=1)
    bad = [m for m in results if (m['exit'] not in (0, 4) and not m['outcome'].startswith('game over'))
           or m['events']]
    hours = sum(m['seconds'] for m in results) / 3600
    print('%d run(s), %.2f game-hours, %d with a problem; %s' % (len(results), hours, len(bad), args.out))
    for m in results:
        if m['modes'] and m['modes'][0] != '800x600':
            print('  warning: %s ran at %s: the HUD coordinates are for 800x600' % (m['name'], m['modes'][0]))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
