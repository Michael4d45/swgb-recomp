"""The game's screens in game pixels, for both games and every screen size, and the routes
through them.

The menus, the setup screen and the score screen are 800x600 at every Screen Size. In a
game the HUD is the game's own art at its three sizes, 800, 1024 and 1280 wide. At a
widescreen WxH (--res) the host lays the HUD out at a base size (hud_base) and puts it at
the bottom, centred (src/runtime/widescreen.c). The game menu and the Options dialog are
centred on the screen. The Save and Load screens fill the largest of the game's sizes
that fits, centred.

layout(game, size) holds everything that depends on the game or the size. A tool takes
one Layout and passes it to whatever builds its --keys script.
"""
import os
from dataclasses import dataclass

# ---- the menus (800x600 at every size) -------------------------------------------------------
MAIN_SINGLE = (345, 250)            # main menu: Single Player
MAIN_OPTIONS = (40, 490)            # main menu: Options
MAIN_EXIT = (40, 565)               # main menu: Exit
# The main menu's Options panel: Screen Size. The third choice is 1280x1024, or WxH with --res.
SCREEN_RADIO = {800: (453, 383), 1024: (453, 413), 1280: (453, 443)}
SETUP_START = (118, 568)            # Standard Game: Start Game
# The setup screen's drop-downs: the arrow, then the first item and the step.
DD_OTHERS = ((770, 211), (690, 236), 21)        # 1..7 computer players
DD_LOCATION = ((770, 124), (680, 149), 21)      # Forced Deployment .. Shoreline (16 shown)
DD_SIZE = ((770, 153), (690, 178), 21)          # Tiny, Small, Medium, Normal, Large, Giant
DD_DIFFICULTY = ((770, 182), (690, 207), 21)    # Easiest .. Hardest
DD_REVEAL = ((770, 297), (690, 322), 21)        # Normal, Explored, All Visible
# The civilization drop-downs: the player's row, then the computers' rows. Each row's arrow is
# at x 249; its items at x 290, from the row + 25, 21 apart.
MY_CIV_ROW, CIV_ROW0, CIV_ROW_STEP = 94, 169, 24
CIV_ARROW_X, CIV_ITEM_X = 249, 290
TEAM_X = 472                        # the setup screen's Team buttons: each click is the next team
LOCATIONS = ['Forced Deployment', 'Forest', 'Fortress', 'Ice Lake', 'Islands', 'LandMass', 'Large Sea',
             'Motherlode', 'Nova Assault', 'Nova Lake', 'Precipice', 'Raiders', 'Rivers', 'Savannah',
             'Sea', 'Shoreline']
SIZES = ['Tiny', 'Small', 'Medium', 'Normal', 'Large', 'Giant']
DIFFICULTIES = ['Easiest', 'Easy', 'Moderate', 'Hard', 'Hardest']
CAMPAIGN_MISSION1 = (150, 140)      # a campaign screen's first mission
CAMPAIGN_OK = (318, 481)            # the difficulty dialog (Easy is chosen)
CAMPAIGN_MENU = (90, 25)            # a campaign screen's Campaign Menu: back to the campaigns
OBJECTIVES_OK = (589, 502)          # a mission's opening Objectives (Escape closes it too)
SAVED_X, SAVED_ROW0, SAVED_STEP, SAVED_OK = 300, 121, 21, (178, 568)   # Saved Game: its list, OK
SCORE_TABS = [(130, 562), (238, 562), (346, 562), (454, 562), (562, 562), (672, 562)]
SCORE_MENU = (710, 514)             # the score screen: Main Menu (Scenario Menu in a campaign)
WIN_CHEAT = 'skywalker'             # typed in the chat: an instant victory

# Each campaign's mission boxes on its campaign screen, in order. A box shows once the mission
# before it is won (Basic Training's are all open from the start). Found by winning each
# mission in turn and diffing the campaign screen before and after (2026-11-26).
BASE_MISSIONS = {
    'Attichitcuk': [(105, 160), (265, 150), (430, 200), (270, 295), (95, 350), (245, 460), (405, 430)],
    'OOM-9': [(150, 140), (377, 149), (133, 265), (368, 265), (90, 430), (255, 420), (428, 440)],
    'Boss Nass': [(150, 140), (264, 156), (398, 143), (110, 310), (323, 326), (120, 455), (311, 466)],
    'Chewbacca': [(150, 140), (382, 146), (100, 286), (250, 277), (411, 293), (115, 455), (335, 453)],
    'Darth Vader': [(150, 140), (401, 132), (116, 290), (300, 270), (445, 288), (181, 490), (391, 451)],
    'Princess Leia': [(150, 140), (296, 147), (430, 150), (114, 293), (286, 308), (450, 293), (133, 458),
                      (360, 460)],
}
CC_MISSIONS = {
    'Confederacy': [(150, 140), (397, 128), (110, 280), (284, 280), (450, 276), (178, 455), (408, 458)],
    'Galactic Republic': [(150, 140), (390, 119), (113, 285), (278, 282), (447, 281), (187, 470), (398, 485)],
}

