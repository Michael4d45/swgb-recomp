"""The tools' own tests, no game needed: tools/soak.py's scripts and log reading,
tools/lift_check.py, and tools/test.py's image and --fps-log readers.

    py -3 -m unittest -v tests.unit.test_tools        (tools/test.py --level unit runs it)
"""
import json
import os
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

    def test_pick(self):
        s = soak.Script()
        t = soak.pick(soak.DD_SIZE, 2, s, 10)
        self.assertEqual(t, 12.5)
        (ax, ay), (ix, iy), step = soak.DD_SIZE
        self.assertEqual(s.steps, [(10, 'click:%d:%d' % (ax, ay)), (11.2, 'click:%d:%d' % (ix, iy + 2 * step))])


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
        plan = self.mp.game_plan(random.Random(1), 'b', 1, 3, a, 'out')
        self.assertEqual(plan, sorted(plan, key=lambda s: s[0]))
        steps = [s for _, s in plan]
        self.assertTrue(any('drop_after' in st for st in steps))
        self.assertEqual([s for t, s in plan if 118 <= t <= 195 and s.startswith(('click', 'rclick'))], [])   # c drops: b only votes
        t_chat = next(t for t, s in plan if s == 'enter')
        near = [s for t, s in plan if t_chat - 2 <= t <= t_chat + 5 and s.startswith(('click', 'rclick'))]
        self.assertEqual(near, [])          # no clicks while typing
        plan_c = self.mp.game_plan(random.Random(1), 'c', 2, 3, a, 'out')
        self.assertFalse(any('drop_after' in s for _, s in plan_c))
        text, wd = self.mp.script('b', 1, 3, a, 'out')
        self.assertTrue(text.startswith('on:%s\n' % self.mp.ON_MENU))
        _, wd_a = self.mp.script('a', 0, 3, a, 'out')
        self.assertEqual(wd_a - wd, self.mp.B_DELAY + self.mp.STEP)

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


if __name__ == '__main__':
    unittest.main()
