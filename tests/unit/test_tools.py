"""The tools' own tests, no game needed: tools/swgb's screens, paths and game folders,
tools/soak.py's scripts and log reading, tools/lift_check.py, tools/smooth_check.py,
tools/test.py's image and --fps-log readers, perf_ab.py and mp_pair.py's options and sync compare;
every environment variable the sources read is in docs/building.md.

    py -3 -m unittest -v tests.unit.test_tools        (tools/test.py --level unit runs it)
"""
import argparse
import json
import os
import random
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import lift_check  # noqa: E402
import res_check  # noqa: E402
import smooth_check  # noqa: E402
import mp_pair  # noqa: E402
import perf_ab  # noqa: E402
import soak  # noqa: E402
import test as runner  # noqa: E402
from swgb import runner as swgb_runner, screens  # noqa: E402

BASE, CC = screens.layout('base'), screens.layout('cc')

LOG = r"""  registry: HKCU is private to A:\Games\star\games\testing-base-0
entering 0x00632881
[host] open data\genie.dat -> ok
SetDisplayMode(800, 600, 8)
[keys] 20.0s click 345,250
[keys] 21.0s click 600,168
[host] open sound\stream\won1.mp3 -> ok
[host] open campaign\media\xc1s5_end.mm -> ok
[host] open savegame\soak7.gax -> ok
[keys] 30.0s key 48
ICALL: unresolved VA 0x005648A0 from 0x00406E20
  nearest below: sub_0056480C (+0x94)
[watchdog] 60 s: stopping
"""