# What differs between the games on the menus. Clone Campaigns' Single Player menu: Episode
# I,IV,V,VI Campaigns (the base game's six), Episode II Campaigns, Standard Game, Custom
# Campaign, Saved Game; its setup screen has nine civilizations; its Episode II campaign
# screen has two campaigns, and Galactic Republic's planet answers once Confederacy is won.
GAMES = {
    'base': dict(sp_campaigns=(603, 112), sp_standard=(600, 168), sp_saved=(603, 286), main_multi=(275, 390),
                 civs=('Galactic Empire', 'Gungans', 'Rebel Alliance', 'Royal Naboo', 'Trade Federation',
                       'Wookiees'),
                 campaigns={'Attichitcuk': (160, 322), 'OOM-9': (187, 212), 'Boss Nass': (272, 118),
                            'Darth Vader': (512, 118), 'Princess Leia': (595, 212), 'Chewbacca': (625, 322)},
                 missions=BASE_MISSIONS, requires={}, save_ext='.gax'),
    'cc': dict(sp_campaigns=(603, 170), sp_standard=(603, 228), sp_saved=(603, 344), main_multi=(290, 405),
               civs=('Confederacy', 'Galactic Empire', 'Gungans', 'Rebel Alliance', 'Republic', 'Royal Naboo',
                     'Trade Federation', 'Wookiees'),
               campaigns={'Confederacy': (225, 315), 'Galactic Republic': (555, 315)},
               missions=CC_MISSIONS, requires={'Galactic Republic': 'Confederacy'}, save_ext='.ga1'),
}

# ---- the HUD at its three base sizes ----------------------------------------------------------
# The places are the panels' centres in SWGB_RES_DUMP's panel tree, at --res equal to each base
# (1024x600 for the 800 base, moved back by its 112 columns), 2026-11-27. The minimap is
# (centre x, centre y, reach): its diamond's centre, and how far out a click may go. The
# right-hand column is Menu, Objectives, Chat, Diplomacy, Tech Tree, top to bottom. `rows`
# is the interface's height under the map view.
HUD = {
    800: dict(screen=(800, 600), rows=170, map_view=(0, 20, 799, 425),
              grid=((25, 70, 115, 152), (452, 495, 538, 577)),
              right=(768, (476, 501, 526, 551, 576)),
              minimap=(586, 528, 120), minimap_panel=(453, 461, 266, 134),
              minimap_buttons=((664, 480), (692, 480), (470, 565)),
              top_icons=((190, 441), (212, 441), (233, 441), (254, 441)),
              command_center=(400, 170)),
    1024: dict(screen=(1024, 768), rows=200, map_view=(0, 20, 1023, 560),
               grid=((37, 81, 126, 170), (605, 648, 691, 733)),
               right=(984, (626, 653, 678, 705, 731)),
               minimap=(770, 681, 150), minimap_panel=(607, 599, 326, 164),
               minimap_buttons=((860, 624), (890, 624), (636, 728)),
               top_icons=((268, 580), (291, 580), (314, 580), (337, 580)),
               command_center=(500, 300)),
    1280: dict(screen=(1280, 1024), rows=250, map_view=(0, 20, 1279, 760),
               grid=((47, 98, 149, 200), (822, 873, 924, 975)),
               right=(1231, (850, 879, 908, 936, 966)),
               minimap=(966, 919, 200), minimap_panel=(769, 820, 394, 198),
               minimap_buttons=((1085, 857), (1113, 857), (803, 975)),
               top_icons=((327, 795), (354, 795), (382, 795), (410, 795)),
               command_center=(610, 340)),
}
HUD_BUTTONS = ('menu', 'objectives', 'chat', 'diplomacy', 'techtree')
# The game menu's buttons, from the screen's middle (x + 8); "quit and load?" / "quit?" Yes.
GAME_MENU = {'quit': -122, 'save': -82, 'load': -49, 'restart': -16, 'options': 17, 'about': 50, 'cancel': 90}
YES = (-95, 28)
# The in-game Options dialog at 800x600, centred on a bigger screen: Speed Fast, OK and the
# Screen Size radio buttons (the third: 1280x1024, or WxH with --res).
OPTIONS_FAST, OPTIONS_OK = (135, 248), (305, 455)
OPTIONS_RADIO = {800: (133, 318), 1024: (133, 353), 1280: (133, 388)}
# The Save and Load screens in 800x600 units, scaled to the size they are shown at: the name
# field, Save's OK, Load's Cancel, and the Load list (x, first row, step).
SAVE_NAME, SAVE_OK, LOAD_CANCEL = (403, 76), (178, 568), (631, 568)
LOAD_LIST = (300, 121, 21)


