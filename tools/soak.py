#!/usr/bin/env python3
"""Long headless games against the computer, to find what a short one never runs.

    py -3 tools/soak.py [--mode skirmish|monkey|campaign|missions|ingame-load|...] [--runs N]
                        [--minutes M] [--jobs J] [--seed S] [--run EXE] [--game DIR] [--out DIR]
                        [--cc] [--civs A,B] [--my-civ C] [--campaign NAME] [--first K]
                        [--last K] [--unlocked] [--screen 800|1024|1280]

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

  missions  --campaign's missions --first..--last in a row (any of the six, or with
            --cc either Episode II campaign): each mission's box on the campaign
            screen (screens.py), the briefing, the monkey with a save, the
            victory cheat, the end briefing, the score screen, Scenario Menu.
            The missions before --first are passed through (won at once) to
            open their boxes, unless --unlocked. `chain` is this mode.
  ingame-load  a varied skirmish (or --campaign's mission --first) with the
            monkey, a save at 30%, then the game menu's Load at 55%: Yes, the
            save from the list, OK, and the monkey in the loaded game.

--screen 1024|1280 sets the Screen Size on the main menu's Options first (it
lasts for that run: the game stores it only when it exits normally) and moves
the HUD's clicks to that size's layout (base game only).

In every mode the game speed is set to Fast (--speed) and the game is saved
once, 40% of the way in (--no-save).

Each run's log, its --keys script and its shots go to OUT/<run>/; the
summary prints every unresolved indirect call, fault and not-lifted stub
with the keys that came before it, and how long the game ran. Exit codes of
the host: 4 the watchdog ended it (the run survived), 0 the game exited,
2 a not-lifted stub, 3 a fault, 5 an unresolved indirect call (coverage.c).

Then `py -3 tools/seeds_from_logs.py` turns the unresolved addresses into
seeds (it reads work/soak/**/*.log).

Coordinates are the 800x600 screens (the menus, the setup screen, the HUD)
unless --screen says otherwise: a --game folder whose Screen Size is 1024 or
1280 moves the HUD, so the tool checks the game's SetDisplayMode calls and
warns (with --screen, a run at another size is a problem).
"""
import argparse
import datetime
import json
import math
import os
import random
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from swgb import runner, screens  # noqa: E402
from swgb.runner import Script  # noqa: E402
from swgb.screens import chat, fast_speed, pick, save_game  # noqa: E402