class Screens(unittest.TestCase):
    """tools/swgb/screens.py: the layouts and the routes."""

    def test_route_picks_each_screen_size(self):
        # the main menu's Options, the size's own radio button, then Single Player: in that order,
        # far enough apart for each screen to come up
        for size in (800, 1024, 1280):
            s = swgb_runner.Script()
            start = screens.to_skirmish(s, BASE, 12, size)
            clicks = [(t, w) for t, w in s.steps]
            self.assertEqual([w for _, w in clicks[:3]],
                             ['click:%d:%d' % xy for xy in (screens.MAIN_OPTIONS, screens.SCREEN_RADIO[size],
                                                            screens.MAIN_SINGLE)])
            self.assertEqual(clicks[-1], (start, 'click:%d:%d' % screens.SETUP_START))
            self.assertTrue(all(b[0] - a[0] >= 5 for a, b in zip(clicks, clicks[1:])), clicks)

    def test_present_check_skirmish_sets_size_and_easy_opponent(self):
        try:
            import present_check
        except ImportError:
            self.skipTest('no numpy/PIL')
        s = present_check.Script('out')
        end = s.skirmish(20, 800)
        clicks = [w for _, w in s.steps]
        self.assertLess(clicks.index('click:%d:%d' % screens.SCREEN_RADIO[800]),
                        clicks.index('click:%d:%d' % screens.MAIN_SINGLE))
        i_start = clicks.index('click:%d:%d' % screens.SETUP_START)
        for dd in (screens.DD_OTHERS, screens.DD_DIFFICULTY):
            self.assertIn('click:%d:%d' % dd[0], clicks[:i_start])
            self.assertIn('click:%d:%d' % dd[1], clicks[:i_start])
        t_start, w_start = s.steps[-1]
        self.assertEqual(w_start, 'click:%d:%d' % screens.SETUP_START)
        self.assertEqual(end, t_start + present_check.LOAD)

    def test_hud_inside_the_screen_and_its_panels(self):
        # every HUD place is on the interface band under the map, the minimap's centre in its panel
        for size in (800, 1024, 1280, (1920, 1080), (1366, 768), (1024, 600)):
            lay = screens.layout('base', size)
            band = lay.h - screens.HUD[lay.base]['rows']
            for name, (x, y) in list(lay.hud.items()) + [('grid', xy) for xy in lay.command_grid]:
                self.assertTrue(0 <= x < lay.w and band <= y < lay.h, (size, name, x, y))
            (mx, my, reach), (px, py, pw, ph) = lay.minimap, lay.minimap_panel
            self.assertEqual((mx, my), (px + pw // 2, py + ph // 2), size)
            self.assertLessEqual(reach, pw // 2 + 4, size)

    def test_widescreen_moves_the_base_hud(self):
        # WxH: the base layout at the bottom, centred; the game's own sizes: the art's places
        self.assertEqual(BASE.hud['menu'], (768, 476))
        self.assertEqual(screens.layout('base', 1024).hud['techtree'], (984, 731))
        self.assertEqual(screens.layout('base', (1920, 1080)).hud['menu'], (1231 + 320, 850 + 56))
        self.assertEqual(screens.layout('base', (1364, 768)).minimap[:2], (966 + 42, 919 - 256))
        self.assertIsNone(screens.layout('base', (1920, 1080)).command_center)

    def test_dialogs_centred_and_save_screens_scaled(self):
        lay = screens.layout('base', 1024)
        self.assertEqual(lay.game_menu['save'], (520, 302))              # centred: +112, +84
        self.assertEqual(lay.game_menu['load'], (520, 335))
        self.assertEqual(lay.yes, (417, 412))
        self.assertEqual(lay.options_radio[1280], (133 + 112, 388 + 84))
        self.assertEqual(lay.save_ok, (227, 727))                         # scaled by 1.28
        wide = screens.layout('base', (1920, 1080))
        self.assertEqual(wide.save_ok, (320 + 284, 28 + 969))             # the 1280x1024 screen, centred
        self.assertEqual(screens.fit(1364, 768), (1024, 768))
        self.assertEqual(screens.fit(1280, 720), (800, 600))

    def test_games_differ_on_the_menus_only(self):
        self.assertEqual(sorted(CC.missions), ['Confederacy', 'Galactic Republic'])
        self.assertEqual(CC.save_ext, '.ga1')
        self.assertEqual(CC.civ_items()[-1], 'Random')
        self.assertEqual(len(CC.civ_items()), 9)
        for boxes in list(CC.missions.values()) + list(BASE.missions.values()):
            if boxes is not BASE.missions['Attichitcuk']:
                self.assertEqual(boxes[0], screens.CAMPAIGN_MISSION1)
        self.assertEqual(CC.hud, BASE.hud)
        self.assertNotEqual(CC.sp_standard, BASE.sp_standard)

    def test_layout_is_frozen(self):
        import dataclasses
        with self.assertRaises(dataclasses.FrozenInstanceError):
            BASE.save_ext = '.ga1'


def write(path, text):
    with open(path, 'w') as f:
        f.write(text)


class Paths(unittest.TestCase):
    """tools/swgb/runner.py's paths() and game folders."""

    def checkout(self, d, local=None, worktree=False):
        """A checkout in d/main (and d/wt, a worktree of it), with tools/local.json."""
        main = os.path.join(d, 'main')
        os.makedirs(os.path.join(main, 'tools'))
        os.makedirs(os.path.join(main, '.git', 'worktrees', 'wt'))
        if local is not None:
            write(os.path.join(main, 'tools', 'local.json'), json.dumps(local))
        if not worktree:
            return main
        wt = os.path.join(d, 'wt')
        os.makedirs(os.path.join(wt, 'tools'))
        write(os.path.join(wt, '.git'), 'gitdir: %s\n' % os.path.join(main, '.git', 'worktrees', 'wt'))
        write(os.path.join(main, '.git', 'worktrees', 'wt', 'commondir'), '../..\n')
        return wt

    def test_checkout_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            root = self.checkout(d)
            p = swgb_runner.paths(env={}, root=root)
            self.assertIsNone(p.star)
            self.assertEqual(p.main, root)
            self.assertEqual(p.games, os.path.join(root, 'work', 'games'))
            self.assertEqual(p.results, os.path.join(root, 'work', 'test-results'))
            self.assertEqual(p.template, os.path.join(root, 'game'))
            self.assertEqual(p.exe('cc'), os.path.join(root, 'work', 'battlegrounds_x1.exe'))
            self.assertEqual(os.path.dirname(p.build), root)
            self.assertGreaterEqual(p.max_jobs, 1)

    def test_star_from_env_or_local_json(self):
        with tempfile.TemporaryDirectory() as d:
            star = os.path.join(d, 'star')
            root = self.checkout(d, local={'star': star, 'max_jobs': 3})
            p = swgb_runner.paths(env={}, root=root)
            self.assertEqual(p.main, os.path.join(star, 'swgb-recomp'))
            self.assertEqual(p.build, os.path.join(star, 'builds', 'integration'))
            self.assertIsNone(p.cc_profiles)                             # none made: the install's own
            self.assertEqual(p.max_jobs, 3)
            os.makedirs(os.path.join(star, 'games', 'clone-campaigns'))
            self.assertEqual(swgb_runner.paths(env={}, root=root).cc_profiles,
                             os.path.join(star, 'games', 'clone-campaigns'))
            other = os.path.join(d, 'other')
            p = swgb_runner.paths(env={'SWGB_STAR': other, 'SWGB_MAX_JOBS': '2'}, root=root)
            self.assertEqual((p.games, p.max_jobs), (os.path.join(other, 'games'), 2))

    def test_worktree_reads_the_main_checkouts_local_json(self):
        with tempfile.TemporaryDirectory() as d:
            wt = self.checkout(d, local={'star': os.path.join(d, 'star'), 'build': os.path.join(d, 'b')}, worktree=True)
            p = swgb_runner.paths(env={}, root=wt)
            self.assertEqual((p.star, p.build), (os.path.join(d, 'star'), os.path.join(d, 'b')))
            write(os.path.join(wt, 'tools', 'local.json'), '{}')    # its own wins
            self.assertIsNone(swgb_runner.paths(env={}, root=wt).star)

    def test_unknown_key_is_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            root = self.checkout(d, local={'stars': 'x'})
            with self.assertRaises(ValueError):
                swgb_runner.paths(env={}, root=root)

    def test_to_png_removes_the_bmp(self):
        # PIL maps a BMP's file: a name kept for the image held it, and Windows refused the remove
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            im = Image.new('P', (800, 600), 7)
            im.putpalette([i for i in range(256) for _ in range(3)])
            im.save(os.path.join(d, 'shot.bmp'))                           # as the host's shots are
            self.assertEqual(swgb_runner.to_png(d), [os.path.join(d, 'shot.png')])
            self.assertEqual(os.listdir(d), ['shot.png'])
            with Image.open(os.path.join(d, 'shot.png')) as png:
                self.assertEqual(png.getpixel((5, 5)), 7)

    def test_ensure_game_completes_a_cut_copy(self):
        with tempfile.TemporaryDirectory() as d:
            tpl, dst, prof = os.path.join(d, 'tpl'), os.path.join(d, 'g'), os.path.join(d, 'prof')
            for sub in ('Data', 'SaveGame'):
                os.makedirs(os.path.join(tpl, sub))
            os.makedirs(prof)
            for f in ('Data/genie.dat', 'Data/sounds.drs', 'SaveGame/a.gax', 'player.nfx'):
                write(os.path.join(tpl, f), 'template')
            write(os.path.join(prof, 'player.nf1'), '800x600')
            os.makedirs(os.path.join(dst, 'Data'))                          # a copy cut short
            os.link(os.path.join(tpl, 'Data', 'genie.dat'), os.path.join(dst, 'Data', 'genie.dat'))
            swgb_runner.ensure_game(tpl, dst, prof, reset_screen=False)
            self.assertTrue(os.path.exists(os.path.join(dst, 'Data', 'sounds.drs')))
            self.assertTrue(os.path.exists(os.path.join(dst, swgb_runner.MARKER)))
            with open(os.path.join(dst, 'player.nf1')) as f:
                self.assertEqual(f.read(), '800x600')
            self.assertEqual(os.stat(os.path.join(dst, 'SaveGame', 'a.gax')).st_nlink, 1)   # a copy, not a link
            self.assertEqual(os.stat(os.path.join(dst, 'Data', 'sounds.drs')).st_nlink, 2)
            with self.assertRaises(FileNotFoundError):                      # no profile there: said, not skipped
                swgb_runner.ensure_game(tpl, os.path.join(d, 'g2'), os.path.join(d, 'none'), reset_screen=False)

    @staticmethod
    def _make_profile(players):
        import struct
        import zlib
        buf = bytearray(b'1.06' + struct.pack('<fIII', 1.1, 0, 0, len(players)))
        for name, campaigns, w, h, tail_flags in players:
            nb = name.encode('ascii')[:254]
            buf += nb + b'\x00' * (255 - len(nb)) + struct.pack('<III', 0, 0, len(campaigns))
            for cname, scens in campaigns:
                cb = cname.encode('ascii')[:254]
                buf += cb + b'\x00' * (255 - len(cb)) + struct.pack('<IIIII', 0, len(scens), 0, 1, 0) + bytes(scens)
            block = bytearray(306)
            struct.pack_into('<HH', block, 294, w, h)
            block[298:306] = tail_flags
            buf += block
        comp = zlib.compressobj(zlib.Z_DEFAULT_COMPRESSION, zlib.DEFLATED, -15)
        return comp.compress(bytes(buf)) + comp.flush(), len(buf)

    def test_reset_profile_resolution(self):
        import struct
        import zlib
        with tempfile.TemporaryDirectory() as d:
            # 1-player (0 campaigns), 1-player (1 campaign with 7 scenarios), and 2-player profiles
            p1, n1 = self._make_profile([('michael', [], 1280, 1024, b'\x01\x00\x02\x00\x00\x00\x00\x00')])
            p2, n2 = self._make_profile([('wide', [('1CAM1', [1, 2, 3, 4, 5, 6, 7])], 2560, 1440, b'\x01\x00\x02\x00\x00\x00\x00\x01')])
            p3, n3 = self._make_profile([('a', [], 1280, 1024, b'\x00' * 8), ('b', [('c', [0, 1])], 1024, 768, b'\x01' * 8)])
            p0, _ = self._make_profile([])
            tpl = os.path.join(d, 'tpl')
            dst = os.path.join(d, 'dst')
            os.makedirs(tpl)
            for fname, raw in (('player.nfx', p1), ('player.nf1', p2), ('two.nfx', p3), ('empty.nfx', p0), ('junk.nfx', b'not-deflate')):
                with open(os.path.join(tpl, fname), 'wb') as fh:
                    fh.write(raw)
            swgb_runner.ensure_game(tpl, dst, hive_root=None)
            # Template stays untouched; dst player.nf* are reset to 800x600 with length/flags intact
            self.assertEqual(open(os.path.join(tpl, 'player.nfx'), 'rb').read(), p1)
            for fname, expected_len, n_camp in (('player.nfx', n1, 0), ('player.nf1', n2, 1)):
                dec = zlib.decompress(open(os.path.join(dst, fname), 'rb').read(), -15)
                self.assertEqual(len(dec), expected_len)
                self.assertEqual(struct.unpack_from('<HH', dec, len(dec) - 12), (800, 600))
                self.assertEqual(struct.unpack_from('<i', dec, 20 + 263)[0], n_camp)
            self.assertFalse(swgb_runner.reset_profile_resolution(os.path.join(dst, 'player.nfx')))
            self.assertTrue(swgb_runner.reset_profile_resolution(os.path.join(dst, 'two.nfx')))
            dec3 = zlib.decompress(open(os.path.join(dst, 'two.nfx'), 'rb').read(), -15)
            self.assertEqual(len(dec3), n3)
            self.assertEqual(struct.unpack_from('<HH', dec3, len(dec3) - 12), (800, 600))
            self.assertFalse(swgb_runner.reset_profile_resolution(os.path.join(dst, 'empty.nfx')))
            self.assertFalse(swgb_runner.reset_profile_resolution(os.path.join(dst, 'junk.nfx')))

    def test_seed_hive_screen_size(self):
        if not swgb_runner.WIN:
            return
        import winreg
        scratch = r'Software\swgb-recomp\unit-test-py-%d' % os.getpid()
        with tempfile.TemporaryDirectory() as d:
            tpl, dst = os.path.join(d, 'tpl'), os.path.join(d, 'dst')
            os.makedirs(tpl)
            try:
                swgb_runner.ensure_game(tpl, dst, hive_root=scratch)
                for sub in swgb_runner.GAME_REG_KEYS:
                    key = '%s\\%s\\%s' % (scratch, swgb_runner.hive_name(dst), sub)
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
                        self.assertEqual(winreg.QueryValueEx(k, 'Screen Size')[0], 800)
            finally:
                # Delete scratch tree recursively without touching real hives
                def del_tree(root, path):
                    try:
                        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as k:
                            while True:
                                try:
                                    sub = winreg.EnumKey(k, 0)
                                except OSError:
                                    break
                                del_tree(root, path + '\\' + sub)
                        winreg.DeleteKey(root, path)
                    except OSError:
                        pass
                del_tree(winreg.HKEY_CURRENT_USER, scratch)


class MachinePaths(unittest.TestCase):
    def test_find(self):
        import io
        import machine_paths
        lines = {'tools/x.py': ["help=r'A:\\Games\\star\\builds'\n", "print('disc:\\n')\n"],
                 'tools/setup.ps1': ['Its drive (E:\\), an .iso\n']}
        found = machine_paths.find(sorted(lines), lambda f: io.StringIO(''.join(lines[f])))
        self.assertEqual([(f, n) for f, n, _ in found], [('tools/x.py', 1)])   # a word before X: is no drive


class SoakTools(unittest.TestCase):
    def test_script_sorted(self):
        s = soak.Script()
        s.at(5, 'b')
        s.click(1, (10, 20))
        s.at(3, 'shot=x.bmp')
        self.assertEqual(s.text().splitlines(), ['1:click:10:20', '3:shot=x.bmp', '5:b'])

    def test_analyse(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'run.log')
            with open(p, 'w') as f:
                f.write(LOG)
            m = soak.analyse(p, '')
        self.assertEqual(len(m['events']), 1)
        self.assertIn('0x005648A0', m['events'][0]['line'])
        self.assertEqual(len(m['events'][0]['keys_before']), 3)
        self.assertEqual(m['modes'], ['800x600'])
        self.assertTrue(m['watchdog'])
        self.assertEqual(m['endings'], [['21', 'victory']])
        self.assertEqual(m['missions_won'], ['xc1s5'])
        self.assertEqual(m['save_opens'], [['21', 'soak7']])

    def test_pick(self):
        s = soak.Script()
        t = screens.pick(s, 10, screens.DD_SIZE, 2)
        self.assertEqual(t, 12.5)
        (ax, ay), (ix, iy), step = screens.DD_SIZE
        self.assertEqual(s.steps, [(10, 'click:%d:%d' % (ax, ay)), (11.2, 'click:%d:%d' % (ix, iy + 2 * step))])

    def test_skirmish_civs_are_parameters(self):
        s = soak.Script()
        _, what = soak.setup_skirmish(s, CC, random.Random(3), 20, my_civ='Republic', civs=['Confederacy'])
        self.assertEqual(what['my_civ'], 'Republic')
        self.assertEqual(set(what['civs']), {'Confederacy'})
        item = 'click:%d:%d' % (screens.CIV_ITEM_X, screens.MY_CIV_ROW + 25 + 21 * CC.civ_items().index('Republic'))
        self.assertIn(item, [w for _, w in s.steps])
        _, drawn = soak.setup_skirmish(soak.Script(), CC, random.Random(3), 20)   # the same seed, drawn
        self.assertNotIn('my_civ', drawn)


class SmoothCheck(unittest.TestCase):
    def log(self, smooth):
        """A unit that walks 6 px a step (50 ms), drawn every 10 ms; a tree that stands."""
        lines = []
        for frame in range(60):
            ms = 1000 + 10 * frame
            step = frame // 5
            sx = 100 + 6 * step
            f = (frame % 5) / 5.0
            ox = round((f - 1) * 6) if smooth and step else 0
            lines.append('%d %d %d 0A000000 7 %.4f 10.0000 %d 50 %d 0' % (ms, step, 1000 + 50 * step, 1 + 0.1 * step, sx, ox))
            lines.append('%d %d %d 0B000000 8 5.0000 5.0000 300 80 0 0' % (ms, step, 1000 + 50 * step))
        return '\n'.join(lines) + '\nnot a draw line\n'

    def analyse(self, smooth):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'draw.log')
            open(p, 'w').write(self.log(smooth))
            return smooth_check.analyse(p)

    def test_plain_and_smooth(self):
        plain = self.analyse(False)
        self.assertEqual(len(plain), 1)          # the tree never moved
        m = plain[0]
        self.assertEqual((m['id'], m['steps']), (7, 11))
        self.assertEqual(m['plain_max'], 6.0)
        self.assertGreater(m['plain_still'], 0.7)
        self.assertFalse(m['smoothed'])
        m = self.analyse(True)[0]
        self.assertTrue(m['smoothed'])
        self.assertLessEqual(m['smooth_max'], 2.0)
        self.assertLess(m['smooth_still'], m['plain_still'])


class SoakPlans(unittest.TestCase):
    """The campaign, in-game Load and --screen scripts."""

    def args(self, **kw):
        a = dict(campaign='', first=1, last=None, unlocked=False, minutes=5, speed='fast', save=True,
                 screen=None)
        a.update(kw)
        return argparse.Namespace(**a)

    def test_missions_pass_through(self):
        s = soak.Script()
        end, what = soak.plan_missions(s, BASE, random.Random(1), 'o', self.args(campaign='Boss Nass', first=3), 'g', 7, 20)
        self.assertEqual(what['missions'], [3, 4, 5, 6, 7])
        self.assertEqual(what['passed_through'], [1, 2])
        text = s.text()
        boxes = BASE.missions['Boss Nass']
        clicks = [ln.split(':', 1)[1] for ln in text.splitlines()]
        for xy in boxes:                                   # every box, in order
            self.assertIn('click:%d:%d' % xy, clicks)
        order = [clicks.index('click:%d:%d' % xy) for xy in boxes]
        self.assertEqual(order, sorted(order))
        self.assertEqual(text.count('score_m'), 2 * 7)                # each mission's score screen, two shots
        self.assertEqual(sum(1 for c in clicks if c == 'click:%d:%d' % screens.CAMPAIGN_OK), 1)   # mission 1 only
        self.assertEqual(sum(1 for c in clicks if c == 'click:%d:%d' % screens.SCORE_MENU), 7)
        self.assertGreater(end, 5 * 5 * 60)

    def test_missions_unlocked(self):
        s = soak.Script()
        _, what = soak.plan_missions(s, BASE, random.Random(1), 'o', self.args(campaign='OOM-9', first=6, unlocked=True),
                                     'g', 7, 20)
        self.assertEqual((what['missions'], what['passed_through']), ([6, 7], []))
        self.assertNotIn('click:%d:%d' % screens.CAMPAIGN_OK, s.text())

    def test_republic_after_confederacy(self):
        s = soak.Script()
        _, what = soak.plan_missions(s, CC, random.Random(1), 'o', self.args(campaign='Galactic Republic', first=2),
                                     'g', 7, 20)
        self.assertEqual(what['passed_through'], ['Confederacy %d' % m for m in range(1, 8)] + [1])
        self.assertEqual(what['missions'], list(range(2, 8)))
        text = s.text()
        self.assertLess(text.index('click:%d:%d' % CC.campaigns['Confederacy']),
                        text.index('click:%d:%d' % CC.campaigns['Galactic Republic']))
        self.assertEqual(runner.soak_missions(['--mode', 'missions', '--campaign', 'Galactic Republic',
                                               '--first', '2']), (6, 8))

    def test_screen_1024_plays_at_1024(self):
        # --screen 1024: the size picked first, then every in-game click on the 1024 layout
        lay = screens.layout('base', 1024)
        s = soak.Script()
        t0 = screens.set_screen(s, 12, 1024)
        soak.plan_play(s, lay, random.Random(4), 'o', self.args(mode='monkey', idle_campaign=False, my_civ=None, civs=[]),
                       'monkey', 9, t0)
        clicks = [w for _, w in s.steps if w.startswith('click:')]
        self.assertEqual(clicks[1], 'click:%d:%d' % screens.SCREEN_RADIO[1024])
        self.assertIn('click:%d:%d' % lay.hud['menu'], clicks)
        self.assertNotIn('click:%d:%d' % BASE.hud['menu'], clicks)
        self.assertIn('click:%d:%d' % lay.save_ok, clicks)

    def test_ingame_load(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'SaveGame'))
            for n in ('aaa.gax', 'zzz.gax', 'other.ga1'):
                open(os.path.join(d, 'SaveGame', n), 'w').close()
            s = soak.Script()
            end, what = soak.plan_ingame_load(s, BASE, random.Random(2), 'o', self.args(minutes=10), d, 9, 20)
        self.assertEqual(what['save'], 'soak9')
        self.assertEqual(what['save_row'], 1)                     # aaa, soak9, zzz
        text = s.text()
        load = text.index('click:%d:%d' % BASE.game_menu['load'])
        self.assertLess(text.index('click:%d:%d' % BASE.game_menu['save']), load)
        self.assertLess(load, text.index('click:%d:%d' % BASE.yes))
        self.assertIn('click:300:%d' % (BASE.load_list[1] + BASE.load_list[2]), text)

    def test_monkey_leaves_room_for_a_save(self):
        # Its panels and game menu end with Escapes 5-6 s later: none may land on a save at t1.
        for seed in range(200):
            s = soak.Script()
            soak.monkey(s, BASE, random.Random(seed), 0, 100, 'o')
            late = [st for st in s.steps if st[0] > 100 and not st[1].startswith('shot=')]
            self.assertTrue(all(t < 102.1 for t, _ in late), (seed, late))

    def test_saved_row_counts_recorded_games(self):
        # Clone Campaigns' list: its saves and its recorded games, alphabetical.
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'SaveGame'))
            for n in ('eh.ga1', 'recorded game -  21-Jan-2023.mg1', 'zz.gax', 'b.mgx'):
                open(os.path.join(d, 'SaveGame', n), 'w').close()
            self.assertEqual(screens.saved_row(CC, d, 'soak9'), 2)