def hud_base(w, h):
    """The base size the HUD is laid out at on a w x h screen (widescreen.c's g_base_w)."""
    return 1280 if w >= 1280 and h >= 720 else 1024 if w >= 1024 and h >= 700 else 800


def fit(w, h):
    """The largest of the game's own sizes within w x h: where the Save and Load screens go."""
    return (1280, 1024) if w >= 1280 and h >= 1024 else (1024, 768) if w >= 1024 and h >= 768 else (800, 600)


@dataclass(frozen=True)
class Layout:
    game: str                       # 'base' or 'cc'
    w: int                          # the screen in the game
    h: int
    base: int                       # the HUD's base width (hud_base)
    # the menus (800x600)
    sp_campaigns: tuple
    sp_standard: tuple
    sp_saved: tuple
    main_multi: tuple
    civs: tuple
    campaigns: dict
    missions: dict
    requires: dict
    save_ext: str
    # in the game
    map_view: tuple
    command_grid: tuple
    hud: dict
    minimap: tuple
    minimap_panel: tuple
    minimap_buttons: tuple
    top_icons: tuple
    command_center: tuple           # where the view starts; None at a widescreen size (unmeasured)
    game_menu: dict
    yes: tuple
    options_fast: tuple
    options_ok: tuple
    options_radio: dict
    save_name: tuple
    save_ok: tuple
    load_cancel: tuple
    load_list: tuple                # x, the first row, the step (a float), OK

    def civ_items(self):
        """The civilization drop-down's items: the civilizations, then Random."""
        return self.civs + ('Random',)