MONKEY_KEYS = list('abcdefghijklmnopqrstuvwxyz0123456789') + \
    ['esc', 'enter', 'space', 'tab', 'up', 'down', 'left', 'right', 'home', 'end', 'bksp'] + \
    ['f1', 'f2', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f11']   # F3 pauses, F10 is the menu: kept off


def setup_skirmish(s, lay, rnd, t, my_civ=None, civs=()):
    """Main menu -> Standard Game -> a setup varied from RND -> Start Game. MY_CIV: the player's
    civilization; CIVS: the computers', in turn (else drawn). Returns (t, what)."""
    t = screens.to_setup(s, lay, t)
    others = rnd.randint(1, 7)
    size = rnd.randint(max(0, (others + 1 + 1) // 2 - 1), 5)   # big enough for the players, more or less
    loc = rnd.randrange(len(screens.LOCATIONS))
    diff = rnd.randrange(len(screens.DIFFICULTIES))
    t = pick(s, t, screens.DD_OTHERS, others - 1)
    t = pick(s, t, screens.DD_LOCATION, loc)
    t = pick(s, t, screens.DD_SIZE, size)
    t = pick(s, t, screens.DD_DIFFICULTY, diff)
    if my_civ:                                      # the player's own row, above the computers'
        t = screens.set_civ(s, lay, t, screens.MY_CIV_ROW, my_civ)
    drawn = []
    for p in range(others):
        civ = civs[p % len(civs)] if civs else rnd.choice(lay.civ_items())
        drawn.append(civ)
        t = screens.set_civ(s, lay, t, screens.computer_row(p), civ)
    s.click(t + 1, screens.SETUP_START)
    what = {'others': others, 'location': screens.LOCATIONS[loc], 'size': screens.SIZES[size],
            'difficulty': screens.DIFFICULTIES[diff], 'civs': drawn}
    if my_civ:
        what['my_civ'] = my_civ
    return t + 1, what


def setup_campaign(s, lay, rnd, t, name=None):
    name = name or rnd.choice(sorted(c for c in lay.campaigns if c not in lay.requires))   # a locked one: missions
    s.click(t, screens.MAIN_SINGLE)
    s.click(t + 15, lay.sp_campaigns)
    s.click(t + 30, lay.campaigns[name])
    s.click(t + 40, screens.CAMPAIGN_MISSION1)
    s.click(t + 46, screens.CAMPAIGN_OK)
    return t + 46, {'campaign': name, 'mission': 1}


def setup_fixed(s, lay, t, out, others, size, diff, teams=None, loc=None, reveal=None):
    """Standard Game with a fixed setup, a shot of it in OUT. teams: team clicks per row, me
    first (0 = no team)."""
    t = screens.to_setup(s, lay, t)
    t = pick(s, t, screens.DD_OTHERS, others - 1)
    if loc is not None:
        t = pick(s, t, screens.DD_LOCATION, loc)
    t = pick(s, t, screens.DD_SIZE, size)
    t = pick(s, t, screens.DD_DIFFICULTY, diff)
    if reveal is not None:
        t = pick(s, t, screens.DD_REVEAL, reveal)
    # A click just after a drop-down closes, or a second one on a button the cursor has
    # not left, did not count (a 6-allies game started with me and the 7th on no team).
    t += 2
    for row, n in enumerate(teams or []):
        y = screens.MY_CIV_ROW if row == 0 else screens.computer_row(row - 1)
        for k in range(n):
            s.move(t, (screens.TEAM_X - 60, y))
            s.click(t + 0.7, (screens.TEAM_X, y))
            t += 1.6
    s.shot(t + 1, os.path.join(out, 'shot_setup.bmp'))
    t += 1
    s.click(t + 1, screens.SETUP_START)
    return t + 1, {'others': others, 'size': screens.SIZES[size], 'difficulty': screens.DIFFICULTIES[diff],
                   'teams': teams, 'location': screens.LOCATIONS[loc] if loc is not None else '(as left)'}


def score_tour(s, t, out, tag):
    """Each tab of the score screen, a shot of the first and the last."""
    s.shot(t, os.path.join(out, 'score_%s.bmp' % tag))
    tabs = screens.SCORE_TABS
    for k, xy in enumerate(tabs[1:] + tabs[:1]):
        s.click(t + 2 + 2 * k, xy)
    s.shot(t + 2 + 2 * len(tabs), os.path.join(out, 'score_%s_tabs.bmp' % tag))
    return t + 4 + 2 * len(tabs)


def shots(s, t0, t1, out, every=300):
    for ts in range(int(t0) + 30, int(t1), every):
        s.shot(ts, os.path.join(out, 'shot_%04d.bmp' % ts))


def habits(s, lay, rnd, t0, t1):
    """An idle player: train a worker now and then, jump home."""
    t = t0
    x0, y0, x1, y1 = lay.map_view
    while t < t1:
        s.at(t, 'h')                                   # the command center, selected and centred
        s.click(t + 1, lay.command_grid[0])            # its first button: a worker
        # rally / gather point, in the map view's middle (150-650 x 60-400 at 800x600)
        s.click(t + 3, (rnd.randint(x0 + (x1 - x0) * 3 // 16, x1 - (x1 - x0) * 3 // 16),
                        rnd.randint(y0 + 40, y1 - 25)), 'rclick')
        t += rnd.uniform(25, 45)


def monkey(s, lay, rnd, t0, t1, out):
    """Random input over the game screen and the HUD."""
    t = t0
    x0, y0, x1, y1 = lay.map_view
    while t < t1:
        r = rnd.random()
        if r < 0.30:
            s.click(t, (rnd.randint(x0, x1), rnd.randint(y0, y1)))
        elif r < 0.45:
            s.click(t, (rnd.randint(x0, x1), rnd.randint(y0, y1)), 'rclick')
        elif r < 0.62:
            s.click(t, rnd.choice(lay.command_grid))
        elif r < 0.66:
            cx, cy, w = lay.minimap
            s.click(t, (cx + rnd.randint(-w // 2, w // 2), cy + rnd.randint(-w // 4, w // 4)),
                    rnd.choice(['click', 'rclick']))
        elif r < 0.68:
            s.click(t, rnd.choice(lay.minimap_buttons + lay.top_icons))
        elif r < 0.72 and t + 8 > t1:
            # No room before t1 for a panel or the game menu (their Escapes come 5-6 s later): a
            # save or load scripted at t1 would be closed by them. A key instead.
            s.at(t, rnd.choice(MONKEY_KEYS))
        elif r < 0.71:
            # A HUD panel (objectives, chat, diplomacy, ...): a click or two inside it, then Escape.
            s.click(t, lay.hud[rnd.choice(['objectives', 'chat', 'diplomacy', 'techtree'])])
            for k in range(rnd.randint(0, 3)):
                s.click(t + 1.5 + k, (rnd.randint(150, 650), rnd.randint(120, 450)))
            s.at(t + 5, 'esc')
            t += 5
        elif r < 0.72:
            # The game menu: Options or About, then out again; never Quit, Restart or Load.
            s.click(t, lay.hud['menu'])
            s.click(t + 1.5, lay.game_menu[rnd.choice(['options', 'about', 'cancel'])])
            s.at(t + 5, 'esc')
            s.at(t + 6, 'esc')
            t += 6
        elif r < 0.95:
            s.at(t, rnd.choice(MONKEY_KEYS))
        else:
            s.at(t, 'h')                       # home: keeps the view where there is something to click
        t += rnd.uniform(0.4, 2.0)
    shots(s, t0, t1, out)


def analyse(log, keys_text):
    text = open(log, encoding='utf-8', errors='replace').read()
    lines = text.splitlines()
    keyed = [(i, l) for i, l in enumerate(lines) if l.startswith('[keys]')]
    events = []
    for i, l in enumerate(lines):
        if runner.EVENTS.search(l):
            before = [k for j, k in keyed if j < i][-8:]
            events.append({'line': l.strip(), 'context': [x.rstrip() for x in lines[i:i + 30]],
                           'keys_before': before})
    m = re.search(r'\[watchdog\] (\d+) s', text)
    last_key = keyed[-1][1] if keyed else ''
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
    # The saved games the game opened (--files), by the [keys] time before: a save writes one,
    # a load reads it.
    saves, now = [], 0.0
    for l in lines:
        if l.startswith('[keys] '):
            mm = re.match(r'\[keys\] ([0-9.]+)s', l)
            now = float(mm.group(1)) if mm else now
        else:
            mm = re.match(r'\[host\] open (?:.*\\)?savegame\\([^\\]+?)\.ga[x1] -> ok', l, re.I)
            if mm:
                saves.append(['%.0f' % now, mm.group(1).lower()])
    # Campaign missions: a mission's end briefing (campaign\media\<campaign>s<N>_end.mm) opens
    # when it is won, Basic Training's too (which plays no victory music).
    won = []
    for l in lines:
        mm = re.match(r'\[host\] open campaign\\media\\(\w+?)_end\.mm -> ok', l, re.I)
        if mm and mm.group(1).lower() not in won:
            won.append(mm.group(1).lower())
    return {'events': events, 'watchdog': bool(m), 'last_key': last_key, 'save_opens': saves,
            'missions_won': won,
            'endings': [['%.0f' % t, k] for t, k in endings],
            'modes': ['%dx%d' % mm for mm in runner.modes(text)], 'frames': int(frames[-1]) if frames else None,
            'firsthit': fh}


# ---- the plans: each fills a script from t0 (the main menu) and returns (end, what) -------------
def plan_play(s, lay, rnd, out, args, mode, seed, t0):
    """skirmish, monkey, campaign: a setup, then the idle habits or the monkey."""
    if mode == 'campaign':
        t, what = setup_campaign(s, lay, rnd, t0, args.campaign or None)
        start = t + 100                  # the briefing (~35 s), then the mission loads
        # Its Objectives dialog: Escape closes it. Not its OK button: once the dialog is gone,
        # that spot is the minimap, and the view jumps into unexplored black.
        for k in range(4):
            s.at(start - 30 + 10 * k, 'esc')
    else:
        t, what = setup_skirmish(s, lay, rnd, t0, args.my_civ, args.civs)
        start = t + 80                   # the map loads
    end = args.minutes * 60 + start
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    t = start + 2
    if args.speed == 'fast':
        t = fast_speed(s, lay, t)
        what['speed'] = 'fast'
    save_at = start + (end - start) * 0.4 if args.save else None
    if save_at:
        what['save'] = 'soak%d' % seed
    if mode == 'monkey' or (mode == 'campaign' and not args.idle_campaign):
        if save_at:
            monkey(s, lay, rnd, t, save_at - 3, out)
            save_game(s, lay, save_at, what['save'])
            monkey(s, lay, rnd, save_at + 22, end - 5, out)
        else:
            monkey(s, lay, rnd, t, end - 5, out)
    else:
        if save_at:
            habits(s, lay, rnd, t, save_at - 3)
            save_game(s, lay, save_at, what['save'])
            habits(s, lay, rnd, save_at + 22, end - 5)
        else:
            habits(s, lay, rnd, t, end - 5)
        shots(s, start, end, out)
    return end, what


def plan_defeat(s, lay, rnd, out, args, game, seed, t0):
    """Alone against two Hardest computers on one team, a Tiny map, and no defence: the
    computers win. The run goes on for --minutes; the log says when (lost.mp3)."""
    t, what = setup_fixed(s, lay, t0, out, others=2, size=0, diff=4, teams=[0, 1, 1])
    start = t + 80
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    if args.speed == 'fast':
        fast_speed(s, lay, start + 2)
    end = start + args.minutes * 60
    shots(s, start, end, out, 120)
    return end, dict(what, goal='defeat')


def plan_allied(s, lay, rnd, out, args, game, seed, t0):
    """Me and --allies Hardest computers (team 1) against one (team 2): the allies win the
    game for an idle player, a victory with nothing typed (won1/won2.mp3). In practice the
    one Hardest enemy rushes the idle player first, with 2 allies on a Medium map and with
    6 on a Large one (defeats at 7-12 minutes): my defeat does not wait for my team. It is
    a natural defeat with allies on the map; a victory by fighting needs a defence."""
    n = args.allies
    size = min(4, max(2, (n + 2 + 1) // 2))            # Medium .. Large: the allies close in sooner
    t, what = setup_fixed(s, lay, t0, out, others=n + 1, size=size, diff=4, teams=[1] * (n + 1) + [2])
    start = t + 80
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, lay, start + 2) if args.speed == 'fast' else start + 2
    end = start + args.minutes * 60
    habits(s, lay, rnd, t, end - 5)
    shots(s, start, end, out, 120)
    return end, dict(what, goal='allied victory')


def plan_victory(s, lay, rnd, out, args, game, seed, t0):
    """A varied skirmish played for --minutes with a save at 40%, then won (the cheat), the
    score screen's tabs, Main Menu, and the save loaded from Single Player -> Saved Game
    and played for --after minutes."""
    t, what = setup_skirmish(s, lay, rnd, t0, args.my_civ, args.civs)
    start = t + 80
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, lay, start + 2) if args.speed == 'fast' else start + 2
    won = start + args.minutes * 60
    name = 'soak%d' % seed
    save_at = start + (won - start) * 0.4
    habits(s, lay, rnd, t, save_at - 3)
    save_game(s, lay, save_at, name)
    habits(s, lay, rnd, save_at + 22, won - 5)
    shots(s, start, won, out)
    t = chat(s, won, screens.WIN_CHEAT)
    t = score_tour(s, t + 25, out, 'victory')
    s.click(t, screens.SCORE_MENU)
    # The saved games, as the list shows them: alphabetical.
    row = screens.saved_row(lay, game, name)
    s.click(t + 10, screens.MAIN_SINGLE)
    s.click(t + 25, lay.sp_saved)
    s.click(t + 35, (screens.SAVED_X, screens.SAVED_ROW0 + screens.SAVED_STEP * row))
    s.shot(t + 37, os.path.join(out, 'shot_saved_list.bmp'))
    s.click(t + 38, screens.SAVED_OK)
    loaded = t + 110
    s.shot(loaded, os.path.join(out, 'shot_loaded.bmp'))
    end = loaded + args.after * 60
    habits(s, lay, rnd, loaded + 2, end - 5)
    shots(s, loaded, end, out)
    return end, dict(what, goal='victory, then load', save=name, save_row=row, speed=args.speed)


def mission(s, lay, rnd, out, args, seed, t, box, m, play, tag):
    """One mission from its campaign screen: its box (mission 1: the difficulty's OK), the
    briefing, Escape for its Objectives; if PLAY, Fast and the monkey for --minutes with a save
    40% of the way in, else nothing (passing through); then the cheat, the end briefing, the
    score screen's tabs and Scenario Menu. Returns the time back on the campaign screen."""
    s.click(t, box)
    if m == 1:
        s.click(t + 6, screens.CAMPAIGN_OK)      # the difficulty, the first time only
    start = t + 110                              # the briefing (~35 s), then the mission loads
    for k in range(4):
        s.at(start - 30 + 10 * k, 'esc')         # its Objectives
    s.shot(start, os.path.join(out, 'shot_%s_start.bmp' % tag))
    if not play:
        won = start + 5                          # passing through: won at once
    else:
        t = fast_speed(s, lay, start + 2) if args.speed == 'fast' else start + 2
        won = start + args.minutes * 60
        if args.save:
            save_at = t + (won - t) * 0.4
            monkey(s, lay, rnd, t, save_at - 3, out)
            save_game(s, lay, save_at, 'soak%d%s' % (seed, tag))
            monkey(s, lay, rnd, save_at + 22, won - 5, out)
        else:
            monkey(s, lay, rnd, t, won - 5, out)
    t = chat(s, won, screens.WIN_CHEAT)
    # The end briefing (20-35 s in every mission tried), then the score screen.
    t = score_tour(s, t + 45, out, tag)
    s.click(t, screens.SCORE_MENU)
    s.shot(t + 12, os.path.join(out, 'shot_%s_after.bmp' % tag))
    return t + 15


def default_campaign(lay):
    """missions without --campaign: OOM-9, Confederacy in Clone Campaigns."""
    return 'Confederacy' if lay.game == 'cc' else 'OOM-9'


def plan_missions(s, lay, rnd, out, args, game, seed, t0):
    """--campaign's missions --first..--last in a row on its campaign screen (`chain` is this
    mode). Each played mission: its box, the briefing, Escape for its Objectives, Fast, the
    monkey for --minutes with a save 40% of the way in, then won (the cheat), the end
    briefing, the score screen's tabs and Scenario Menu, back on the campaign screen for the
    next box.

    A box shows once the mission before it is won, in this game folder's player profile, so
    the missions before --first are passed through first: each started and won at once,
    about three minutes each. A campaign that opens only when another is won (Galactic
    Republic: Confederacy) has that one passed through first, all of it. --unlocked skips
    both, for a profile that has them open already. Mission 1's first start asks for the
    difficulty: its OK (Easy)."""
    name = args.campaign or default_campaign(lay)
    boxes = lay.missions[name]
    last = min(args.last or len(boxes), len(boxes))
    s.click(t0, screens.MAIN_SINGLE)
    s.click(t0 + 15, lay.sp_campaigns)
    t = t0 + 30
    passed, played = [], []
    before = lay.requires.get(name)
    if before and not args.unlocked:
        s.click(t, lay.campaigns[before])
        t += 10
        for m, box in enumerate(lay.missions[before], 1):
            t = mission(s, lay, rnd, out, args, seed, t, box, m, False, 'pre%d' % m)
            passed.append('%s %d' % (before, m))
        # After a campaign's last mission the game is back on the list of campaigns.
    s.click(t, lay.campaigns[name])
    t += 10
    for m in range(args.first if args.unlocked else 1, last + 1):
        play = m >= args.first
        t = mission(s, lay, rnd, out, args, seed, t, boxes[m - 1], m, play, 'm%d' % m)
        (played if play else passed).append(m)
    return t + 5, {'campaign': name, 'missions': played, 'passed_through': passed, 'speed': args.speed,
                   'save': args.save}


plan_chain = plan_missions


def ingame_load(s, lay, t, row, out, tag='loaded'):
    """The game menu -> Load -> Yes (quit this game) -> the row'th save -> OK. Returns the time
    the loaded game is up."""
    s.click(t, lay.hud['menu'])
    s.click(t + 2, lay.game_menu['load'])
    s.click(t + 4, lay.yes)
    x, row0, step, ok = lay.load_list
    s.shot(t + 12, os.path.join(out, 'shot_load_list.bmp'))
    s.click(t + 13, (x, round(row0 + step * row)))
    s.click(t + 15, ok)
    s.shot(t + 80, os.path.join(out, 'shot_%s.bmp' % tag))
    return t + 82


def plan_ingame_load(s, lay, rnd, out, args, game, seed, t0):
    """A varied skirmish (or, with --campaign, that campaign's mission --first, which must be
    open: Basic Training's all are), the monkey, a save 30% of the way in, the monkey on, then
    the game menu's Load at 55%: Yes, the save from the list, OK; and the monkey in the loaded
    game to the end. The main menu's Saved Game is `victory` and `load`."""
    if args.campaign:
        s.click(t0, screens.MAIN_SINGLE)
        s.click(t0 + 15, lay.sp_campaigns)
        s.click(t0 + 30, lay.campaigns[args.campaign])
        s.click(t0 + 40, lay.missions[args.campaign][args.first - 1])
        if args.first == 1:
            s.click(t0 + 46, screens.CAMPAIGN_OK)
        start = t0 + 150
        for k in range(4):
            s.at(start - 30 + 10 * k, 'esc')
        what = {'campaign': args.campaign, 'mission': args.first}
    else:
        # Easiest, 1-3 computers on a Medium map: Hardest ones razed the monkey's base before
        # the Load (a defeat at 11 minutes), and the monkey then clicked the main menu's Exit.
        t, what = setup_fixed(s, lay, t0, out, others=rnd.randint(1, 3), size=2, diff=0,
                              loc=rnd.randrange(len(screens.LOCATIONS)))
        start = t + 80
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, lay, start + 2) if args.speed == 'fast' else start + 2
    end = start + args.minutes * 60
    name = 'soak%d' % seed
    save_at, load_at = start + (end - start) * 0.3, start + (end - start) * 0.55
    monkey(s, lay, rnd, t, save_at - 3, out)
    save_game(s, lay, save_at, name)
    monkey(s, lay, rnd, save_at + 22, load_at - 3, out)
    row = screens.saved_row(lay, game, name)
    loaded = ingame_load(s, lay, load_at, row, out)
    monkey(s, lay, rnd, loaded, end - 5, out)
    return end, dict(what, goal='save, then the in-game Load', save=name, save_row=row, speed=args.speed,
                     load_at=round(load_at))


def plan_load(s, lay, rnd, out, args, game, seed, t0):
    """Single Player -> Saved Game -> the --row'th save in the list -> OK, then the monkey
    for --minutes."""
    names = screens.saved_names(lay, game)
    s.click(t0, screens.MAIN_SINGLE)
    s.click(t0 + 15, lay.sp_saved)
    s.click(t0 + 25, (screens.SAVED_X, screens.SAVED_ROW0 + screens.SAVED_STEP * args.row))
    s.shot(t0 + 27, os.path.join(out, 'shot_saved_list.bmp'))
    s.click(t0 + 28, screens.SAVED_OK)
    start = t0 + 100
    s.shot(start, os.path.join(out, 'shot_loaded.bmp'))
    end = start + args.minutes * 60
    monkey(s, lay, rnd, start + 2, end - 5, out)
    return end, {'load': names[args.row] if args.row < len(names) else None, 'row': args.row}


def sweep_points(lay, radius=0.8, n=8):
    """Minimap points on the ring where the players start, clear of the minimap's own
    buttons."""
    cx, cy, w = lay.minimap
    pts = []
    for k in range(n):
        a = 2 * math.pi * k / n
        x, y = cx + radius * w * math.cos(a), cy + radius * w * 0.47 * math.sin(a)
        if any(abs(x - bx) < 16 and abs(y - by) < 12 for bx, by in lay.minimap_buttons):
            x, y = cx + 0.4 * w * math.cos(a), cy + 0.4 * w * 0.47 * math.sin(a)
        pts.append((int(x), int(y)))
    return pts


# The players' colours on the minimap (exact palette entries), by player number.
PLAYER_RGB = {1: (0, 0, 255), 2: (255, 0, 0), 3: (0, 255, 0), 4: (255, 255, 0), 5: (0, 255, 255),
              6: (255, 0, 255), 7: (185, 185, 185), 8: (255, 130, 1)}


def wave(s, t, group, out, ewoks):
    """simonsays x EWOKS, H, a box select around the command center, Ctrl+group."""
    for e in range(ewoks):
        t = chat(s, t + 3, 'simonsays')
    s.at(t + 1, 'h')
    s.shot(t + 3, os.path.join(out, 'wave%d.bmp' % group))
    s.at(t + 5, 'ldown:5:25')
    s.at(t + 5.6, 'move:400:220')
    s.at(t + 6.2, 'move:795:420')
    s.at(t + 6.8, 'lup:795:420')
    s.at(t + 8, 'ctrl+%d' % group)
    s.shot(t + 9, os.path.join(out, 'wave%d_selected.bmp' % group))
    return t + 11


def plan_fight(s, lay, rnd, out, args, game, seed, t0):
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
    t, what = setup_fixed(s, lay, t0, out, others=n + 1, size=4, diff=0, teams=[1] * (n + 1) + [2],
                          loc=screens.LOCATIONS.index('Savannah'), reveal=2)
    start = t + 80
    s.shot(start, os.path.join(out, 'shot_start.bmp'))
    t = fast_speed(s, lay, start + 2) if args.speed == 'fast' else start + 2
    t = wave(s, t, 1, out, args.ewoks)
    end = start + args.minutes * 60
    return end, dict(what, goal='victory by fighting', enemy_player=n + 2, ewoks=args.ewoks, dwell=args.dwell,
                     wave_every=args.wave_every, live_from=round(t + 2))


def enemy_on_minimap(bmp, rgb):
    """The densest 12x12 cluster of the enemy's colour on the 800x600 minimap, and how
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


def fight_control(args, lay, out, log, live, proc, t_launch, what):
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
    pts = sweep_points(lay)
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
        for i, xy in enumerate(screens.SCORE_TABS[1:]):
            time.sleep(2)
            say('click:%d:%d' % xy)
            time.sleep(2)
            say('shot=' + os.path.join(out, 'score_tab%d.bmp' % (i + 1)))
        time.sleep(4)
        what['ended_by'] = 'the game over, then this tool'
        runner.kill_tree(proc)


ENDGAME = {'fight': plan_fight, 'load': plan_load, 'defeat': plan_defeat, 'allied': plan_allied, 'victory': plan_victory,
           'chain': plan_chain, 'missions': plan_missions, 'ingame-load': plan_ingame_load}
# The endgame modes that also work in Clone Campaigns (the others script the base game's screens).
CC_MODES = ('missions', 'ingame-load')


LOST = re.compile(r'stream\\lost\.mp3 -> ok', re.I)


def defeat_watch(log, live, out, proc, what):
    """missions: a mission the game declares lost (lost.mp3) ends its run: the defeat and the
    score screen shot, then this run's process tree stopped (by its PID). The rest of the
    script would otherwise click on through the menus, out of step."""
    while proc.poll() is None:
        time.sleep(2)
        try:
            text = open(log, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        if LOST.search(text):
            for shot, pause in (('defeat.bmp', 30), ('score_defeat.bmp', 4)):
                with open(live, 'a', newline='\n') as f:
                    f.write('0:shot=%s\n' % os.path.join(out, shot))
                time.sleep(pause)
            what['ended_by'] = 'a defeat, then this tool'
            runner.kill_tree(proc)
            return


def run_one(args, lay, i, mode, seed, out_root, game):
    rnd = random.Random(seed)
    name = '%02d-%s-%d' % (i, mode if mode not in ('missions', 'chain') else 'm%d' % args.first, seed)
    out = os.path.join(out_root, name)
    os.makedirs(out, exist_ok=True)
    s = Script()
    # Pick Screen Size on Options first so the run always matches `lay`, regardless of the hive's stored value.
    t0 = screens.set_screen(s, 12, args.screen or 800)
    if mode in ENDGAME:
        end, what = ENDGAME[mode](s, lay, rnd, out, args, game, seed, t0)
    else:
        end, what = plan_play(s, lay, rnd, out, args, mode, seed, t0)
    s.shot(end - 2, os.path.join(out, 'shot_end.bmp'))
    keys = s.write(os.path.join(out, 'keys.txt'))
    log = os.path.join(out, 'run.log')
    cmd = runner.host_cmd(args.run, args.exe, game, end + 5, keys, '--files', *args.extra.split())
    live = os.path.join(out, 'live.txt')
    if mode in ('fight', 'missions', 'chain'):
        open(live, 'w').close()
        cmd += ['--keys-live', live]
    meta = {'name': name, 'mode': mode, 'seed': seed, 'setup': what, 'minutes': args.minutes,
            'planned_seconds': round(end),
            'game': game, 'cmd': cmd, 'started': datetime.datetime.now().isoformat(timespec='seconds')}
    t_launch = time.time()
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        p = runner.popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=out)
        meta['pid'] = p.pid
        if mode == 'fight':
            fight_control(args, lay, out, log, live, p, t_launch, what)
        elif mode in ('missions', 'chain'):
            defeat_watch(log, live, out, p, what)
        rc = p.wait()
    meta['seconds'] = round(time.time() - t_launch)
    meta['exit'] = rc
    meta['outcome'] = runner.EXIT.get(rc, 'exit %d' % rc)
    meta.update(analyse(log, s.text()))
    if what.get('load_at'):                        # ingame-load: the save read back after the Load
        meta['save_reads'] = [o for o in meta['save_opens'] if o[1] == what['save'] and float(o[0]) >= what['load_at']]
    if what.get('ended_by') and meta['endings']:   # fight, missions: ended by this tool after the game over
        meta['outcome'] = 'game over: %s' % meta['endings'][-1][1]
    if mode in ('missions', 'chain'):
        meta['mission'] = what.get('missions', [None])[-1] if what.get('missions') else None
        meta['result'] = 'lost' if what.get('ended_by') else 'won' if meta['missions_won'] else 'not finished'
    runner.to_png(out)
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
    ap.add_argument('--first', type=int, default=1, help='missions, chain: the first mission played')
    ap.add_argument('--last', type=int, default=None, help="missions, chain: the last (default: the campaign's last)")
    ap.add_argument('--unlocked', action='store_true',
                    help='missions: the profile has the missions before --first open already: start there')
    ap.add_argument('--screen', type=int, default=None, choices=[800, 1024, 1280],
                    help='set the Screen Size first (main menu, Options); the HUD clicks follow it')
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
    ap.add_argument('--run', default=None, help='the host (default: the build of tools/swgb/runner.paths())')
    ap.add_argument('--exe', default=None, help='the game\'s exe (default: work/ of the main checkout)')
    ap.add_argument('--game', default=None,
                    help='the game folder (default: GAMES/soak-base or soak-cc, made on first use); '
                         'job k > 0 uses a copy, DIR-soakK (hard links)')
    ap.add_argument('--extra', default='',
                    help='more host options, e.g. "--firsthit 0x401000 0x651000" with a SWGB_TRACE build')
    ap.add_argument('--out', default=os.path.join(ROOT, 'work', 'soak',
                                                  datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    args = ap.parse_args()
    kind = 'cc' if args.cc else 'base'
    if args.cc:
        if args.mode in ENDGAME and args.mode not in CC_MODES:
            sys.exit('--cc: --mode %s plays the base game\'s screens; skirmish, monkey, campaign, %s'
                     % (args.mode, ', '.join(CC_MODES)))
        if args.screen and args.screen != 800:
            sys.exit('--cc: --screen is the base game\'s (its main menu\'s Options are elsewhere)')
    lay = screens.layout(kind, args.screen or 800)
    P = runner.paths()
    args.run = args.run or P.host(kind)
    args.exe = args.exe or P.exe(kind)
    args.game = args.game or P.game('soak', kind)
    args.civs = [c for c in args.civs.split(',') if c]
    args.my_civ = args.my_civ or None
    for c in args.civs + ([args.my_civ] if args.my_civ else []):
        if c not in lay.civ_items():
            sys.exit('no civilization %r: %s' % (c, ', '.join(lay.civ_items())))
    if args.campaign and args.campaign not in lay.campaigns:
        sys.exit('no campaign %r: %s' % (args.campaign, ', '.join(sorted(lay.campaigns))))
    if args.mode in ('missions', 'chain', 'ingame-load') and args.campaign:
        n = len(lay.missions[args.campaign])
        if not 1 <= args.first <= n or (args.last and not args.first <= args.last <= n):
            sys.exit('%s has missions 1-%d' % (args.campaign, n))
    sys.stdout.reconfigure(line_buffering=True)    # a run's line as it ends, into a redirected log too
    args.out = os.path.abspath(args.out)        # the game runs in its own folder
    args.game = os.path.abspath(args.game)
    seed = args.seed if args.seed is not None else random.randrange(1 << 20)
    rnd = random.Random(seed)
    os.makedirs(args.out, exist_ok=True)
    games = [args.game] + ['%s-soak%d' % (args.game.rstrip('\\/'), k) for k in range(1, args.jobs)]
    for g in games[1:]:
        runner.ensure_game(args.game, g)
    modes = ['skirmish', 'monkey', 'campaign']
    plan = [(i, args.mode if args.mode != 'mixed' else modes[i % 3], rnd.randrange(1 << 20), {})
            for i in range(args.runs)]
    if args.mode in ('missions', 'chain'):
        # A game of its own per mission, in turn, in the one folder (its profile opens each next
        # box): a mission lost before the cheat (a hero killed, a base razed while the monkey
        # plays) would leave the rest of one long script clicking out of step. The first run
        # also passes through what comes before --first.
        n = len(lay.missions[args.campaign or default_campaign(lay)])
        plan = [(k, args.mode, rnd.randrange(1 << 20), {'first': m, 'last': m, 'unlocked': args.unlocked or k > 0})
                for k, m in enumerate(range(args.first, min(args.last or n, n) + 1))]
        args.jobs, games = 1, games[:1]
    print('soak: %d run(s), %s, %g min each, %d at once, seed %d -> %s'
          % (len(plan), args.mode, args.minutes, args.jobs, seed, args.out))
    results, lock = [], threading.Lock()
    free = list(games)

    def worker(item):
        with lock:
            g = free.pop()
        try:
            m = run_one(argparse.Namespace(**dict(vars(args), **item[3])), lay, item[0], item[1], item[2], args.out, g)
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
    size = '%dx%d' % (lay.w, lay.h)
    for m in results:
        # The menus are 800x600 at every Screen Size: the game's own size is the one after them.
        m['screen_ok'] = not m['modes'] or (size in m['modes'] if args.screen else
                                             all(x == '800x600' for x in m['modes']))
    json.dump(sorted(results, key=lambda m: m['name']), open(os.path.join(args.out, 'summary.json'), 'w'), indent=1)
    bad = [m for m in results if (m['exit'] not in (0, 4) and not m['outcome'].startswith('game over'))
           or m['events'] or (args.screen and not m['screen_ok'])]
    hours = sum(m['seconds'] for m in results) / 3600
    print('%d run(s), %.2f game-hours, %d with a problem; %s' % (len(results), hours, len(bad), args.out))
    for m in results:
        if not m['screen_ok']:
            print('  warning: %s ran at %s: the HUD coordinates are for %s (--screen)'
                  % (m['name'], ', '.join(m['modes']), size))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