class LiftCheck(unittest.TestCase):
    def write_gen(self, d, bodies):
        os.makedirs(d, exist_ok=True)
        src = '#include "recomp_types.h"\n\n' + ''.join('void %s(void) {\n%s\n}\n\n' % (n, b) for n, b in bodies)
        open(os.path.join(d, 'recomp_0000.c'), 'w').write(src)
        open(os.path.join(d, 'recomp_dispatch.c'), 'w').write(
            'const x[] = {\n' + ''.join('    { 0x%su, %s },\n' % (n[4:], n) for n, _ in bodies) + '};\n')

    def test_identical_changed_new(self):
        with tempfile.TemporaryDirectory() as d:
            gen, base, stats = os.path.join(d, 'gen'), os.path.join(d, 'b.json'), os.path.join(d, 's.json')
            json.dump({'lifted': 2, 'errors': 0, 'stubs': 0}, open(stats, 'w'))
            self.write_gen(gen, [('sub_00401000', '    eax = 1;'), ('sub_00401010', '    eax = 2;')])
            rep = lift_check.check(gen, stats, base)
            self.assertTrue(rep['ok'])
            self.assertTrue(os.path.exists(base))                 # recorded on the first run
            rep = lift_check.check(gen, stats, base)
            self.assertTrue(rep['ok'])
            self.assertEqual(rep['identical'], 2)
            self.write_gen(gen, [('sub_00401000', '    eax = 1;'), ('sub_00401010', '    eax = 3;'),
                                 ('sub_00401020', '    eax = 4;')])
            rep = lift_check.check(gen, stats, base)
            self.assertFalse(rep['ok'])
            self.assertEqual((rep['identical'], rep['changed'], rep['new'], rep['gone']), (1, 1, 1, 0))
            self.write_gen(gen, [('sub_00401000', '    eax = 1;')])
            rep = lift_check.check(gen, stats, base)
            self.assertFalse(rep['ok'])
            self.assertEqual(rep['gone'], 1)

    def test_runner_lift_new(self):
        """test.py's lift check: no baseline is NEW, its hashes only in the check's folder;
        --update-baselines records them, and the next run compares."""
        with tempfile.TemporaryDirectory() as d:
            self.write_gen(os.path.join(d, 'src', 'recomp', 'gen'), [('sub_00401000', '    eax = 1;')])

            class A:
                main, update_baselines = d, False

            class C:
                a, baselines = A(), os.path.join(d, 'base')
            check = runner.Check('lift/base', 'smoke', None, doc='x')
            fn = runner.lift_check_fn('base')
            r = runner.Result(check, os.path.join(d, 'results'))
            fn(C(), r)
            self.assertEqual(r.status, 'NEW')
            self.assertFalse(os.path.exists(C.baselines))
            self.assertTrue(os.path.exists(os.path.join(r.dir, 'new-baselines', 'lift', 'base.json')))
            A.update_baselines = True
            fn(C(), runner.Result(check, os.path.join(d, 'results')))
            self.assertTrue(os.path.exists(os.path.join(C.baselines, 'lift', 'base.json')))
            A.update_baselines = False
            r = runner.Result(check, os.path.join(d, 'results'))
            fn(C(), r)
            self.assertEqual((r.status, r.metrics['identical']), ('PASS', 1))

    def test_errors_fail(self):
        with tempfile.TemporaryDirectory() as d:
            gen, stats = os.path.join(d, 'gen'), os.path.join(d, 's.json')
            self.write_gen(gen, [('sub_00401000', '    eax = 1;')])
            json.dump({'lifted': 1, 'errors': 3, 'stubs': 0}, open(stats, 'w'))
            rep = lift_check.check(gen, stats, None)
            self.assertFalse(rep['ok'])

    def test_one_line_body(self):
        """the host's functions (run_lift.py HLE) are one line: void sub_X(void) { swgb_inflate(); }"""
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, 'recomp_0001.c'), 'w').write('void sub_0062343B(void) { swgb_inflate(); }\n')
            open(os.path.join(d, 'recomp_dispatch.c'), 'w').write('{ 0x0062343Bu, sub_0062343B },\n')
            funcs, dispatch, _ = lift_check.scan(d)
            self.assertEqual(list(funcs), ['sub_0062343B'])
            self.assertEqual(dispatch, ['sub_0062343B'])