def layout(game='base', size=800):
    """The screens of GAME ('base' or 'cc') with the game at SIZE: 800, 1024, 1280 (the game's
    own sizes) or (W, H)."""
    w, h = HUD[size]['screen'] if size in HUD else size
    bw = hud_base(w, h)
    t = HUD[bw]
    dx, dy = (w - bw) // 2, h - t['screen'][1]           # the HUD: at the bottom, centred

    def hud(xy):
        return xy[0] + dx, xy[1] + dy

    cx, cy = (w - 800) // 2, (h - 600) // 2               # a centred 800x600 dialog
    fw, fh = fit(w, h)
    ox, oy = (w - fw) // 2, (h - fh) // 2                 # the Save and Load screens

    def full(xy):
        return ox + xy[0] * fw // 800, oy + xy[1] * fh // 600

    xs, ys = t['grid']
    rx, rys = t['right']
    mx, my, reach = t['minimap']
    px, py, pw, ph = t['minimap_panel']
    x0, y0, x1, y1 = t['map_view']
    lx, l0, ls = LOAD_LIST
    return Layout(
        game=game, w=w, h=h, base=bw, **GAMES[game],
        map_view=(x0, y0, x1 + 2 * dx, y1 + dy),
        command_grid=tuple(hud((x, y)) for y in ys for x in xs),
        hud={name: hud((rx, y)) for name, y in zip(HUD_BUTTONS, rys)},
        minimap=hud((mx, my)) + (reach,), minimap_panel=hud((px, py)) + (pw, ph),
        minimap_buttons=tuple(hud(xy) for xy in t['minimap_buttons']),
        top_icons=tuple(hud(xy) for xy in t['top_icons']),
        command_center=t['command_center'] if (w, h) == t['screen'] else None,
        game_menu={k: (w // 2 + 8, h // 2 + v) for k, v in GAME_MENU.items()},
        yes=(w // 2 + YES[0], h // 2 + YES[1]),
        options_fast=(OPTIONS_FAST[0] + cx, OPTIONS_FAST[1] + cy), options_ok=(OPTIONS_OK[0] + cx, OPTIONS_OK[1] + cy),
        options_radio={k: (x + cx, y + cy) for k, (x, y) in OPTIONS_RADIO.items()},
        save_name=full(SAVE_NAME), save_ok=full(SAVE_OK), load_cancel=full(LOAD_CANCEL),
        load_list=(ox + lx * fw // 800, oy + l0 * fh / 600, ls * fh / 600, full(SAVE_OK)))


# ---- routes -----------------------------------------------------------------------------------
# Each takes the script, the time to start and what it needs; it returns the time it is done.
def set_screen(s, t, size):
    """The main menu's Options panel: Screen Size SIZE (800, 1024, 1280: the third, WxH with
    --res). A game folder keeps it in its own registry hive (--multi); the game stores it only
    when it exits normally. Returns when Single Player can be clicked."""
    s.click(t, MAIN_OPTIONS)
    s.click(t + 7, SCREEN_RADIO[size])
    return t + 12


def to_setup(s, lay, t, size=None):
    """The main menu [-> Options -> Screen Size SIZE] -> Single Player -> Standard Game.
    Returns when the setup screen takes clicks."""
    if size:
        t = set_screen(s, t, size)
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, lay.sp_standard)
    return t + 33


def to_skirmish(s, lay, t, size=None):
    """to_setup, then Start Game with the setup the folder last used. Returns the time of the
    Start Game click: the map is up 45-70 s later."""
    t = to_setup(s, lay, t, size)
    s.click(t, SETUP_START)
    return t


def pick(s, t, dd, k):
    """Open a drop-down and click its k-th item. Returns 2.5 s later."""
    (ax, ay), (ix, iy), step = dd
    s.click(t, (ax, ay))
    s.click(t + 1.2, (ix, iy + step * k))
    return t + 2.5


def set_civ(s, lay, t, row_y, civ):
    """The civilization drop-down of the setup screen's row at ROW_Y: CIV (or 'Random')."""
    s.click(t, (CIV_ARROW_X, row_y))
    s.click(t + 1.2, (CIV_ITEM_X, row_y + 25 + 21 * lay.civ_items().index(civ)))
    return t + 2.5


def computer_row(p):
    """The setup screen's row of computer player p (0 = the first)."""
    return CIV_ROW0 + CIV_ROW_STEP * p


def fast_speed(s, lay, t):
    """The game menu -> Options -> Speed: Fast -> OK. Returns 6 s later."""
    s.click(t, lay.hud['menu'])
    s.click(t + 2, lay.game_menu['options'])
    s.click(t + 4, lay.options_fast)
    s.click(t + 5, lay.options_ok)
    return t + 6


def save_game(s, lay, t, name):
    """The game menu -> Save -> a name -> OK (the name field has the focus). Returns 20 s later:
    saving runs the whole world through the writer."""
    s.click(t, lay.hud['menu'])
    s.click(t + 2, lay.game_menu['save'])
    for k, ch in enumerate(name):
        s.at(t + 5 + 0.3 * k, ch)
    s.click(t + 6 + 0.3 * len(name), lay.save_ok)
    return t + 20


def chat(s, t, words):
    """Enter, the words, Enter: the chat line, where the cheats go too."""
    s.at(t, 'enter')
    for k, ch in enumerate(words):
        s.at(t + 1 + 0.3 * k, 'space' if ch == ' ' else ch)
    s.at(t + 2 + 0.3 * len(words), 'enter')
    return t + 3 + 0.3 * len(words)


def saved_game(s, lay, t, row):
    """The main menu -> Single Player -> Saved Game -> the row'th save -> OK. Returns the time
    of OK."""
    s.click(t, MAIN_SINGLE)
    s.click(t + 15, lay.sp_saved)
    s.click(t + 25, (SAVED_X, SAVED_ROW0 + SAVED_STEP * row))
    s.click(t + 28, SAVED_OK)
    return t + 28


def quit_game(s, lay, t):
    """The HUD's Menu, Quit Current Game, Yes: the score screen. Returns the time of Yes."""
    s.click(t, lay.hud['menu'])
    s.click(t + 5, lay.game_menu['quit'])
    s.click(t + 9, lay.yes)
    return t + 9


def saved_names(lay, game_dir):
    """The game's Saved Game list as it shows it: the saves and the recorded games (base
    .gax/.mgx, Clone Campaigns .ga1/.mg1), by name, alphabetical. Clone Campaigns' first
    in-game Load counted saves alone and loaded the recorded game a row up instead."""
    exts = (lay.save_ext, lay.save_ext.replace('.ga', '.mg'))
    saved = os.path.join(game_dir, 'SaveGame')
    return sorted({f[:-4].lower() for f in os.listdir(saved) if f.lower().endswith(exts)})


def saved_row(lay, game_dir, name):
    """The row save NAME will have in the game's Saved Game lists."""
    return sorted(set(saved_names(lay, game_dir)) | {name}).index(name)
