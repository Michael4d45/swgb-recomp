"""The tools' own tests, no game needed: tools/soak.py's scripts and log reading,
tools/lift_check.py, and tools/test.py's image and --fps-log readers.

    py -3 -m unittest -v tests.unit.test_tools        (tools/test.py --level unit runs it)
"""
import argparse
import importlib
import json
import os
import random
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import lift_check  # noqa: E402
import soak  # noqa: E402
import test as runner  # noqa: E402

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
        t = soak.pick(soak.DD_SIZE, 2, s, 10)
        self.assertEqual(t, 12.5)
        (ax, ay), (ix, iy), step = soak.DD_SIZE
        self.assertEqual(s.steps, [(10, 'click:%d:%d' % (ax, ay)), (11.2, 'click:%d:%d' % (ix, iy + 2 * step))])


class SoakPlans(unittest.TestCase):
    """The campaign, in-game Load and --screen scripts. soak's coordinates are module globals
    that --cc and --screen change: each test starts from a fresh import."""

    def setUp(self):
        global soak
        soak = importlib.reload(soak)

    def tearDown(self):
        global soak
        soak = importlib.reload(soak)

    def args(self, **kw):
        a = dict(campaign='', first=1, last=None, unlocked=False, minutes=5, speed='fast', save=True,
                 screen=None)
        a.update(kw)
        return argparse.Namespace(**a)

    def test_missions_pass_through(self):
        s = soak.Script()
        end, what = soak.plan_missions(s, random.Random(1), 'o', self.args(campaign='Boss Nass', first=3), 'g', 7)
        self.assertEqual(what['missions'], [3, 4, 5, 6, 7])
        self.assertEqual(what['passed_through'], [1, 2])
        text = s.text()
        boxes = soak.MISSION_BOXES['Boss Nass']
        clicks = [ln.split(':', 1)[1] for ln in text.splitlines()]
        for xy in boxes:                                   # every box, in order
            self.assertIn('click:%d:%d' % xy, clicks)
        order = [clicks.index('click:%d:%d' % xy) for xy in boxes]
        self.assertEqual(order, sorted(order))
        self.assertEqual(text.count('score_m'), 2 * 7)                # each mission's score screen, two shots
        self.assertEqual(sum(1 for c in clicks if c == 'click:%d:%d' % soak.CAMPAIGN_OK), 1)   # mission 1 only
        self.assertEqual(sum(1 for c in clicks if c == 'click:%d:%d' % soak.SCORE_MENU), 7)
        self.assertGreater(end, 5 * 5 * 60)

    def test_missions_unlocked(self):
        s = soak.Script()
        _, what = soak.plan_missions(s, random.Random(1), 'o', self.args(campaign='OOM-9', first=6, unlocked=True),
                                     'g', 7)
        self.assertEqual((what['missions'], what['passed_through']), ([6, 7], []))
        self.assertNotIn('click:%d:%d' % soak.CAMPAIGN_OK, s.text())

    def test_republic_after_confederacy(self):
        soak.clone_campaigns()
        s = soak.Script()
        _, what = soak.plan_missions(s, random.Random(1), 'o', self.args(campaign='Galactic Republic', first=2),
                                     'g', 7)
        self.assertEqual(what['passed_through'], ['Confederacy %d' % m for m in range(1, 8)] + [1])
        self.assertEqual(what['missions'], list(range(2, 8)))
        text = s.text()
        self.assertLess(text.index('click:%d:%d' % soak.CAMPAIGNS['Confederacy']),
                        text.index('click:%d:%d' % soak.CAMPAIGNS['Galactic Republic']))
        self.assertEqual(runner.soak_missions(['--mode', 'missions', '--campaign', 'Galactic Republic',
                                               '--first', '2']), (6, 8))

    def test_cc_tables(self):
        soak.clone_campaigns()
        self.assertEqual(sorted(soak.MISSION_BOXES), ['Confederacy', 'Galactic Republic'])
        self.assertEqual(soak.SAVE_EXT, '.ga1')
        for boxes in soak.MISSION_BOXES.values():
            self.assertEqual(boxes[0], soak.CAMPAIGN_MISSION1)

    def test_screen_1024(self):
        soak.use_screen(1024)
        self.assertEqual(soak.GAME_MENU['save'], (520, 302))      # centred: +112, +84
        self.assertEqual(soak.GAME_MENU_LOAD, (520, 335))
        self.assertEqual(soak.LOAD_YES, (417, 412))
        self.assertEqual(soak.SAVE_OK, (227, 727))                # scaled by 1.28
        self.assertEqual(soak.HUD_RIGHT['menu'], (983, 624))
        x0, y0, x1, y1 = soak.MAP_VIEW
        self.assertEqual((x1, y1 < 768 - 200), (1023, True))

    def test_ingame_load(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'SaveGame'))
            for n in ('aaa.gax', 'zzz.gax', 'other.ga1'):
                open(os.path.join(d, 'SaveGame', n), 'w').close()
            s = soak.Script()
            end, what = soak.plan_ingame_load(s, random.Random(2), 'o', self.args(minutes=10), d, 9)
        self.assertEqual(what['save'], 'soak9')
        self.assertEqual(what['save_row'], 1)                     # aaa, soak9, zzz
        text = s.text()
        load = text.index('click:%d:%d' % soak.GAME_MENU_LOAD)
        self.assertLess(text.index('click:%d:%d' % soak.GAME_MENU['save']), load)
        self.assertLess(load, text.index('click:%d:%d' % soak.LOAD_YES))
        self.assertIn('click:300:%d' % (soak.INGAME_LOAD[1] + soak.INGAME_LOAD[2]), text)

    def test_monkey_leaves_room_for_a_save(self):
        # Its panels and game menu end with Escapes 5-6 s later: none may land on a save at t1.
        for seed in range(200):
            s = soak.Script()
            soak.monkey(s, random.Random(seed), 0, 100, 'o')
            late = [st for st in s.steps if st[0] > 100 and not st[1].startswith('shot=')]
            self.assertTrue(all(t < 102.1 for t, _ in late), (seed, late))

    def test_saved_row_counts_recorded_games(self):
        # Clone Campaigns' list: its saves and its recorded games, alphabetical.
        soak.clone_campaigns()
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'SaveGame'))
            for n in ('eh.ga1', 'recorded game -  21-Jan-2023.mg1', 'zz.gax', 'b.mgx'):
                open(os.path.join(d, 'SaveGame', n), 'w').close()
            self.assertEqual(soak.saved_row(d, 'soak9'), 2)

    def test_screen_option_and_check(self):
        s = soak.Script()
        soak.set_screen(s, 12, 1280)
        self.assertEqual(s.steps, [(12, 'click:40:490'), (18, 'click:453:443')])


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

            class R(runner.Result):
                def __init__(self):
                    self.lines, self.metrics, self.images, self.status, self.root = [], {}, [], 'PASS', d

            r = R()
            self.assertEqual(runner.compare(ctx, r, png, 'k/menu'), 0.0)   # recorded
            self.assertTrue(os.path.exists(os.path.join(d, 'base', 'k', 'menu.png')))
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


if __name__ == '__main__':
    unittest.main()