class ResCheck(unittest.TestCase):
    def _shots(self, w, h):
        import numpy as np
        before = np.full((h, w), 90, dtype=np.int16)       # the map
        before[h - 150:, :] = 60                            # the HUD
        return before

    def test_popup_box(self):
        before = self._shots(1920, 1080)
        after = before.copy()
        after[390:690, 810:1110] = 10                       # a dialog in the middle
        after[1000:1020, 1540:1560] = 200                   # the button's highlight
        after[100:104, 300:304] = 0                         # a unit walking
        self.assertEqual(res_check.popup_box(before, after), (810, 390, 1109, 689))
        self.assertIsNone(res_check.popup_box(before, before))
        rep = res_check.Report(quiet=True)
        self.assertTrue(res_check.check_centred(rep, before, after, 'dialog'))
        off = before.copy()
        off[150:450, 250:550] = 10                          # where an 800x600 layout put it
        self.assertFalse(res_check.check_centred(rep, before, off, 'dialog at 800x600'))
        self.assertFalse(res_check.check_centred(rep, before, before, 'nothing opened'))
        self.assertEqual(rep.fails, 2)

    def test_full_screen(self):
        import numpy as np
        rep = res_check.Report(quiet=True)
        good = np.zeros((1080, 1920), dtype=np.int16)
        good[28:1052, 320:1600] = 40                        # 1280x1024, centred
        self.assertTrue(res_check.check_full_screen(rep, good, 'save'))
        bad = np.zeros((1080, 1920), dtype=np.int16)
        bad[0:1024, 0:1280] = 40                            # the art at 0,0
        self.assertFalse(res_check.check_full_screen(rep, bad, 'save at 0,0'))
        own = np.full((768, 1024), 40, dtype=np.int16)      # the game's own size: no margins
        self.assertTrue(res_check.check_full_screen(rep, own, 'save at 1024x768'))
        self.assertEqual(rep.fails, 1)

    def test_layout(self):
        import numpy as np
        rep = res_check.Report(quiet=True)
        first = np.zeros((1440, 2560), dtype=np.int16)
        first[1190:, 640:1920] = np.arange(1280) % 200 + 20  # the interface at the bottom, centred
        loaded = first.copy()
        loaded[1300:1400, 1400:1700] = 0                    # another minimap, other counters
        self.assertTrue(res_check.check_layout(rep, first, loaded, 'loaded'))
        corner = np.full((1440, 2560), 210, dtype=np.int16)  # uncleared memory
        corner[:600, :800] = 0
        corner[430:600, :800] = 90                          # the 800 layout in the corner
        self.assertFalse(res_check.check_layout(rep, first, corner, 'the 800 layout'))
        self.assertFalse(res_check.check_layout(rep, first, None, 'no shot'))
        self.assertEqual(rep.fails, 2)


