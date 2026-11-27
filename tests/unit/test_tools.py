"""The tools' own tests, no game needed: tools/soak.py's scripts and log reading,
tools/lift_check.py, tools/test.py's image and --fps-log readers, perf_ab.py
and mp_pair.py's sync compare.

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
import mp_pair  # noqa: E402
import perf_ab  # noqa: E402
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
                steps = dict((t, w) for t, w in s.steps)
                self.assertLess(t0, t1, name)
                if size is None:
                    continue
                # a game folder remembers Screen Size: each game picks its own before Single Player
                self.assertEqual(steps[12], 'click:%d:%d' % perf_ab.MAIN_OPTIONS, name)
                self.assertEqual(steps[19], 'click:%d:%d' % perf_ab.SIZE_RADIO[size], name)
                self.assertEqual(size == 'res', '--res' in extra, name)
                self.assertGreater(min(t for t, w in s.steps if w.startswith('click') and t > 19), 19, name)

    def test_mp_sync_compare(self):
        rec = lambda p, turn, ck: (0, (p, 1000 * turn, turn, 1000 * turn, ck, 7, 0, 5, 9))   # noqa: E731
        a = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 12), rec(2, 80, 12)]
        shared, bad = mp_pair.compare(a, list(a))
        self.assertEqual((len(shared), bad), (4, []))
        b = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 13), rec(2, 80, 13)]   # B's builds drift at turn 80
        shared, bad = mp_pair.compare(a, b)
        self.assertEqual(sorted(k for k, _, _ in bad), [(1, 80), (2, 80)])


if __name__ == '__main__':
    unittest.main()