class Runner(unittest.TestCase):
    def test_fps_figures(self):
        rows = ['[fps] %6.1fs mode %d cpu %3.0f%% presents %.0f/s changed %.0f/s | world 1 ms' % (t, 4 if t > 5 else 0, c, p, 30)
                for t, c, p in [(1, 90, 5), (3, 80, 5), (6, 12, 60), (30, 8, 61), (31, 9, 59), (32, 10, 60), (33, 30, 60)]]
        f = runner.fps_figures('\n'.join(rows), settle=20)
        self.assertEqual(f['seconds'], 4)
        self.assertEqual(f['presents_median'], 60)
        self.assertEqual(f['cpu_median'], 9.5)
        self.assertIsNone(runner.fps_figures('nothing'))

    def test_compare(self):
        from PIL import Image

        class A:
            update_baselines = False

        class C:
            a = A()

        with tempfile.TemporaryDirectory() as d:
            ctx = C()
            ctx.baselines = os.path.join(d, 'base')
            png = os.path.join(d, 'x.png')
            Image.new('L', (100, 100), 50).save(png)
            r = runner.Result(runner.Check('skirmish/k', 'smoke', None, game='base', doc='x'), d)
            # no baseline: NEW, not compared, and the baselines left alone (only --update-baselines writes them)
            self.assertIsNone(runner.compare(ctx, r, png, 'k/menu'))
            self.assertEqual(r.status, 'NEW')
            self.assertFalse(os.path.exists(ctx.baselines))
            self.assertTrue(os.path.exists(os.path.join(r.dir, 'new-baselines', 'k', 'menu.png')))
            ctx.a.update_baselines = True
            self.assertIsNone(runner.compare(ctx, r, png, 'k/menu'))
            self.assertTrue(os.path.exists(os.path.join(d, 'base', 'k', 'menu.png')))
            ctx.a.update_baselines = False
            r = runner.Result(r.check, d)
            im = Image.new('L', (100, 100), 50)
            for x in range(10):                                            # 1% differs
                im.putpixel((x, 0), 200)
            im.save(png)
            self.assertAlmostEqual(runner.compare(ctx, r, png, 'k/menu', tol=0.02), 0.001)
            self.assertEqual(r.status, 'PASS')
            for x in range(100):
                for y in range(5):
                    im.putpixel((x, y), 200)
            im.save(png)
            runner.compare(ctx, r, png, 'k/menu', tol=0.02)
            self.assertEqual(r.status, 'FAIL')
            r.status = 'PASS'
            self.assertEqual(runner.compare(ctx, r, png, 'k/menu', tol=0.02, region=(0, 50, 100, 100)), 0.0)
            self.assertEqual(r.status, 'PASS')

    def test_retry_keeps_both_tries(self):
        """--retry: the retry runs in <check>-retry1, so the first try's run.log and shots stay;
        the report has both tries' lines and shots, each under its folder."""
        with tempfile.TemporaryDirectory() as d:
            class A:
                jobs = 2

            class C:
                a, cancelled, results = A(), False, d

            def fake(passes_on):
                def run(c, attempt):
                    r = runner.Result(c, d, attempt)
                    os.makedirs(r.dir, exist_ok=True)
                    with open(os.path.join(r.dir, 'run.log'), 'w') as f:
                        f.write('try %d' % (attempt + 1))
                    with open(os.path.join(r.dir, 'map.png'), 'wb') as f:
                        f.write(b'png')
                    r.image(os.path.join(r.dir, 'map.png'), 'map')
                    if attempt < passes_on:
                        runner.expect_exit(r, -1)            # timed out: what a retry may excuse
                    return r
                return run

            def text(*p):
                with open(os.path.join(d, *p)) as f:
                    return f.read()

            flaky = runner.Check('skirmish/base', 'functional', None, game='base', doc='x')
            solid = runner.Check('films/base', 'functional', None, game='base', doc='x')
            run = fake(passes_on=1)
            results = [run(flaky, 0), fake(0)(solid, 0)]
            runner.retry_failed(C(), results, 1, run)
            r = results[0]
            self.assertEqual((r.status, results[1].status), ('FLAKY', 'PASS'))
            self.assertEqual(text('skirmish_base', 'run.log'), 'try 1')
            self.assertEqual(text('skirmish_base-retry1', 'run.log'), 'try 2')
            self.assertEqual(r.lines[0], 'try 1: FAIL (timing), in skirmish_base/')
            self.assertIn('try 2: PASS, in skirmish_base-retry1/', r.lines)
            self.assertEqual(r.images, [('skirmish_base/map.png', 'try 1: map'),
                                        ('skirmish_base-retry1/map.png', 'try 2: map')])
            self.assertEqual([t['dir'] for t in r.as_dict()['tries']], ['skirmish_base', 'skirmish_base-retry1'])
            self.assertFalse(os.path.exists(os.path.join(d, 'films_base-retry1')))   # a pass is not retried

            # failing every time: --retry 2 keeps three folders, and it stays a FAIL
            run = fake(passes_on=9)
            results = [run(runner.Check('res/1024', 'functional', None, game='base', doc='x'), 0)]
            runner.retry_failed(C(), results, 2, run)
            r = results[0]
            self.assertEqual(r.status, 'FAIL')
            self.assertEqual([t['try'] for t in r.as_dict()['tries']], [1, 2, 3])
            self.assertEqual(sum(l.startswith('try ') for l in r.lines), 3)
            for k, sub in enumerate(['res_1024', 'res_1024-retry1', 'res_1024-retry2']):
                self.assertEqual(text(sub, 'run.log'), 'try %d' % (k + 1))

    def retried(self, *tries):
        """A check run once per entry of tries (a function that fails its Result, or None for a
        pass) under --retry len(tries) - 1: its final Result, and how many times it ran."""
        with tempfile.TemporaryDirectory() as d:
            class A:
                jobs = 1

            class C:
                a, cancelled, results = A(), False, d

            ran = []

            def run(c, attempt):
                r = runner.Result(c, d, attempt)
                ran.append(attempt)
                if tries[attempt]:
                    tries[attempt](r)
                return r
            results = [run(runner.Check('films/base', 'functional', None, game='base', doc='x'), 0)]
            runner.retry_failed(C(), results, len(tries) - 1, run)
            return results[0], len(ran)

    def test_retry_timing_only_is_flaky(self):
        """Only a try that failed on time alone (killed past its time, the watchdog) is retried into FLAKY."""
        timed_out = lambda r: runner.expect_exit(r, -1)
        watchdog = lambda r: runner.expect_exit(r, 4, want=(0,))
        for why in (timed_out, watchdog):
            r, n = self.retried(why, None)
            self.assertEqual((r.status, n), ('FLAKY', 2))
        r, n = self.retried(timed_out, watchdog, None)
        self.assertEqual((r.status, n), ('FLAKY', 3))
        self.assertEqual([t['hard'] for t in r.as_dict()['tries']], [False, False, False])

    def test_retry_hard_failure_stays_fail(self):
        """A wrong picture, a fault exit, an unresolved call or any other failure fails the run
        even when the retry passes; it is not retried again once a try passed."""
        fault = lambda r: runner.expect_exit(r, 3)
        icall = lambda r: runner.events(r, 'ICALL: unresolved 0x00401000\n')
        crash_line = lambda r: runner.events(r, '=== fault 0xC0000005 at 0x75B773FE, thread 1 ===\n')
        picture = lambda r: r.fail('k/menu: 9.0% of the picture differs from the baseline (at most 2.0%)')
        for why in (fault, icall, crash_line, picture):
            r, n = self.retried(why, None, None)
            self.assertEqual((r.status, n), ('FAIL', 2))
            self.assertTrue(r.lines[0].startswith('FAIL: try 2 passed, but try 1 failed for more than time'), r.lines[0])
            self.assertIn('try 1: FAIL, in films_base/', r.lines)
        # a timeout with a fault in the same try is a crash, not timing
        r, _ = self.retried(lambda r: (runner.expect_exit(r, -1), runner.events(r, '=== fault 0xC0000005 at 0x1 ===')), None)
        self.assertEqual(r.status, 'FAIL')
        # timing first, then a hard failure, then a pass: FAIL
        r, n = self.retried(lambda r: runner.expect_exit(r, -1), fault, None)
        self.assertEqual((r.status, n), ('FAIL', 3))
        self.assertIn('try 2', r.lines[0])

    def test_report_new_and_flaky(self):
        """A NEW check fails functional and up (and the merge alias), not smoke, nor under
        --update-baselines; NEW and FLAKY checks are listed above the table."""
        self.assertEqual(runner.ALIASES['merge'], 'functional')
        with tempfile.TemporaryDirectory() as d:
            def run_report(level, update=False):
                class A:
                    update_baselines = update

                class C:
                    a, results = A(), d
                new = runner.Result(runner.Check('skirmish/base', level, None, game='base', doc='x'), d)
                new.new('base/menu.png: no baseline, not compared')
                flaky, n = self.retried(lambda r: runner.expect_exit(r, -1), None)
                plain = runner.Result(runner.Check('unit/keys', 'unit', None, doc='x'), d)
                meta = {'level': level, 'started': 'now', 'host': 'h', 'seconds': 60, 'jobs': 1, 'build': 'b',
                        'commit': 'c', 'dirty': False}
                ok = runner.report(C(), [plain, new, flaky], meta)
                with open(os.path.join(d, 'report.md'), encoding='utf-8') as f:
                    return ok, f.read(), meta
            for level in ('functional', 'soak', 'real'):
                self.assertFalse(run_report(level)[0], level)
            ok, md, meta = run_report('smoke')
            self.assertTrue(ok)
            self.assertEqual(meta['counts']['NEW'], 1)
            self.assertTrue(run_report('functional', update=True)[0])
            ok, md, _ = run_report('functional')
            table = md.index('| Check |')
            self.assertLess(md.index('## NEW: '), table)
            self.assertLess(md.index('## FLAKY: '), table)
            self.assertLess(md.index('- **skirmish/base**: base/menu.png: no baseline'), table)
            self.assertLess(md.index('- **films/base**: run: exit -1 (timed out)'), table)
            with open(os.path.join(d, 'report.html'), encoding='utf-8') as f:
                page = f.read()
            self.assertLess(page.index('<div class="top"><h2>NEW: '), page.index('<table>'))

    def test_hud_sides(self):
        import numpy as np
        from PIL import Image
        self.assertEqual([runner.side_column(d, 3) for d in range(8)], [0, 1, 2, 2, 1, 0, 0, 1])
        self.assertEqual(runner.hud_band(1920, 1080), (1280, 250))
        self.assertEqual(runner.hud_band(1152, 864), (1024, 200))
        self.assertEqual(runner.hud_band(1024, 600), (800, 170))
        W, H, bw, bh = 1920, 1080, 1280, 250
        dx, band = (W - bw) // 2, 12
        rng = np.random.default_rng(1)
        a = np.zeros((H, W, 3), np.uint8)
        a[H - bh:, dx:dx + bw] = rng.integers(30, 255, (bh, bw, 3), dtype=np.uint8)   # the art
        with tempfile.TemporaryDirectory() as d:
            png = os.path.join(d, 'm.png')
            Image.fromarray(a).save(png)
            f = runner.hud_sides(png)                       # black sides
            self.assertEqual(f['sides_black'], 1.0)
            self.assertLess(f['sides_mirror'], 0.01)
            for k in range(dx):                             # as widescreen.c fills them
                a[H - bh:, dx - 1 - k] = a[H - bh:, dx + runner.side_column(k, band)]
                a[H - bh:, dx + bw + k] = a[H - bh:, dx + bw - 1 - runner.side_column(k, band)]
            Image.fromarray(a).save(png)
            f = runner.hud_sides(png)
            self.assertEqual(f['sides_mirror'], 1.0)
            self.assertEqual(f['sides_black'], 0.0)
            self.assertGreater(f['sides_mean'], 20)
            a[H - bh:, :dx] = np.roll(a[H - bh:, :dx], 5, axis=1)   # the art 5 columns off
            Image.fromarray(a).save(png)
            self.assertLess(runner.hud_sides(png)['sides_mirror'], 0.9)
            with Image.open(runner.sides_crop(png, os.path.join(d, 's.png'))) as c:
                self.assertEqual(c.size, (W - bw, bh))

    def test_view_marker_moved(self):
        from PIL import Image, ImageDraw
        with tempfile.TemporaryDirectory() as d:
            shots = []
            for i, x in enumerate((100, 300)):
                im = Image.new('RGB', (400, 200), (20, 20, 20))
                dr = ImageDraw.Draw(im)
                dr.rectangle((5, 5, 30, 30), outline=(255, 255, 255))      # the art's own white: in both
                dr.rectangle((x - 25, 80, x + 25, 110), outline=(255, 255, 255))
                shots.append(os.path.join(d, '%d.png' % i))
                im.save(shots[-1])
            xa, xb = runner.view_marker_moved(shots[0], shots[1], 0, 0, 400, 200)
            self.assertAlmostEqual(xa, 100, delta=1)
            self.assertAlmostEqual(xb, 300, delta=1)
            self.assertEqual(runner.view_marker_moved(shots[0], shots[0], 0, 0, 400, 200), (None, None))

    def test_layout_text(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'dump.txt')
            with open(p, 'w') as f:
                f.write('08A50A88 vt 0065CE24  0,0 1920x1080\n  08A8F070 vt 00665088  320,830 240x250\n')
            self.assertEqual(runner.layout_text(p), 'vt 0065CE24  0,0 1920x1080\n  vt 00665088  320,830 240x250\n')
            self.assertIsNone(runner.layout_text(os.path.join(d, 'none.txt')))

    def test_levels_cumulative(self):
        class A:
            just = False
            with_lower = False
            cc = None
            only = []

        a = A()
        a.level = 'unit'
        unit = runner.select(a)
        self.assertTrue(unit and all(c.level == 'unit' for c in unit))
        a.level = 'functional'
        names = {c.name for c in runner.select(a)}
        self.assertTrue({'unit/scaler', 'conformance/base', 'present/A'} <= names)
        self.assertFalse(any(n.startswith(('soak/', 'real/')) for n in names))
        a.level = 'real'
        self.assertEqual([c.name for c in runner.select(a)], ['real/present'])
        a.level, a.cc = 'smoke', False
        self.assertFalse(any(c.game == 'cc' for c in runner.select(a)))

    def test_skirmish_and_civ_steps_pick_800(self):
        for steps, _ in (runner.skirmish_steps('base', 'o'),
                         runner.civ_skirmish('base', 'Galactic Empire', ['Gungans'], 'o', 2, 's')):
            clicks = [w for _, w in sorted(steps) if w.startswith('click:')]
            self.assertLess(clicks.index('click:%d:%d' % screens.SCREEN_RADIO[800]),
                            clicks.index('click:%d:%d' % screens.MAIN_SINGLE))


class PerfAB(unittest.TestCase):
    """tools/perf_ab.py (two builds, the same games) and mp_pair.py's sync-record compare."""

    def test_figures(self):
        rows = ['[fps] %6.1fs mode 4 cpu %3.0f%% presents %.0f/s changed 9/s | world 1 ms +1500/s speed 1.50 steps %.1f/s of 77.0 ms (max 80)'
                % (t, c, p, 19.0 + (t % 2)) for t, c, p in [(5, 99, 10), (20, 50, 100), (21, 60, 200), (22, 40, 100), (90, 99, 1)]]
        rows.append('[present] 1.0 s: 100 presents, 60 shown (180 Hz, sharp); by the timer: 0 held')
        f = perf_ab.figures('\n'.join(rows), 20, 30)
        self.assertEqual(f['seconds'], 3)
        self.assertEqual(f['cpu'], 50)
        self.assertEqual(f['drawn'], 100)
        self.assertAlmostEqual(f['ms_frame'], 150 * 10 / 400)   # CPU over the window / frames drawn
        self.assertEqual(f['steps'], 19)        # 19, 20, 19
        self.assertEqual(f['shown'], 60)
        self.assertIsNone(perf_ab.figures('nothing', 0, 10))

    def test_ratio_by_round(self):
        run = lambda ms, rc=4: {'ms_frame': ms, 'cpu': 50, 'exit': rc, 'events': 0}   # noqa: E731
        res = {('O1', 'late'): [run(4.0), run(3.0), run(2.0)],
               ('O2', 'late'): [run(3.6), run(3.0), run(1.0, rc=3)]}
        t = perf_ab.summarize(res)['late']
        self.assertEqual(t['O2']['ratio'][:2], [0.95, 0.9])     # rounds 0, 1; the fault's round left out
        self.assertEqual(t['O2']['bad'], 1)
        self.assertNotIn('ratio', t['O1'])
        self.assertIn('ms/frame vs O1', perf_ab.report(perf_ab.summarize(res), ['O1', 'O2']))

    def test_scenarios_set_their_screen_size(self):
        with tempfile.TemporaryDirectory() as d:
            for name, (plan, extra, size) in perf_ab.SCENARIOS.items():
                s, t0, t1 = plan('base', d, size)
                clicks = [w for _, w in sorted(s.steps) if w.startswith('click')]
                self.assertLess(t0, t1, name)
                if size is None:
                    continue
                # a game folder remembers Screen Size: each game picks its own before Single Player
                self.assertEqual(clicks[:3], ['click:%d:%d' % xy for xy in (
                    screens.MAIN_OPTIONS, screens.SCREEN_RADIO[size], screens.MAIN_SINGLE)], name)
                self.assertEqual(size == 1280, '--res' in extra, name)

    def test_mp_sync_compare(self):
        rec = lambda p, turn, ck: (0, (p, 1000 * turn, turn, 1000 * turn, ck, 7, 0, 5, 9))   # noqa: E731
        a = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 12), rec(2, 80, 12)]
        shared, bad = mp_pair.compare(a, list(a))
        self.assertEqual((len(shared), bad), (4, []))
        b = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 13), rec(2, 80, 13)]   # B's builds drift at turn 80
        shared, bad = mp_pair.compare(a, b)
        self.assertEqual(sorted(k for k, _, _ in bad), [(1, 80), (2, 80)])


MP_LOG = """[dplay] CreatePlayer "michael" -> 0001D2A1 (flags 0, event 00000000)
[dplay] player 0001D2A2 (michael) joined (45.9 s)
[sync]  1: 00000002 00001167 0000000A 00001167 0A51D3CA 00009882 00000000 00060015 00005A41
[sync]  2: 00000001 00001167 0000000A 00001167 0A51D3CA 00009882 00000000 00060015 00005A41
[mp] chat from player 3 (0001D2A2) to 0001D2A1 (YYYNNNNN): "mpchatc1"
[dplay] client 1 (127.0.0.1) gone (153.4 s)
[dplay] player 0001D2A2 (michael) left (153.4 s)
ICALL: unresolved VA 0x005648A0 from 0x00406E20
[dplay] DestroyPlayer 0001D2A1 (200.5 s)
[dplay] Close (client) (200.6 s)
[dplay] the host is gone: session lost (201.0 s)
"""


class MpPair(unittest.TestCase):
    def setUp(self):
        import mp_pair
        self.mp = mp_pair

    def test_check(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'run.log')
            with open(p, 'w') as f:
                f.write(MP_LOG)
            ev, chats, me, deps, left, started = self.mp.check(p)
        self.assertEqual(len(ev), 1)
        self.assertEqual(chats, {'mpchatc1': 3})
        self.assertEqual(me, '0001D2A1')
        self.assertEqual([(t, d_) for t, _, d_ in deps], [(153.4, '0001D2A2'), (201.0, 'host')])
        self.assertEqual(left, 200.5)
        self.assertTrue(started)

    def test_compare_all(self):
        r = [(1, (2, 4455, 10, 4455, 5, 6, 0, 7, 8)), (2, (1, 4455, 10, 4455, 5, 6, 0, 7, 8))]
        bad_c = [(1, (2, 4455, 10, 4455, 5, 6, 0, 7, 9))]
        shared, bad = self.mp.compare_all({'a': r, 'b': r, 'c': bad_c})
        self.assertEqual(shared, [(1, 10), (2, 10)])
        self.assertEqual(len(bad), 2)        # c against a, and against b
        shared, bad = self.mp.compare_all({'a': r, 'b': r})
        self.assertEqual(bad, [])
        _, bad = self.mp.compare_all({'a': r + [(3, (3, 4455, 10, 4455, 5, 6, 0, 7, 1))]})
        self.assertEqual(len(bad), 1)        # within one log: the players' records for a turn

    def test_plans(self):
        import argparse
        import random
        a = argparse.Namespace(players=3, minutes=4, activity='busy', chat=True, save=1.0, quit=None, resign=None,
                               drop=('c', 2.0))
        chats = self.mp.chat_times(3, 4)
        self.assertEqual(len(chats), 6)
        self.assertEqual([w for _, w, _ in chats], list('abcabc'))
        plan = self.mp.game_plan(random.Random(1), BASE, 'b', 1, 3, a, 'out')
        self.assertEqual(plan, sorted(plan, key=lambda s: s[0]))
        steps = [s for _, s in plan]
        self.assertTrue(any('drop_after' in st for st in steps))
        self.assertEqual([s for t, s in plan if 118 <= t <= 195 and s.startswith(('click', 'rclick'))], [])   # c drops: b only votes
        t_chat = next(t for t, s in plan if s == 'enter')
        near = [s for t, s in plan if t_chat - 2 <= t <= t_chat + 5 and s.startswith(('click', 'rclick'))]
        self.assertEqual(near, [])          # no clicks while typing
        plan_c = self.mp.game_plan(random.Random(1), BASE, 'c', 2, 3, a, 'out')
        self.assertFalse(any('drop_after' in s for _, s in plan_c))
        text, wd = self.mp.script(BASE, 'b', 1, 3, a, 'out')
        self.assertTrue(text.startswith('on:%s\n' % self.mp.ON_MENU))
        self.assertLess(text.index('click:%d:%d' % screens.SCREEN_RADIO[800]),
                        text.index('click:%d:%d' % BASE.main_multi))
        self.assertIn('click:%d:%d' % BASE.main_multi, text)
        self.assertIn('click:%d:%d' % CC.main_multi, self.mp.script(CC, 'b', 1, 3, a, 'out')[0])
        _, wd_a = self.mp.script(BASE, 'a', 0, 3, a, 'out')
        self.assertEqual(wd_a - wd, self.mp.B_DELAY + self.mp.STEP)

    def test_per_player_options(self):
        mp = self.mp
        self.assertEqual(mp.run_arg('b=X/o2.exe'), ('b', 'X/o2.exe'))
        self.assertEqual(mp.run_arg('build/swgbrecomp.exe'), ('all', 'build/swgbrecomp.exe'))
        with self.assertRaises(argparse.ArgumentTypeError):
            mp.who_arg('e=--smooth')
        runs = mp.per_player([('b', 'o2'), ('all', 'o1')], 3, lambda old, new: new)
        self.assertEqual(runs, {'a': 'o1', 'b': 'o2', 'c': 'o1'})      # a player's own beats `all`
        self.assertEqual(mp.per_player([('all', 'default'), ('b', 'o2')], 3, lambda old, new: new),
                         {'a': 'default', 'b': 'o2', 'c': 'default'})
        import shlex
        args = mp.per_player([('a', shlex.split('--smooth --fps 120')), ('all', ['--mp-cross']), ('all', [])], 2,
                             lambda old, new: old + new)
        self.assertEqual(args, {'a': ['--mp-cross', '--smooth', '--fps', '120'], 'b': ['--mp-cross']})

    def test_chat_shown(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('no PIL')
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x.png')
            im = Image.new('RGB', (800, 600), (175, 126, 36))
            for x in range(10, 100):
                for y in range(160, 165):
                    im.putpixel((x, y), self.mp.CHAT_RGB[2])
            im.save(p)
            self.assertTrue(self.mp.chat_shown(p, 2))
            self.assertFalse(self.mp.chat_shown(p, 3))
            self.assertFalse(self.mp.chat_shown(os.path.join(d, 'none.png'), 2))


ENV_READS = [
    re.compile(r'''\b(?:getenv|GetEnvironmentVariable[AW]?)\s*\(\s*L?["']([A-Za-z_][A-Za-z0-9_()]*)'''),
    re.compile(r'''\bos\.(?:environ\.get|getenv)\s*\(\s*["']([A-Za-z_][A-Za-z0-9_()]*)'''),
    re.compile(r'''\bos\.environ\s*\[\s*["']([A-Za-z_][A-Za-z0-9_()]*)'''),
    re.compile(r'''\$\{?env:([A-Za-z_][A-Za-z0-9_()]*)'''),
]


def env_names_read():
    """{name: first file:line} for every environment variable src/ and tools/ read."""
    found = {}
    for top in ('src', 'tools'):
        for d, subdirs, files in os.walk(os.path.join(ROOT, top)):
            subdirs[:] = [s for s in subdirs if s not in ('recomp', '__pycache__')]   # src/recomp: the lift
            for f in files:
                if not f.endswith(('.c', '.h', '.py', '.ps1')):
                    continue
                path = os.path.join(d, f)
                with open(path, encoding='utf-8', errors='replace') as fh:
                    for n, line in enumerate(fh, 1):
                        for rx in ENV_READS:
                            for name in rx.findall(line):
                                found.setdefault(name, '%s:%d' % (os.path.relpath(path, ROOT), n))
    return found


def env_names_documented():
    """The backquoted names in docs/building.md's "Environment variables" section."""
    with open(os.path.join(ROOT, 'docs', 'building.md'), encoding='utf-8') as fh:
        text = fh.read()
    m = re.search(r'^## Environment variables\n(.*?)^## ', text, re.S | re.M)
    if not m:
        raise AssertionError('docs/building.md has no "## Environment variables" section')
    return set(re.findall(r'`([A-Za-z_][A-Za-z0-9_()]*)`', m.group(1)))


class EnvVars(unittest.TestCase):
    def test_every_name_read_is_documented(self):
        read = env_names_read()
        self.assertIn('SWGB_RES_DUMP', read)   # the scan itself still finds a known one
        missing = {k: v for k, v in read.items() if k not in env_names_documented()}
        self.assertEqual(missing, {}, 'add these to docs/building.md, "Environment variables"')

    def test_patterns(self):
        lines = ['if (getenv("SWGB_A")) x();', 'GetEnvironmentVariableA("SWGB_B", v, sizeof v)',
                 "os.environ.get('SWGB_C', '')", "os.environ['SWGB_D']", 'Join-Path ${env:ProgramFiles(x86)} x',
                 '$env:SWGB_E', "env = dict(os.environ, SWGB_F='1')"]
        got = [name for line in lines for rx in ENV_READS for name in rx.findall(line)]
        self.assertEqual(got, ['SWGB_A', 'SWGB_B', 'SWGB_C', 'SWGB_D', 'ProgramFiles(x86)', 'SWGB_E'])


if __name__ == '__main__':
    unittest.main()
